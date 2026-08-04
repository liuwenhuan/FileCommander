#include "ExternalArchiveTool.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

// Where a tool lives when it is installed but not on PATH. 7-Zip's Windows
// installer does exactly that: it drops 7z.exe in Program Files and adds
// nothing to PATH, so findExecutable() alone reports "not installed" on a
// machine that plainly has it -- measured on the machine this was written for.
QStringList wellKnownToolDirs() {
    QStringList dirs;
#ifdef Q_OS_WIN
    for (const char *var : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        const QString base = qEnvironmentVariable(var);
        if (!base.isEmpty())
            dirs << QDir(base).filePath(QStringLiteral("7-Zip"));
    }
    const QString localApp = qEnvironmentVariable("LOCALAPPDATA");
    if (!localApp.isEmpty())
        dirs << QDir(localApp).filePath(QStringLiteral("Programs/7-Zip"));
#endif
    return dirs;
}

// Resolve a tool once and cache it. Empty string == not installed.
QString findTool(const char *const *names) {
    for (const char *const *n = names; *n; ++n) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(*n));
        if (!p.isEmpty())
            return p;
    }
    // PATH did not have it; try the places an installer would have put it.
    const QStringList dirs = wellKnownToolDirs();
    for (const char *const *n = names; *n; ++n) {
        const QString p =
            QStandardPaths::findExecutable(QString::fromLatin1(*n), dirs);
        if (!p.isEmpty())
            return p;
    }
    return {};
}

// Test hook: pretend nothing is installed. The no-external-tool fallbacks
// cannot be reached any other way on a developer machine that has 7-Zip, and a
// test that silently exercises the 7z path instead is worse than no test.
// Read every call rather than cached, so a test can set it after the binary has
// already resolved the tool for an earlier case.
bool toolsDisabled() {
    return qEnvironmentVariableIsSet("FILECOMMANDER_NO_EXTERNAL_ARCHIVE_TOOL");
}

const QString &sevenZipExe() {
    static const char *const names[] = {"7z", "7za", "7zr", nullptr};
    static const QString exe = findTool(names);
    static const QString none;
    return toolsDisabled() ? none : exe;
}

const QString &unrarExe() {
    static const char *const names[] = {"unrar", nullptr};
    static const QString exe = findTool(names);
    static const QString none;
    return toolsDisabled() ? none : exe;
}

bool isRar(const QString &path) { return path.trimmed().toLower().endsWith(QStringLiteral(".rar")); }

// Is this a UDF disc image? Reads the ECMA-167 Volume Recognition Sequence: a
// run of 2048-byte descriptors starting at sector 16, each tagged with a 5-byte
// standard identifier. "NSR02"/"NSR03" means a UDF filesystem is present.
//
// Most install ISOs are UDF *bridge* images: they carry both an ISO9660
// structure (a stub, for old readers) and the real UDF tree. libarchive only
// implements ISO9660, so it opens the file happily and reports the handful of
// stub entries -- a silent wrong answer rather than an error. Detecting the NSR
// descriptor lets the caller route those to 7z while leaving pure ISO9660
// images on the in-process path.
bool isUdfImage(const QString &path) {
    const QString lower = path.trimmed().toLower();
    if (!lower.endsWith(QStringLiteral(".iso")) && !lower.endsWith(QStringLiteral(".img")) &&
        !lower.endsWith(QStringLiteral(".udf")))
        return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    constexpr qint64 kSectorSize = 2048;
    constexpr qint64 kFirstSector = 16; // ECMA-119: recognition area starts here
    constexpr int kMaxDescriptors = 16; // bounded scan; real sequences are short
    if (!f.seek(kFirstSector * kSectorSize))
        return false;

    for (int i = 0; i < kMaxDescriptors; ++i) {
        const QByteArray sector = f.read(kSectorSize);
        if (sector.size() < 7)
            break;
        // Layout: 1 byte structure type, then a 5-byte standard identifier.
        const QByteArray id = sector.mid(1, 5);
        if (id == "NSR02" || id == "NSR03")
            return true;
        // TEA01 terminates the sequence. Anything unrecognised means we've run
        // off the end of the descriptors, so stop rather than scan garbage.
        if (id != "BEA01" && id != "CD001" && id != "CDW02" && id != "BOOT2")
            break;
    }
    return false;
}

struct ProcResult {
    bool ran = false; // process actually finished (not killed / failed to start)
    int exitCode = -1;
    QByteArray out;
    QString err;
};

// Runs `exe args`, capturing stdout (bytes) and stderr (text). Password is passed
// via the tool's own flag by the caller, not here. Polls `cancel` and enforces a
// timeout so an abandoned preview can't wedge a worker thread.
ProcResult runProcess(const QString &exe, const QStringList &args, std::atomic<bool> *cancel,
                      int timeoutMs) {
    ProcResult r;
    QProcess proc;
    proc.setProgram(exe);
    proc.setArguments(args);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!proc.waitForStarted(5000))
        return r;

    int waited = 0;
    const int slice = 150;
    while (!proc.waitForFinished(slice)) {
        if (cancel && cancel->load()) {
            proc.kill();
            proc.waitForFinished(1000);
            return r;
        }
        waited += slice;
        if (waited >= timeoutMs) {
            proc.kill();
            proc.waitForFinished(1000);
            return r;
        }
    }
    r.ran = proc.exitStatus() == QProcess::NormalExit;
    r.exitCode = proc.exitCode();
    r.out = proc.readAllStandardOutput();
    r.err = QString::fromLocal8Bit(proc.readAllStandardError());
    return r;
}

struct StreamResult {
    bool ran = false; // process finished AND every byte reached the file
    int exitCode = -1;
    QString err;
};

// Like runProcess, but pipes stdout straight into `destPath` instead of holding
// it in memory. Used for extraction, where an entry can be many GB.
StreamResult runProcessToFile(const QString &exe, const QStringList &args, const QString &destPath,
                              std::atomic<bool> *cancel, int timeoutMs) {
    StreamResult r;
    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return r;

    QProcess proc;
    proc.setProgram(exe);
    proc.setArguments(args);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(QIODevice::ReadOnly);
    if (!proc.waitForStarted(5000)) {
        out.close();
        out.remove();
        return r;
    }

    // Drain BOTH pipes as data arrives. stdout obviously, or the writer stalls
    // once the pipe buffer fills -- but stderr too: it's only ~64 KiB, and a tool
    // that chatters past that would block forever on a write nobody is reading,
    // which on a multi-GB extract means a wedged worker thread.
    bool writeFailed = false;
    QByteArray errBuf;
    auto drain = [&]() {
        // Keep only a bounded head: this is diagnostic text for classifyFailure,
        // and a runaway tool shouldn't be able to grow it without limit.
        constexpr int kMaxErrBytes = 64 * 1024;
        const QByteArray e = proc.readAllStandardError();
        if (errBuf.size() < kMaxErrBytes)
            errBuf += e.left(kMaxErrBytes - errBuf.size());
        while (proc.bytesAvailable() > 0) {
            const QByteArray chunk = proc.read(1 << 20);
            if (chunk.isEmpty())
                break;
            if (out.write(chunk) != chunk.size()) {
                writeFailed = true;
                return;
            }
        }
    };

    int waited = 0;
    const int slice = 150;
    bool finished = false;
    while (!finished) {
        finished = proc.waitForFinished(slice);
        drain();
        if (writeFailed) {
            proc.kill();
            proc.waitForFinished(1000);
            break;
        }
        if (finished)
            break;
        if (cancel && cancel->load()) {
            proc.kill();
            proc.waitForFinished(1000);
            break;
        }
        waited += slice;
        if (waited >= timeoutMs) {
            proc.kill();
            proc.waitForFinished(1000);
            break;
        }
    }
    if (finished && !writeFailed) {
        // waitForFinished can return before the last readyRead is delivered.
        proc.waitForReadyRead(0);
        drain();
    }

    errBuf += proc.readAllStandardError();
    r.err = QString::fromLocal8Bit(errBuf);
    const bool clean = finished && !writeFailed && proc.exitStatus() == QProcess::NormalExit;
    if (clean) {
        r.ran = out.flush();
        r.exitCode = proc.exitCode();
    }
    out.close();
    if (!r.ran)
        out.remove(); // never leave a half-written file behind for a caller to serve
    return r;
}

// Classify a failed run as a password problem when the tool's diagnostics mention
// encryption; an empty password then means NeedPassword, otherwise WrongPassword.
ExternalArchiveTool::Status classifyFailure(const QString &stderrText, const QString &password) {
    const QString e = stderrText.toLower();
    const bool encrypted = e.contains(QLatin1String("password")) ||
                           e.contains(QLatin1String("encrypt")) ||
                           e.contains(QLatin1String("wrong"));
    if (encrypted)
        return password.isEmpty() ? ExternalArchiveTool::Status::NeedPassword
                                  : ExternalArchiveTool::Status::WrongPassword;
    return ExternalArchiveTool::Status::Error;
}

// 7z password flag. Always present (with -y) so 7z never blocks on a prompt; an
// empty value is the "no password" case. NB: this is visible in the process
// table, the same trade-off the office/zip preview paths already make locally.
QString sevenZipPasswordArg(const QString &password) {
    return QStringLiteral("-p") + password;
}

ExternalArchiveTool::Status listWith7z(const QString &archivePath, const QString &password,
                                       const std::function<void(const ExternalArchiveTool::Entry &)> &cb,
                                       std::atomic<bool> *cancel) {
    const ProcResult r = runProcess(
        sevenZipExe(),
        {QStringLiteral("l"), QStringLiteral("-slt"), QStringLiteral("-y"),
         sevenZipPasswordArg(password), archivePath},
        cancel, 60000);
    if (!r.ran)
        return ExternalArchiveTool::Status::Error;

    // Parse BEFORE judging the exit code. 7z reports a non-zero status for
    // non-fatal complaints too -- a UDF bridge image, for instance, exits 2 with
    // "Headers Error" over the malformed ISO9660 stub while still listing the
    // whole UDF tree correctly. Treating that as fatal would throw away a
    // perfectly good listing, so entries are collected first and the exit code
    // only decides what to do when nothing came back.
    //
    // -slt emits a header, then one "Key = Value" block per entry after a line of
    // dashes. Parse blocks; a blank line ends the current entry.
    const QString text = QString::fromUtf8(r.out);
    const QStringList lines = text.split(QLatin1Char('\n'));
    bool inEntries = false;
    bool have = false;
    ExternalArchiveTool::Entry cur;
    QVector<ExternalArchiveTool::Entry> entries;
    auto flush = [&]() {
        if (have && !cur.path.isEmpty())
            entries.append(cur);
        cur = ExternalArchiveTool::Entry{};
        have = false;
    };
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (!inEntries) {
            if (line.startsWith(QLatin1String("----------")))
                inEntries = true;
            continue;
        }
        if (line.isEmpty()) {
            flush();
            continue;
        }
        const int eq = line.indexOf(QLatin1String(" = "));
        if (eq < 0)
            continue;
        const QString key = line.left(eq);
        const QString val = line.mid(eq + 3);
        if (key == QLatin1String("Path")) {
            cur.path = QString(val).replace(QLatin1Char('\\'), QLatin1Char('/'));
            have = true;
        } else if (key == QLatin1String("Size")) {
            cur.size = val.toLongLong();
        } else if (key == QLatin1String("Attributes")) {
            if (val.startsWith(QLatin1Char('D')) || val.contains(QLatin1String("D_")))
                cur.isDir = true;
        } else if (key == QLatin1String("Folder")) {
            if (val == QLatin1String("+"))
                cur.isDir = true;
        } else if (key == QLatin1String("Modified") && !val.isEmpty()) {
            cur.modified = QDateTime::fromString(val.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
    }
    flush();

    // Nothing parsed -> the run really did fail; let the exit code and stderr say
    // how (a password prompt is the common case).
    if (entries.isEmpty() && r.exitCode != 0)
        return classifyFailure(r.err, password);

    for (const ExternalArchiveTool::Entry &e : entries)
        cb(e);
    return ExternalArchiveTool::Status::Ok;
}

ExternalArchiveTool::Status readWith7z(const QString &archivePath, const QString &password,
                                       const QString &entryPath, const QString &destPath,
                                       std::atomic<bool> *cancel, qint64 expectedSize) {
    // Streams rather than using runProcess: a disc image entry can be multiple
    // gigabytes (an install.wim is routinely > 4 GB), and buffering stdout in a
    // QByteArray before writing would need that much RAM -- and would exceed
    // QByteArray's 2 GB limit outright.
    const StreamResult r = runProcessToFile(
        sevenZipExe(),
        {QStringLiteral("x"), QStringLiteral("-so"), QStringLiteral("-y"),
         sevenZipPasswordArg(password), archivePath, entryPath},
        destPath, cancel, 600000);
    if (!r.ran)
        return ExternalArchiveTool::Status::Error;
    if (r.exitCode == 0)
        return ExternalArchiveTool::Status::Ok;

    // Non-zero exit: trust the bytes over the status, but only when they check
    // out (see the header note on expectedSize). Anything short or empty is a
    // real failure -- serving a truncated file to a viewer would be worse than
    // reporting the error.
    const qint64 got = QFileInfo(destPath).size();
    const bool sizeOk = expectedSize >= 0 ? got == expectedSize : got > 0;
    if (sizeOk)
        return ExternalArchiveTool::Status::Ok;
    QFile::remove(destPath);
    return classifyFailure(r.err, password);
}

// unrar fallback (only when 7z is absent). `unrar lb` lists bare paths; sizes and
// per-entry types aren't cheaply available, so files are reported flat and the
// caller's tree builder infers intermediate directories from the paths.
ExternalArchiveTool::Status listWithUnrar(const QString &archivePath, const QString &password,
                                          const std::function<void(const ExternalArchiveTool::Entry &)> &cb,
                                          std::atomic<bool> *cancel) {
    const ProcResult r = runProcess(
        unrarExe(),
        {QStringLiteral("lb"), QStringLiteral("-p") + (password.isEmpty() ? QStringLiteral("-") : password),
         archivePath},
        cancel, 60000);
    if (!r.ran)
        return ExternalArchiveTool::Status::Error;
    if (r.exitCode != 0)
        return classifyFailure(r.err, password);
    const QStringList paths = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &p : paths) {
        ExternalArchiveTool::Entry e;
        e.path = QString(p).replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (e.path.endsWith(QLatin1Char('/')))
            e.path.chop(1);
        if (!e.path.isEmpty())
            cb(e);
    }
    return ExternalArchiveTool::Status::Ok;
}

ExternalArchiveTool::Status readWithUnrar(const QString &archivePath, const QString &password,
                                          const QString &entryPath, const QString &destPath,
                                          std::atomic<bool> *cancel) {
    const StreamResult r = runProcessToFile(
        unrarExe(),
        {QStringLiteral("p"), QStringLiteral("-inul"),
         QStringLiteral("-p") + (password.isEmpty() ? QStringLiteral("-") : password), archivePath,
         entryPath},
        destPath, cancel, 600000);
    if (!r.ran)
        return ExternalArchiveTool::Status::Error;
    if (r.exitCode != 0)
        return classifyFailure(r.err, password);
    return ExternalArchiveTool::Status::Ok;
}

// Which backend handles this archive: 7z covers essentially everything (incl.
// encrypted rar via p7zip); unrar is only used for .rar when 7z is absent.
bool use7z() { return !sevenZipExe().isEmpty(); }
bool useUnrar(const QString &archivePath) {
    return sevenZipExe().isEmpty() && isRar(archivePath) && !unrarExe().isEmpty();
}

} // namespace

bool ExternalArchiveTool::available(const QString &archivePath) {
    return use7z() || useUnrar(archivePath);
}

bool ExternalArchiveTool::preferExternal(const QString &archivePath) {
    if (!use7z())
        return false; // nothing better to switch to
    return isUdfImage(archivePath);
}

ExternalArchiveTool::Status ExternalArchiveTool::list(const QString &archivePath,
                                                      const QString &password,
                                                      const std::function<void(const Entry &)> &cb,
                                                      std::atomic<bool> *cancel) {
    if (use7z())
        return listWith7z(archivePath, password, cb, cancel);
    if (useUnrar(archivePath))
        return listWithUnrar(archivePath, password, cb, cancel);
    return Status::Unavailable;
}

ExternalArchiveTool::Status ExternalArchiveTool::readEntry(const QString &archivePath,
                                                           const QString &password,
                                                           const QString &entryPath,
                                                           const QString &destPath,
                                                           std::atomic<bool> *cancel,
                                                           qint64 expectedSize) {
    if (use7z())
        return readWith7z(archivePath, password, entryPath, destPath, cancel, expectedSize);
    if (useUnrar(archivePath))
        return readWithUnrar(archivePath, password, entryPath, destPath, cancel);
    return Status::Unavailable;
}
