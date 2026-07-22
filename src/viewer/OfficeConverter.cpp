#include "OfficeConverter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
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

ProcResult runOfficeOxide(const QString &binary, const QStringList &args, int timeoutMs,
                          const QString &password) {
    ProcResult r;
    QProcess proc;
    // Hand the password to office_oxide out-of-band (never on argv, where it would
    // show up in the process list). Empty means "no password supplied".
    if (!password.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("OFFICE_OXIDE_PASSWORD"), password);
        proc.setProcessEnvironment(env);
    }
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

namespace {
// Maps office_oxide's encryption error strings (see its OfficeError Display impl)
// onto our Encryption states. Returns None for any non-encryption failure.
OfficeConverter::Encryption classifyEncryption(const QString &stderrText) {
    if (stderrText.contains(QStringLiteral("incorrect password"), Qt::CaseInsensitive))
        return OfficeConverter::Encryption::WrongPassword;
    if (stderrText.contains(QStringLiteral("format not supported"), Qt::CaseInsensitive))
        return OfficeConverter::Encryption::Unsupported;
    if (stderrText.contains(QStringLiteral("document is encrypted"), Qt::CaseInsensitive) ||
        stderrText.contains(QStringLiteral("encrypted"), Qt::CaseInsensitive))
        return OfficeConverter::Encryption::NeedsPassword;
    return OfficeConverter::Encryption::None;
}
} // namespace

OfficeConverter::Result OfficeConverter::convert(const QString &path, const QString &password) {
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
            "office_oxide CLI not found. Install it with "
            "`cargo install --git https://github.com/yfedoseev/office_oxide office_oxide_cli`, "
            "ensure the `office-oxide` binary is on PATH, or set TTC_OFFICE_OXIDE to its full path.");
        return result;
    }

    // PowerPoint: try the per-slide SVG render first. A non-empty result is the
    // real slide-image preview; on an encrypted pptx we return the encryption
    // state so the UI can prompt for a password. Only a clean empty result (`[]`
    // -- legacy .ppt, or a pptx the renderer can't turn into slides) falls through
    // to the flattened text (html) preview below.
    const QString suffix = suffixLower(path);
    if (suffix == QLatin1String("ppt") || suffix == QLatin1String("pptx")) {
        Result pres = convertPresentation(binary, path, password);
        if (!pres.ok)
            pres.encryption = classifyEncryption(pres.error);
        if (pres.ok && !pres.slideSvgs.isEmpty())
            return pres;
        if (pres.encrypted())
            return pres;
        // else: empty `[]` -> fall through to convertDocument (text preview).
    }

    Result r = (result.kind == Kind::Document) ? convertDocument(binary, path, password)
                                               : convertSpreadsheet(binary, path, password);
    // office_oxide reports encryption (OOXML wrappers and legacy .doc fEncrypted
    // alike) through its exit error text; classify it so the UI can prompt for a
    // password inline or report a wrong one.
    if (!r.ok)
        r.encryption = classifyEncryption(r.error);
    return r;
}

OfficeConverter::Result OfficeConverter::convertPresentation(const QString &binary,
                                                             const QString &path,
                                                             const QString &password) {
    Result result;
    result.kind = Kind::Presentation;

    // PowerPoint → per-slide SVG (`office-oxide svg <file>`): stdout is a JSON
    // array of standalone SVG document strings, one per slide. Non-pptx (incl.
    // legacy .ppt) and any pptx the renderer can't handle print `[]`.
    const ProcResult run = runOfficeOxide(binary, {QStringLiteral("svg"), path}, kTimeoutMs, password);
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

    const QJsonDocument doc = QJsonDocument::fromJson(run.stdOut.toUtf8());
    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (const QJsonValue &v : arr) {
            if (v.isString())
                result.slideSvgs << v.toString();
        }
    }
    // ok only when there is at least one slide; an empty array leaves ok=false so
    // convert() falls back to the flattened text preview.
    result.ok = !result.slideSvgs.isEmpty();
    return result;
}

OfficeConverter::Result OfficeConverter::convertDocument(const QString &binary, const QString &path,
                                                         const QString &password) {
    Result result;
    result.kind = Kind::Document;

    // Word / PowerPoint → HTML (`office-oxide html <file>`): block-structured
    // (<h1>/<p>/<strong>/<table>), which QTextBrowser renders faithfully.
    // office_oxide emits empty `<img>` placeholders (it doesn't export image
    // bytes over the CLI).
    const ProcResult run = runOfficeOxide(binary, {QStringLiteral("html"), path}, kTimeoutMs, password);
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

OfficeConverter::Result OfficeConverter::convertSpreadsheet(const QString &binary, const QString &path,
                                                            const QString &password) {
    Result result;
    result.kind = Kind::Spreadsheet;

    // Excel → tab-separated cell text (`office-oxide text <file>`). The CLI has
    // no CSV subcommand, but `text` yields clean TSV (one row per line, cells
    // separated by tabs, commas kept literal) which the viewer renders as a
    // grid. (`markdown` would give a titled table too, but TSV maps 1:1 to
    // cells without any table-syntax parsing.)
    const ProcResult run = runOfficeOxide(binary, {QStringLiteral("text"), path}, kTimeoutMs, password);
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
