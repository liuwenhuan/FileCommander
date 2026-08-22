#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#endif

#include <string>
#include <vector>

#include "Settings.h"

namespace {
#ifdef Q_OS_WIN
// Describes what is wrong with a path's ACL, or an empty string when nothing
// is.
//
// A reason rather than a bool, because this runs on machines nobody can attach
// a debugger to: when it failed on a CI runner all "Expected: true" said was
// that some part of a twenty-line check did not hold.
//
// The principals compared against are the ones restrictWindowsPath() actually
// grants to -- the process TOKEN USER, LocalSystem and BuiltinAdministrators --
// not the file's owner. Those are the same account in the ordinary case, but
// not always: where a policy makes Administrators the owner of objects an
// administrator creates, the owner is a group the code never named, and an
// otherwise perfectly private ACL reads as a failure.
std::string privateWindowsAclProblem(const QString &path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return "could not open the process token";
    DWORD tokenInfoSize = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenInfoSize);
    std::vector<BYTE> tokenInfo(tokenInfoSize);
    const bool hasTokenUser =
        tokenInfoSize > 0 &&
        GetTokenInformation(token, TokenUser, tokenInfo.data(), tokenInfoSize, &tokenInfoSize);
    CloseHandle(token);
    if (!hasTokenUser)
        return "could not read the token user";
    const PSID userSid = reinterpret_cast<TOKEN_USER *>(tokenInfo.data())->User.Sid;

    const auto sidText = [](PSID sid) -> std::string {
        LPWSTR text = nullptr;
        if (!sid || !ConvertSidToStringSidW(sid, &text))
            return "<unreadable>";
        const std::string out = QString::fromWCharArray(text).toStdString();
        LocalFree(text);
        return out;
    };

    std::wstring widePath = path.toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD status = GetNamedSecurityInfoW(
        widePath.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || !descriptor || !dacl) {
        if (descriptor)
            LocalFree(descriptor);
        return "GetNamedSecurityInfoW failed with " + std::to_string(status);
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        LocalFree(descriptor);
        return "could not read the security descriptor control bits";
    }
    if (!(control & SE_DACL_PROTECTED)) {
        LocalFree(descriptor);
        return "the DACL still inherits (SE_DACL_PROTECTED is not set), so the "
               "path is not private";
    }

    ACL_SIZE_INFORMATION aclInfo{};
    if (!GetAclInformation(dacl, &aclInfo, sizeof(aclInfo), AclSizeInformation)) {
        LocalFree(descriptor);
        return "could not read the ACL contents";
    }

    BYTE systemBuffer[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(systemBuffer);
    BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
    DWORD administratorsSize = sizeof(administratorsBuffer);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer, &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administratorsBuffer,
                            &administratorsSize)) {
        LocalFree(descriptor);
        return "could not build the well-known SIDs to compare against";
    }

    std::string problem;
    bool userHasFullAccess = false;
    for (DWORD index = 0; problem.empty() && index < aclInfo.AceCount; ++index) {
        void *rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            problem = "could not read ACE " + std::to_string(index);
            break;
        }
        const auto *header = static_cast<const ACE_HEADER *>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            problem = "ACE " + std::to_string(index) + " is not an access-allowed ACE";
            break;
        }
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
        const PSID sid = reinterpret_cast<PSID>(const_cast<DWORD *>(&ace->SidStart));
        const bool isUser = EqualSid(sid, userSid);
        if (!isUser && !EqualSid(sid, systemBuffer) && !EqualSid(sid, administratorsBuffer)) {
            problem = "ACE " + std::to_string(index) + " grants access to " + sidText(sid) +
                      ", which is neither this user (" + sidText(userSid) +
                      "), LocalSystem, nor Administrators";
            break;
        }
        if (isUser && ((ace->Mask & GENERIC_ALL) == GENERIC_ALL ||
                       (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS))
            userHasFullAccess = true;
    }

    if (problem.empty() && !userHasFullAccess) {
        problem = "no ACE gives this user (" + sidText(userSid) +
                  ") full access; the owner is " + sidText(owner) + " and the ACL has " +
                  std::to_string(aclInfo.AceCount) + " entries";
    }
    LocalFree(descriptor);
    return problem;
}
#endif

// See test_SessionManager.cpp for why this avoids setTestModeEnabled().
class IsolatedConfigDir {
public:
    IsolatedConfigDir()
        : xdgWasSet(qEnvironmentVariableIsSet("XDG_CONFIG_HOME")),
          previousXdg(qgetenv("XDG_CONFIG_HOME")),
          overrideWasSet(qEnvironmentVariableIsSet("FILECOMMANDER_CONFIG_HOME")),
          previousOverride(qgetenv("FILECOMMANDER_CONFIG_HOME")) {
        if (dir.isValid()) {
            qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
            qputenv("FILECOMMANDER_CONFIG_HOME", dir.path().toUtf8());
        }
    }

    ~IsolatedConfigDir() {
        if (!dir.isValid())
            return;
        if (xdgWasSet)
            qputenv("XDG_CONFIG_HOME", previousXdg);
        else
            qunsetenv("XDG_CONFIG_HOME");
        if (overrideWasSet)
            qputenv("FILECOMMANDER_CONFIG_HOME", previousOverride);
        else
            qunsetenv("FILECOMMANDER_CONFIG_HOME");
    }

    bool isValid() const { return dir.isValid(); }
    QTemporaryDir dir;

private:
    bool xdgWasSet;
    QByteArray previousXdg;
    bool overrideWasSet;
    QByteArray previousOverride;
};
} // namespace

TEST(SettingsTest, ExplicitIniPathPersistsWithoutChangingGlobalConfigLocation) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString settingsPath = temporaryDir.filePath(QStringLiteral("isolated.ini"));
    Settings settings(settingsPath);
    settings.setNotepadEditorHeight(180);

    Settings reloaded(settingsPath);
    EXPECT_EQ(reloaded.notepadEditorHeight(), 180);
}

TEST(SettingsTest, EmptyExplicitIniPathUsesSafeDefaultLocation) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    const QString expectedDir = isolated.dir.filePath(QStringLiteral("FileCommander"));
    EXPECT_EQ(Settings::configDir(), expectedDir);

    Settings settings{QString()};
    settings.setNotepadEditorHeight(181);

    Settings reloaded;
    EXPECT_EQ(reloaded.notepadEditorHeight(), 181);
}

TEST(SettingsTest, DefaultConfigDirectoryAndFileUsePlatformPrivateStorage) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());

    {
        Settings settings;
        settings.setNotepadEditorHeight(182);
    }

#ifdef Q_OS_WIN
    EXPECT_EQ(privateWindowsAclProblem(Settings::configDir()), std::string())
        << "on " << Settings::configDir().toStdString();
    EXPECT_EQ(privateWindowsAclProblem(Settings::configFilePath()), std::string())
        << "on " << Settings::configFilePath().toStdString();
#else
    const QFileDevice::Permissions privateDirectory =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser;
    const QFileDevice::Permissions privateFile =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser;
    EXPECT_EQ(QFileInfo(Settings::configDir()).permissions(), privateDirectory);
    EXPECT_EQ(QFileInfo(Settings::configFilePath()).permissions(), privateFile);
#endif
}

TEST(SettingsTest, ShortcutReturnsDefaultWhenUnset) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    EXPECT_EQ(settings.shortcut("copy", QKeySequence(Qt::Key_F5)),
              QKeySequence(Qt::Key_F5));
}

TEST(SettingsTest, SetShortcutOverridesDefault) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.setShortcut("copy", QKeySequence(Qt::CTRL | Qt::Key_C));
    EXPECT_EQ(settings.shortcut("copy", QKeySequence(Qt::Key_F5)),
              QKeySequence(Qt::CTRL | Qt::Key_C));
}

TEST(SettingsTest, ClearShortcutOverridesRestoresDefaults) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.setShortcut("copy", QKeySequence(Qt::CTRL | Qt::Key_C));
    settings.clearShortcutOverrides();
    EXPECT_EQ(settings.shortcut("copy", QKeySequence(Qt::Key_F5)),
              QKeySequence(Qt::Key_F5));
}

TEST(SettingsTest, ListFontSizeDefaultsToTwelve) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    EXPECT_EQ(settings.listFontSize(), 12);
}

TEST(SettingsTest, ListFontSizeRoundTrips) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.setListFontSize(14);
    EXPECT_EQ(settings.listFontSize(), 14);
}

TEST(SettingsTest, ListFontSizeClampsToRange) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.setListFontSize(2);
    EXPECT_EQ(settings.listFontSize(), 8);
    settings.setListFontSize(99);
    EXPECT_EQ(settings.listFontSize(), 16);
}

TEST(SettingsTest, MenuFontSizeDefaultsToTwelveAndRoundTrips) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;

    EXPECT_EQ(settings.menuFontSize(), 12);

    settings.setMenuFontSize(13);

    EXPECT_EQ(settings.menuFontSize(), 13);
}

TEST(SettingsTest, MenuFontSizeClampsToRange) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;

    settings.setMenuFontSize(2);
    EXPECT_EQ(settings.menuFontSize(), 8);
    settings.setMenuFontSize(99);
    EXPECT_EQ(settings.menuFontSize(), 16);
}

TEST(SettingsTest, NotifyOnReceivedDefaultsOnAndRoundTrips) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    // An arrival that leaves no trace except a new file is easy to miss, so the
    // notice is on unless the user turns it off.
    EXPECT_TRUE(settings.notifyOnReceived());
    settings.setNotifyOnReceived(false);
    EXPECT_FALSE(settings.notifyOnReceived());
}

TEST(SettingsTest, RemovesLegacyReduceMotionPreference) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    const QString settingsPath = temporaryDir.filePath(QStringLiteral("settings.ini"));

    {
        QSettings legacy(settingsPath, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("appearance/reduceMotion"), true);
        legacy.sync();
    }

    { Settings migrated(settingsPath); }

    QSettings stored(settingsPath, QSettings::IniFormat);
    EXPECT_FALSE(stored.contains(QStringLiteral("appearance/reduceMotion")));
}

TEST(SettingsTest, GlobalFontFamilyDefaultsToSystemAndRoundTrips) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;

    EXPECT_TRUE(settings.globalFontFamily().isEmpty());
    EXPECT_TRUE(settings.showTabBar());
    EXPECT_TRUE(settings.showShortcutLabels());
    EXPECT_TRUE(settings.autoUpdateCheck());

    settings.setGlobalFontFamily(QStringLiteral("Noto Sans CJK SC"));
    settings.setShowTabBar(false);
    settings.setShowShortcutLabels(false);
    settings.setAutoUpdateCheck(false);

    EXPECT_EQ(settings.globalFontFamily(), QStringLiteral("Noto Sans CJK SC"));
    EXPECT_FALSE(settings.showTabBar());
    EXPECT_FALSE(settings.showShortcutLabels());
    EXPECT_FALSE(settings.autoUpdateCheck());
}

TEST(SettingsTest, WindowGeometryRoundTrips) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    const QByteArray blob = "fake-geometry-bytes";
    settings.setWindowGeometry(blob);
    EXPECT_EQ(settings.windowGeometry(), blob);
}

TEST(SettingsTest, FavoriteDirectoriesDefaultToHomeOnFirstRun) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    EXPECT_EQ(settings.favoriteDirectories(), QStringList({QDir::homePath()}));
}

TEST(SettingsTest, AddFavoriteDirectoryPersists) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.removeFavoriteDirectory(QDir::homePath()); // drop the first-run seed
    settings.addFavoriteDirectory("/home/user/projects");
    settings.addFavoriteDirectory("/tmp");
    EXPECT_EQ(settings.favoriteDirectories(),
              QStringList({"/home/user/projects", "/tmp"}));
}

TEST(SettingsTest, AddFavoriteDirectoryDoesNotDuplicate) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.removeFavoriteDirectory(QDir::homePath()); // drop the first-run seed
    settings.addFavoriteDirectory("/tmp");
    settings.addFavoriteDirectory("/tmp");
    EXPECT_EQ(settings.favoriteDirectories().size(), 1);
}

TEST(SettingsTest, RemoveFavoriteDirectoryRemovesIt) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.removeFavoriteDirectory(QDir::homePath()); // drop the first-run seed
    settings.addFavoriteDirectory("/tmp");
    settings.addFavoriteDirectory("/home");
    settings.removeFavoriteDirectory("/tmp");
    EXPECT_EQ(settings.favoriteDirectories(), QStringList({"/home"}));
}

TEST(SettingsTest, MaxConcurrentTransfersDefaultsToTwo) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    EXPECT_EQ(settings.maxConcurrentTransfers(), 2);
}

TEST(SettingsTest, MaxConcurrentTransfersRoundTrips) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.setMaxConcurrentTransfers(4);
    EXPECT_EQ(settings.maxConcurrentTransfers(), 4);
}

TEST(SettingsTest, MaxConcurrentTransfersClampsToRange) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    Settings settings;
    settings.setMaxConcurrentTransfers(0);
    EXPECT_EQ(settings.maxConcurrentTransfers(), 1);
    settings.setMaxConcurrentTransfers(99);
    EXPECT_EQ(settings.maxConcurrentTransfers(), 8);
}

TEST(SettingsTest, VideoMutedDefaultsToFalse) {
    IsolatedConfigDir iso;
    Settings settings;
    EXPECT_FALSE(settings.videoMuted())
        << "A fresh install should preview with sound — the user unmutes once to discover it";
}

TEST(SettingsTest, VideoMutedRoundTripsExplicitTrue) {
    IsolatedConfigDir iso;
    Settings settings;
    settings.setVideoMuted(true);
    EXPECT_TRUE(settings.videoMuted()) << "Explicitly muted must survive reload";
    Settings reloaded;
    EXPECT_TRUE(reloaded.videoMuted()) << "Persisted mute must survive reload";
}

TEST(SettingsTest, VideoMutedRoundTripsExplicitFalse) {
    IsolatedConfigDir iso;
    Settings settings;
    settings.setVideoMuted(true);  // from fresh default false
    settings.setVideoMuted(false);
    EXPECT_FALSE(settings.videoMuted());
    Settings reloaded;
    EXPECT_FALSE(reloaded.videoMuted());
}

TEST(SettingsTest, VideoVolumeDefaultsToSeventy) {
    IsolatedConfigDir iso;
    Settings settings;
    EXPECT_EQ(settings.videoVolume(), 70);
}

TEST(SettingsTest, VideoVolumeClamps) {
    IsolatedConfigDir iso;
    Settings settings;
    settings.setVideoVolume(-5);
    EXPECT_EQ(settings.videoVolume(), 0);
    settings.setVideoVolume(200);
    EXPECT_EQ(settings.videoVolume(), 100);
}

TEST(SettingsTest, AccountServerDefaultsToOfficial) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));

    EXPECT_TRUE(settings.accountUsesOfficialServer());
    EXPECT_TRUE(settings.accountCustomServerUrl().isEmpty());
}

TEST(SettingsTest, CustomAccountServerRoundTrips) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("settings.ini"));
    {
        Settings settings(ini);
        settings.setAccountUsesOfficialServer(false);
        settings.setAccountCustomServerUrl(QStringLiteral("https://example.test/api///"));
    }

    Settings reloaded(ini);
    EXPECT_FALSE(reloaded.accountUsesOfficialServer());
    EXPECT_EQ(reloaded.accountCustomServerUrl(), QStringLiteral("https://example.test/api"));
}

TEST(SettingsTest, LegacyOfficialAccountServerMigratesToTheNewOfficialChoice) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings legacy(ini, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("account/serverUrl"),
                        QStringLiteral("https://sgvps.aigutta.com/"));
    }

    Settings migrated(ini);
    EXPECT_TRUE(migrated.accountUsesOfficialServer());
    EXPECT_TRUE(migrated.accountCustomServerUrl().isEmpty());
    QSettings stored(ini, QSettings::IniFormat);
    EXPECT_FALSE(stored.contains(QStringLiteral("account/serverUrl")));
}

TEST(SettingsTest, LegacyCustomAccountServerMigratesWithoutBeingReclassified) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings legacy(ini, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("account/serverUrl"),
                        QStringLiteral(" http://localhost:9000/api/// "));
    }

    Settings migrated(ini);
    EXPECT_FALSE(migrated.accountUsesOfficialServer());
    EXPECT_EQ(migrated.accountCustomServerUrl(), QStringLiteral("http://localhost:9000/api"));
}

TEST(SettingsTest, ExistingAccountServerSchemaIsNotOverwrittenByLegacyData) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings stored(ini, QSettings::IniFormat);
        stored.setValue(QStringLiteral("account/serverMode"), QStringLiteral("custom"));
        stored.setValue(QStringLiteral("account/customServerUrl"),
                        QStringLiteral("https://custom.example/api/"));
        stored.setValue(QStringLiteral("account/serverUrl"),
                        QStringLiteral("https://sgvps.aigutta.com"));
    }

    Settings settings(ini);
    EXPECT_FALSE(settings.accountUsesOfficialServer());
    EXPECT_EQ(settings.accountCustomServerUrl(), QStringLiteral("https://custom.example/api"));
    QSettings stored(ini, QSettings::IniFormat);
    EXPECT_FALSE(stored.contains(QStringLiteral("account/serverUrl")));
}

TEST(SettingsTest, UnknownAccountServerModeFallsBackToOfficial) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings stored(ini, QSettings::IniFormat);
        stored.setValue(QStringLiteral("account/serverMode"), QStringLiteral("mystery"));
    }
    Settings settings(ini);
    EXPECT_TRUE(settings.accountUsesOfficialServer());
}
