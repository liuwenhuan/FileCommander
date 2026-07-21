#include "OfficeConverter.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

namespace {

constexpr int kTimeoutMs = 30000; // office_oxide should be near-instant; this is a generous cap.

QString suffixLower(const QString &path) {
    return QFileInfo(path).suffix().toLower();
}

// Candidate binary names, tried in order at each search location.
const QStringList &candidateNames() {
    static const QStringList names = {QStringLiteral("office_oxide"), QStringLiteral("office-oxide"),
                                       QStringLiteral("oxide")};
    return names;
}

// Result of one synchronous subprocess invocation.
struct ProcResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    QString stdOut;
    QString stdErr;
};

ProcResult runOfficeOxide(const QString &binary, const QStringList &args, int timeoutMs) {
    ProcResult r;
    QProcess proc;
    proc.start(binary, args);
    if (!proc.waitForStarted(timeoutMs)) {
        r.stdErr = QStringLiteral("failed to start `%1`").arg(binary);
        return r;
    }
    r.started = true;
    proc.closeWriteChannel();
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        r.timedOut = true;
        r.stdErr = QStringLiteral("`%1` timed out after %2 ms").arg(binary).arg(timeoutMs);
        return r;
    }
    r.exitCode = proc.exitCode();
    r.stdOut = QString::fromUtf8(proc.readAllStandardOutput());
    r.stdErr = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    return r;
}

// Heuristic: does this stderr look like a clap "you gave me an argument I
// don't understand" error, as opposed to a real conversion failure? Used to
// decide whether a speculative flag/subcommand should be retried without it.
bool looksLikeUsageError(const QString &stdErrText) {
    const QString s = stdErrText.toLower();
    return s.contains(QStringLiteral("unexpected argument")) ||
           s.contains(QStringLiteral("unrecognized subcommand")) ||
           s.contains(QStringLiteral("found argument")) ||
           s.contains(QStringLiteral("invalid value")) ||
           s.contains(QStringLiteral("unknown option")) ||
           s.contains(QStringLiteral("error: unknown"));
}

} // namespace

bool OfficeConverter::isOfficeFile(const QString &path) {
    return kindFor(path) != Kind::None;
}

OfficeConverter::Kind OfficeConverter::kindFor(const QString &path) {
    const QString suffix = suffixLower(path);
    if (suffix == QLatin1String("doc") || suffix == QLatin1String("docx") ||
        suffix == QLatin1String("ppt") || suffix == QLatin1String("pptx"))
        return Kind::Document;
    if (suffix == QLatin1String("xls") || suffix == QLatin1String("xlsx"))
        return Kind::Spreadsheet;
    return Kind::None;
}

QString OfficeConverter::resolveBinary() {
    // 1. Explicit override. An unusable override falls through rather than
    // hard-failing resolution, so a stale env var doesn't break PATH lookup.
    const QString envPath = qEnvironmentVariable("TTC_OFFICE_OXIDE");
    if (!envPath.isEmpty()) {
        const QFileInfo info(envPath);
        if (info.exists() && info.isExecutable())
            return info.absoluteFilePath();
    }

    // 2. PATH.
    for (const QString &name : candidateNames()) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }

    // 3. Common install locations that aren't always on PATH.
    const QStringList extraDirs = {
        QDir::homePath() + QStringLiteral("/.local/bin"),
        QDir::homePath() + QStringLiteral("/.cargo/bin"),
    };
    for (const QString &name : candidateNames()) {
        const QString found = QStandardPaths::findExecutable(name, extraDirs);
        if (!found.isEmpty())
            return found;
    }

    return QString();
}

bool OfficeConverter::isAvailable() {
    return !resolveBinary().isEmpty();
}

OfficeConverter::Result OfficeConverter::convert(const QString &path) {
    Result result;
    result.kind = kindFor(path);
    if (result.kind == Kind::None) {
        result.error = QStringLiteral("Not a recognized Office file: %1").arg(path);
        return result;
    }
    if (!QFileInfo::exists(path)) {
        result.error = QStringLiteral("File does not exist: %1").arg(path);
        return result;
    }

    const QString binary = resolveBinary();
    if (binary.isEmpty()) {
        result.error = QStringLiteral(
            "office_oxide CLI not found. Install it with `cargo install office_oxide_cli`, "
            "ensure the `office-oxide` binary is on PATH, or set TTC_OFFICE_OXIDE to its full path.");
        return result;
    }

    return (result.kind == Kind::Document) ? convertDocument(binary, path) : convertSpreadsheet(binary, path);
}

OfficeConverter::Result OfficeConverter::convertDocument(const QString &binary, const QString &path) {
    Result result;
    result.kind = Kind::Document;

    // Directory to receive extracted images. Created eagerly (with a unique
    // name) so it exists whether or not the CLI ends up writing into it. We
    // disable auto-removal: the caller owns cleanup once it has read the
    // markdown and resolved the image links against this path.
    QTemporaryDir workDir(QDir::tempPath() + QStringLiteral("/ttc-office-oxide-XXXXXX"));
    if (!workDir.isValid()) {
        result.error = QStringLiteral("could not create a temp directory for extracted images");
        return result;
    }
    workDir.setAutoRemove(false);

    // ASSUMPTION (unverified -- see header comment): the documented CLI has
    // no dedicated image-extraction flag for `markdown`. We speculatively
    // pass `--images-dir <dir>`, the most common naming convention for this
    // kind of option in comparable tools (e.g. pandoc's `--extract-media`),
    // and fall back to a plain `markdown <file>` call (text only, no images)
    // if the installed CLI rejects it as an unknown argument.
    ProcResult run = runOfficeOxide(
        binary, {QStringLiteral("markdown"), path, QStringLiteral("--images-dir"), workDir.path()}, kTimeoutMs);
    bool imagesRequested = true;

    if (run.started && !run.timedOut && run.exitCode != 0 && looksLikeUsageError(run.stdErr)) {
        imagesRequested = false;
        run = runOfficeOxide(binary, {QStringLiteral("markdown"), path}, kTimeoutMs);
    }

    if (!run.started || run.timedOut) {
        result.error = run.stdErr;
        QDir(workDir.path()).removeRecursively();
        return result;
    }
    if (run.exitCode != 0) {
        result.error = run.stdErr.isEmpty() ? QStringLiteral("office_oxide exited with code %1").arg(run.exitCode)
                                             : run.stdErr;
        QDir(workDir.path()).removeRecursively();
        return result;
    }

    result.ok = true;
    result.markdown = run.stdOut;
    if (imagesRequested) {
        result.workDir = workDir.path();
    } else {
        // Nothing was ever written into it; don't leave an orphaned empty dir.
        QDir(workDir.path()).removeRecursively();
    }
    return result;
}

OfficeConverter::Result OfficeConverter::convertSpreadsheet(const QString &binary, const QString &path) {
    Result result;
    result.kind = Kind::Spreadsheet;

    // ASSUMPTION (unverified -- see header comment): the documented
    // subcommand set (`text`, `markdown`, `html`, `info`, `ir`) has no `csv`
    // subcommand, even though the underlying library supports RFC-4180 CSV
    // output internally. We speculatively try `csv <file>`, mirroring the
    // naming of the other subcommands, and fall back to `text <file>` --
    // which at least yields readable cell text -- if the CLI rejects it.
    ProcResult run = runOfficeOxide(binary, {QStringLiteral("csv"), path}, kTimeoutMs);

    if (run.started && !run.timedOut && run.exitCode != 0 && looksLikeUsageError(run.stdErr)) {
        run = runOfficeOxide(binary, {QStringLiteral("text"), path}, kTimeoutMs);
    }

    if (!run.started || run.timedOut) {
        result.error = run.stdErr;
        return result;
    }
    if (run.exitCode != 0) {
        result.error = run.stdErr.isEmpty() ? QStringLiteral("office_oxide exited with code %1").arg(run.exitCode)
                                             : run.stdErr;
        return result;
    }

    result.ok = true;
    result.csv = run.stdOut;
    return result;
}
