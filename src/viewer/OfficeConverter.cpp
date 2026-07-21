#include "OfficeConverter.h"

#include <archive.h>
#include <archive_entry.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QVector>

#include <algorithm>

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

// One embedded image pulled from an OOXML media folder. `mime` is empty for
// formats QTextBrowser can't render (emf/wmf); such slots are kept so injection
// stays position-aligned with the HTML's <img> tags, but emit nothing.
struct MediaImage {
    QString mime;
    QByteArray data;
};

// Returns the browser-friendly MIME type for a media file name, or "" if it's a
// vector/metafile format Qt can't display inline.
QString mimeForImage(const QString &lowerName) {
    if (lowerName.endsWith(QLatin1String(".png")))
        return QStringLiteral("image/png");
    if (lowerName.endsWith(QLatin1String(".jpg")) || lowerName.endsWith(QLatin1String(".jpeg")))
        return QStringLiteral("image/jpeg");
    if (lowerName.endsWith(QLatin1String(".gif")))
        return QStringLiteral("image/gif");
    if (lowerName.endsWith(QLatin1String(".bmp")))
        return QStringLiteral("image/bmp");
    if (lowerName.endsWith(QLatin1String(".webp")))
        return QStringLiteral("image/webp");
    if (lowerName.endsWith(QLatin1String(".tif")) || lowerName.endsWith(QLatin1String(".tiff")))
        return QStringLiteral("image/tiff");
    return QString(); // emf / wmf / svg-as-vector / unknown
}

// Trailing integer of a media file's base name, e.g. "word/media/image10.png" →
// 10. OOXML numbers media in insertion (document) order, so sorting by this
// yields the same order office_oxide renders the corresponding <img> tags.
int mediaNumber(const QString &path) {
    const QString base = path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
    const QRegularExpression re(QStringLiteral("([0-9]+)$"));
    const QRegularExpressionMatch m = re.match(base);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

// Extracts embedded raster images from an OOXML file (docx/pptx/xlsx are zip
// containers with a <part>/media/ folder), ordered to line up with the HTML's
// <img> tags. Empty for legacy binary .doc/.ppt/.xls (not zips -- office_oxide
// recognises their images but exports no bytes) or for a non-zip/unreadable file.
QVector<MediaImage> extractOoxmlMediaImages(const QString &officePath) {
    QVector<MediaImage> result;

    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, officePath.toUtf8().constData(), 65536) != ARCHIVE_OK) {
        archive_read_free(a);
        return result;
    }

    struct Named {
        QString name;
        QString mime;
        QByteArray data;
    };
    QVector<Named> found;
    struct archive_entry *entry = nullptr;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        const QString lower = name.toLower();
        if (!lower.contains(QStringLiteral("/media/"))) {
            archive_read_data_skip(a);
            continue;
        }
        const QString mime = mimeForImage(lower);
        QByteArray buf;
        const la_int64_t size = archive_entry_size(entry);
        if (size > 0 && size < (64LL << 20)) { // cap a single image at 64 MiB
            buf.resize(int(size));
            const la_ssize_t n = archive_read_data(a, buf.data(), size_t(size));
            buf.truncate(n > 0 ? int(n) : 0);
        }
        found.push_back({name, mime, buf});
    }
    archive_read_free(a);

    std::sort(found.begin(), found.end(),
              [](const Named &x, const Named &y) { return mediaNumber(x.name) < mediaNumber(y.name); });
    for (const Named &f : found)
        result.push_back({f.mime, f.mime.isEmpty() ? QByteArray() : f.data});
    return result;
}

// Replaces office_oxide's empty <img .../> placeholders with the extracted
// media images, in order, as self-contained base64 data URIs. Placeholders past
// the available images (or for unrenderable formats) are dropped.
QString inlineImages(const QString &html, const QVector<MediaImage> &images) {
    const QRegularExpression imgRe(QStringLiteral("<img\\b[^>]*>"));
    QString out;
    int last = 0;
    int idx = 0;
    QRegularExpressionMatchIterator it = imgRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        if (idx < images.size() && !images.at(idx).data.isEmpty()) {
            out += QStringLiteral("<img src=\"data:%1;base64,%2\" />")
                       .arg(images.at(idx).mime,
                            QString::fromLatin1(images.at(idx).data.toBase64()));
        }
        last = m.capturedEnd();
        ++idx;
    }
    out += html.mid(last);
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

    const QString binary = resolveBinary();
    if (binary.isEmpty()) {
        result.error = QStringLiteral(
            "office_oxide CLI not found. Install it with "
            "`cargo install --git https://github.com/yfedoseev/office_oxide office_oxide_cli`, "
            "ensure the `office-oxide` binary is on PATH, or set TTC_OFFICE_OXIDE to its full path.");
        return result;
    }

    return (result.kind == Kind::Document) ? convertDocument(binary, path) : convertSpreadsheet(binary, path);
}

OfficeConverter::Result OfficeConverter::convertDocument(const QString &binary, const QString &path) {
    Result result;
    result.kind = Kind::Document;

    // Word / PowerPoint → HTML (`office-oxide html <file>`): block-structured
    // (<h1>/<p>/<strong>/<table>), which QTextBrowser renders faithfully.
    // office_oxide emits empty `<img>` placeholders (it doesn't export image
    // bytes over the CLI); for OOXML files we pull the embedded images straight
    // out of the zip container and inline them as base64 data URIs, in document
    // order. Legacy .doc/.ppt (not zips) have no extractable media, so their
    // placeholders are simply dropped.
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

    result.ok = true;
    result.html = inlineImages(run.stdOut, extractOoxmlMediaImages(path));
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
