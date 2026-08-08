#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ShellShortcuts.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#endif

// "Send To" writes a real .lnk through COM, so the only test worth having is
// one that reads the shortcut back and checks where it points. Asserting that
// a file appeared would pass on a zero-byte file.
namespace {

#ifdef Q_OS_WIN
// The target recorded inside a .lnk, or empty if it cannot be read.
//
// Initialises COM itself: the library under test scopes its own initialisation
// to each call, so by the time the test reads the file back there is none in
// this thread, and CoCreateInstance fails. Without this the check silently
// compared two empty strings against the real path and looked like a bug in
// the shortcut rather than in the reader.
QString shortcutTarget(const QString &linkPath) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool owned = SUCCEEDED(init);
    struct Uninit {
        bool owned;
        ~Uninit() {
            if (owned)
                CoUninitialize();
        }
    } uninit{owned};

    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void **>(&link))) ||
        !link)
        return {};
    IPersistFile *file = nullptr;
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file))) || !file) {
        link->Release();
        return {};
    }
    QString target;
    const QString native = QDir::toNativeSeparators(linkPath);
    if (SUCCEEDED(file->Load(reinterpret_cast<const wchar_t *>(native.utf16()), STGM_READ))) {
        wchar_t buffer[MAX_PATH] = {};
        if (SUCCEEDED(link->GetPath(buffer, MAX_PATH, nullptr, 0)))
            target = QString::fromWCharArray(buffer);
    }
    file->Release();
    link->Release();
    return target;
}
#endif

} // namespace

// Each platform offers its own destinations, and the menu is built from
// supports(). A destination claimed but not implemented would put a
// permanently failing action in front of the user.
TEST(ShellShortcutsTest, SupportedDestinationsMatchThePlatform) {
    using Destination = fc::ShellShortcuts::Destination;
    EXPECT_TRUE(fc::ShellShortcuts::isSupported());
    EXPECT_TRUE(fc::ShellShortcuts::supports(Destination::Desktop));
#ifdef Q_OS_WIN
    EXPECT_TRUE(fc::ShellShortcuts::supports(Destination::Startup));
    EXPECT_FALSE(fc::ShellShortcuts::supports(Destination::Applications));
#else
    EXPECT_TRUE(fc::ShellShortcuts::supports(Destination::Applications));
    EXPECT_FALSE(fc::ShellShortcuts::supports(Destination::Startup));
#endif
    // An unsupported destination must refuse rather than write somewhere odd.
    const Destination absent =
#ifdef Q_OS_WIN
        Destination::Applications;
#else
        Destination::Startup;
#endif
    EXPECT_TRUE(fc::ShellShortcuts::locationFor(absent).isEmpty());
    const PlatformResult res = fc::ShellShortcuts::create(QStringLiteral("anything"), absent);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, PlatformError::Unsupported);
}

#ifdef Q_OS_WIN
// Writes into a real Desktop folder, so it cleans up after itself. The name is
// distinctive enough that a leftover is recognisable if cleanup ever fails.
TEST(ShellShortcutsTest, TheShortcutPointsAtTheFileItWasMadeFrom) {
    const QString desktop =
        fc::ShellShortcuts::locationFor(fc::ShellShortcuts::Destination::Desktop);
    ASSERT_FALSE(desktop.isEmpty()) << "Windows did not report a Desktop folder";

    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    // A real .exe, so the test does not depend on a fake one being accepted.
    const QString source = QDir(QString::fromLocal8Bit(qgetenv("SystemRoot")))
                               .filePath(QStringLiteral("System32/notepad.exe"));
    ASSERT_TRUE(QFileInfo::exists(source)) << "no notepad.exe to point at";
    const QString target = QDir(work.path()).filePath(QStringLiteral("fc-shortcut-probe.exe"));
    ASSERT_TRUE(QFile::copy(source, target));

    const QString link = QDir(desktop).filePath(QStringLiteral("fc-shortcut-probe.lnk"));
    QFile::remove(link);

    const PlatformResult res =
        fc::ShellShortcuts::create(target, fc::ShellShortcuts::Destination::Desktop);
    ASSERT_TRUE(res.ok) << qPrintable(res.message);
    ASSERT_TRUE(QFileInfo::exists(link)) << "no .lnk at " << qPrintable(link);

    // Compared as std::string: gtest has no printer for QString and dumps it as
    // a list of two-byte objects, which hides the one thing worth reading.
    EXPECT_EQ(QDir::fromNativeSeparators(shortcutTarget(link)).toStdString(),
              QDir::fromNativeSeparators(QFileInfo(target).absoluteFilePath()).toStdString())
        << "the shortcut does not point at the file it was made from";

    QFile::remove(link);
}

// A missing file must fail before anything is written, so a stale row in the
// panel cannot leave a shortcut to nothing on the desktop.
TEST(ShellShortcutsTest, AMissingTargetCreatesNothing) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString absent = QDir(work.path()).filePath(QStringLiteral("gone.exe"));

    const PlatformResult res =
        fc::ShellShortcuts::create(absent, fc::ShellShortcuts::Destination::Desktop);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, PlatformError::NotFound);

    const QString desktop =
        fc::ShellShortcuts::locationFor(fc::ShellShortcuts::Destination::Desktop);
    EXPECT_FALSE(QFileInfo::exists(QDir(desktop).filePath(QStringLiteral("gone.lnk"))));
}

// Startup must resolve to a folder, since the whole design choice was to use a
// visible folder rather than a registry key the user cannot find.
TEST(ShellShortcutsTest, TheStartupFolderResolvesAndIsNotTheDesktop) {
    const QString startup =
        fc::ShellShortcuts::locationFor(fc::ShellShortcuts::Destination::Startup);
    const QString desktop =
        fc::ShellShortcuts::locationFor(fc::ShellShortcuts::Destination::Desktop);
    EXPECT_FALSE(startup.isEmpty());
    EXPECT_TRUE(QFileInfo(startup).isDir()) << qPrintable(startup);
    EXPECT_NE(startup, desktop);
}
#else // Linux

namespace {

// A file whose first bytes are an ELF header. `appImage` additionally sets the
// type-2 AppImage marker at offset 8, the same one build-appimage.sh checks
// its own output for.
QString writeElf(const QString &dir, const QString &name, bool appImage) {
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    QByteArray head(64, '\0');
    head[0] = '\x7f';
    head[1] = 'E';
    head[2] = 'L';
    head[3] = 'F';
    if (appImage) {
        head[8] = 'A';
        head[9] = 'I';
        head[10] = '\x02';
    }
    file.write(head);
    file.close();
    return path;
}

} // namespace

// A Linux executable usually has no extension, so the name cannot decide this
// -- it is read from the file's own first bytes.
TEST(ShellShortcutsTest, LaunchableIsDecidedByContentNotExtension) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());

    const QString binary = writeElf(work.path(), QStringLiteral("tool"), false);
    ASSERT_FALSE(binary.isEmpty());
    EXPECT_TRUE(fc::ShellShortcuts::isLaunchable(binary));

    const QString text = QDir(work.path()).filePath(QStringLiteral("notes.txt"));
    QFile txt(text);
    ASSERT_TRUE(txt.open(QIODevice::WriteOnly));
    txt.write("#!/bin/sh\necho hello\n");
    txt.close();
    EXPECT_FALSE(fc::ShellShortcuts::isLaunchable(text))
        << "a shell script is not an ELF binary and must not be offered";

    EXPECT_FALSE(fc::ShellShortcuts::isLaunchable(work.path())) << "a directory is not launchable";
}

// The download-then-nothing-happens case: an AppImage with no execute bit.
TEST(ShellShortcutsTest, ANonExecutableAppImageIsDetectedAndCanBeFixed) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString image = writeElf(work.path(), QStringLiteral("App-x86_64.AppImage"), true);
    ASSERT_FALSE(image.isEmpty());
    ASSERT_TRUE(QFile::setPermissions(image, QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    EXPECT_TRUE(fc::ShellShortcuts::needsExecutableBit(image));

    const PlatformResult res = fc::ShellShortcuts::makeExecutable(image);
    ASSERT_TRUE(res.ok) << qPrintable(res.message);
    EXPECT_TRUE(QFileInfo(image).isExecutable());
    // And it must not keep asking once fixed.
    EXPECT_FALSE(fc::ShellShortcuts::needsExecutableBit(image));

    // A plain ELF binary is NOT this case: it may legitimately be a data file
    // the user has no intention of running, so it is left alone.
    const QString binary = writeElf(work.path(), QStringLiteral("library.so"), false);
    ASSERT_TRUE(QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    EXPECT_FALSE(fc::ShellShortcuts::needsExecutableBit(binary));
}

// The .desktop file has to be a launcher, not a text file. The execute bit is
// what makes GNOME and KDE treat it as one.
TEST(ShellShortcutsTest, TheDesktopEntryPointsAtTheBinaryAndIsItselfExecutable) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString binary = writeElf(work.path(), QStringLiteral("probe-tool"), false);
    ASSERT_FALSE(binary.isEmpty());

    const QString dir =
        fc::ShellShortcuts::locationFor(fc::ShellShortcuts::Destination::Applications);
    ASSERT_FALSE(dir.isEmpty());
    const QString entry = QDir(dir).filePath(QStringLiteral("probe-tool.desktop"));
    QFile::remove(entry);

    const PlatformResult res =
        fc::ShellShortcuts::create(binary, fc::ShellShortcuts::Destination::Applications);
    ASSERT_TRUE(res.ok) << qPrintable(res.message);
    ASSERT_TRUE(QFileInfo::exists(entry)) << qPrintable(entry);

    QFile file(entry);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString body = QString::fromUtf8(file.readAll());
    file.close();

    EXPECT_TRUE(body.startsWith(QLatin1String("[Desktop Entry]")));
    EXPECT_TRUE(body.contains(QStringLiteral("Exec=\"%1\"").arg(binary)))
        << "the launcher does not point at the binary:\n"
        << qPrintable(body);
    EXPECT_TRUE(body.contains(QLatin1String("Type=Application")));
    EXPECT_TRUE(QFileInfo(entry).isExecutable())
        << "without the execute bit the desktop shows this as a text file";

    QFile::remove(entry);
}
#endif
