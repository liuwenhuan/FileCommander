#include "OfficeConverter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

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

// Removes all <img> tags from HTML. office_oxide (our patched build) already
// inlines embedded images as data: URIs, but PowerPoint places pictures by slide
// geometry rather than text order, so they don't line up with the flattened text
// preview -- pptx/ppt drop them.
QString stripImgTags(const QString &html) {
    QString out = html;
    out.remove(QRegularExpression(QStringLiteral("<img\\b[^>]*>")));
    return out;
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
    // Encrypted OOXML is an OLE2 wrapper (detectable up front); office_oxide can't
    // decrypt, so report it before even launching the CLI.
    if (isEncrypted(path)) {
        result.encrypted = true;
        result.error = QStringLiteral("The document is encrypted.");
        return result;
    }

    const QString binary = resolveBinary();
    if (binary.isEmpty()) {
        result.error = QStringLiteral(
            "office_oxide CLI not found. Install it with "
            "`cargo install --git https://github.com/yfedoseev/office_oxide office_oxide_cli`, "
            "ensure the `office-oxide` binary is on PATH, or set TTC_OFFICE_OXIDE to its full path.");
        return result;
    }

    Result r =
        (result.kind == Kind::Document) ? convertDocument(binary, path) : convertSpreadsheet(binary, path);
    // Legacy .doc/.xls encryption is detected by office_oxide (fEncrypted), which
    // surfaces as an "... encrypted" error; flag it so the UI shows the right note.
    if (!r.ok && r.error.contains(QStringLiteral("encrypt"), Qt::CaseInsensitive))
        r.encrypted = true;
    return r;
}

bool OfficeConverter::isEncrypted(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(8);
    // OLE2 / Compound File Binary magic (encrypted OOXML is wrapped in one).
    static const QByteArray kOle2 = QByteArray::fromHex(QByteArrayLiteral("d0cf11e0a1b11ae1"));
    if (!head.startsWith(kOle2))
        return false; // a zip-based OOXML or other file: not container-encrypted
    // An OLE2 file with an OOXML extension is an encrypted package (a normal
    // docx/xlsx/pptx is a zip). Legacy .doc/.xls/.ppt are OLE2 normally, so their
    // encryption is left to office_oxide's fEncrypted detection.
    const QString suffix = suffixLower(path);
    return suffix == QLatin1String("docx") || suffix == QLatin1String("xlsx") ||
           suffix == QLatin1String("pptx");
}

OfficeConverter::Result OfficeConverter::convertDocument(const QString &binary, const QString &path) {
    Result result;
    result.kind = Kind::Document;

    // Word / PowerPoint → HTML (`office-oxide html <file>`): block-structured
    // (<h1>/<p>/<strong>/<table>), which QTextBrowser renders faithfully.
    // office_oxide emits empty `<img>` placeholders (it doesn't export image
    // bytes over the CLI).
    const ProcResult run = runOfficeOxide(binary, {QStringLiteral("html"), path}, kTimeoutMs);
    if (!run.started || run.timedOut) {
        result.error = run.stdErr;
        return result;
    }
    if (run.exitCode != 0) {
        result.error = run.stdErr.isEmpty()
                           ? QStringLiteral("office_oxide exited with code %1").arg(run.exitCode)
                           : run.stdErr;
        return result;
    }

    // Our patched office_oxide already inlines embedded images as data: URIs
    // (docx/doc from word/media & the .doc Data-stream BLIPs alike), so Word
    // documents just pass the HTML through. PowerPoint images are positioned by
    // slide geometry, not text order, so they don't line up with the flattened
    // text preview -- pptx/ppt strip them.
    const QString suffix = suffixLower(path);
    const bool isPresentation =
        (suffix == QLatin1String("ppt") || suffix == QLatin1String("pptx"));
    result.ok = true;
    result.html = isPresentation ? stripImgTags(run.stdOut) : run.stdOut;
    return result;
}

OfficeConverter::Result OfficeConverter::convertSpreadsheet(const QString &binary, const QString &path) {
    Result result;
    result.kind = Kind::Spreadsheet;

    // Excel → tab-separated cell text (`office-oxide text <file>`). The CLI has
    // no CSV subcommand, but `text` yields clean TSV (one row per line, cells
    // separated by tabs, commas kept literal) which the viewer renders as a
    // grid. (`markdown` would give a titled table too, but TSV maps 1:1 to
    // cells without any table-syntax parsing.)
    const ProcResult run = runOfficeOxide(binary, {QStringLiteral("text"), path}, kTimeoutMs);
    if (!run.started || run.timedOut) {
        result.error = run.stdErr;
        return result;
    }
    if (run.exitCode != 0) {
        result.error = run.stdErr.isEmpty()
                           ? QStringLiteral("office_oxide exited with code %1").arg(run.exitCode)
                           : run.stdErr;
        return result;
    }

    result.ok = true;
    result.tsv = run.stdOut; // tab-separated
    return result;
}
