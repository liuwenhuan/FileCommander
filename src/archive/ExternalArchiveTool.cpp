#include "ExternalArchiveTool.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

// Resolve a tool once and cache it. Empty string == not installed.
QString findTool(const char *const *names) {
    for (const char *const *n = names; *n; ++n) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(*n));
        if (!p.isEmpty())
            return p;
    }
    return {};
}

const QString &sevenZipExe() {
    static const char *const names[] = {"7z", "7za", "7zr", nullptr};
    static const QString exe = findTool(names);
    return exe;
}

const QString &unrarExe() {
    static const char *const names[] = {"unrar", nullptr};
    static const QString exe = findTool(names);
    return exe;
}

bool isRar(const QString &path) { return path.trimmed().toLower().endsWith(QStringLiteral(".rar")); }

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
    if (r.exitCode != 0 && r.exitCode != 1)
        return classifyFailure(r.err, password);

    // -slt emits a header, then one "Key = Value" block per entry after a line of
    // dashes. Parse blocks; a blank line ends the current entry.
    const QString text = QString::fromUtf8(r.out);
    const QStringList lines = text.split(QLatin1Char('\n'));
    bool inEntries = false;
    bool have = false;
    ExternalArchiveTool::Entry cur;
    auto flush = [&]() {
        if (have && !cur.path.isEmpty())
            cb(cur);
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
    return ExternalArchiveTool::Status::Ok;
}

ExternalArchiveTool::Status readWith7z(const QString &archivePath, const QString &password,
                                       const QString &entryPath, const QString &destPath,
                                       std::atomic<bool> *cancel) {
    const ProcResult r = runProcess(
        sevenZipExe(),
        {QStringLiteral("x"), QStringLiteral("-so"), QStringLiteral("-y"),
         sevenZipPasswordArg(password), archivePath, entryPath},
        cancel, 120000);
    if (!r.ran)
        return ExternalArchiveTool::Status::Error;
    if (r.exitCode != 0 && r.exitCode != 1)
        return classifyFailure(r.err, password);
    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly))
        return ExternalArchiveTool::Status::Error;
    out.write(r.out);
    out.close();
    return ExternalArchiveTool::Status::Ok;
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
    const ProcResult r = runProcess(
        unrarExe(),
        {QStringLiteral("p"), QStringLiteral("-inul"),
         QStringLiteral("-p") + (password.isEmpty() ? QStringLiteral("-") : password), archivePath,
         entryPath},
        cancel, 120000);
    if (!r.ran)
        return ExternalArchiveTool::Status::Error;
    if (r.exitCode != 0)
        return classifyFailure(r.err, password);
    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly))
        return ExternalArchiveTool::Status::Error;
    out.write(r.out);
    out.close();
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
                                                           std::atomic<bool> *cancel) {
    if (use7z())
        return readWith7z(archivePath, password, entryPath, destPath, cancel);
    if (useUnrar(archivePath))
        return readWithUnrar(archivePath, password, entryPath, destPath, cancel);
    return Status::Unavailable;
}
