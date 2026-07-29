#include "ChecksumDialog.h"
#include "ThemedDialogs.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialogButtonBox>

#include "FileProvider.h"
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QMenu>
#include <QShortcut>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#include <zlib.h>

namespace {

// Read files a mebibyte at a time so multi-gigabyte inputs never sit in memory
// all at once.
constexpr qint64 kChunkSize = 1 << 20; // 1 MiB

enum Column { ColFile = 0, ColMd5, ColCrc32, ColSha1, ColCount };

quint64 cellKey(int row, int column) {
    return (static_cast<quint64>(static_cast<quint32>(row)) << 32) |
           static_cast<quint32>(column);
}

int progressPercent(qint64 done, qint64 total) {
    if (total <= 0)
        return 100;
    if (done <= 0)
        return 0;
    if (done >= total)
        return 100;
    const long double ratio = static_cast<long double>(done) / static_cast<long double>(total);
    return qBound(0, static_cast<int>(ratio * 100.0L), 100);
}

} // namespace

// ---------------------------------------------------------------------------
// ChecksumWorker
// ---------------------------------------------------------------------------

ChecksumWorker::ChecksumWorker(QStringList paths, std::shared_ptr<std::atomic<bool>> cancel)
    : m_paths(std::move(paths)), m_cancel(std::move(cancel)) {}

ChecksumWorker::ChecksumWorker(QVector<FileInfo> infos, std::shared_ptr<FileProvider> provider,
                               std::shared_ptr<std::atomic<bool>> cancel)
    : m_infos(std::move(infos)), m_provider(std::move(provider)), m_cancel(std::move(cancel)) {
    m_paths.reserve(m_infos.size());
    for (const FileInfo &info : m_infos)
        m_paths.append(info.path());
}

void ChecksumWorker::process() {
    if (m_provider)
        processProvider();
    else
        processLocal();
}

void ChecksumWorker::processProvider() {
    // Sizes come from the listing the panel already has -- no stat over the
    // wire just to fill a progress bar.
    qint64 total = 0;
    for (const FileInfo &info : m_infos)
        if (!info.isDir())
            total += info.size();

    qint64 done = 0;
    emit progress(done, total);

    QByteArray buf;
    buf.resize(static_cast<int>(kChunkSize));

    for (int row = 0; row < m_infos.size(); ++row) {
        if (m_cancel->load())
            return;

        const FileInfo &info = m_infos.at(row);
        if (info.isDir()) {
            const QString dir = tr("(directory)");
            emit rowReady(row, dir, dir, dir);
            continue;
        }

        FileHandle *handle = m_provider->openRead(info.path());
        if (!handle) {
            const QString err = tr("(unreadable)");
            emit rowReady(row, err, err, err);
            continue;
        }

        QCryptographicHash md5(QCryptographicHash::Md5);
        QCryptographicHash sha1(QCryptographicHash::Sha1);
        uLong crc = crc32(0L, Z_NULL, 0);
        bool failed = false;

        while (true) {
            if (m_cancel->load()) {
                m_provider->closeHandle(handle);
                return;
            }
            const qint64 n = m_provider->read(handle, buf.data(), buf.size());
            if (n < 0) { // read error, as distinct from the 0 that means EOF
                failed = true;
                break;
            }
            if (n == 0)
                break;

            md5.addData(buf.constData(), static_cast<int>(n));
            sha1.addData(buf.constData(), static_cast<int>(n));
            crc = crc32(crc, reinterpret_cast<const Bytef *>(buf.constData()),
                        static_cast<uInt>(n));

            done += n;
            emit progress(done, total);
        }
        m_provider->closeHandle(handle);

        if (failed) {
            const QString err = tr("(read error)");
            emit rowReady(row, err, err, err);
            continue;
        }

        const QString md5Hex = QString::fromLatin1(md5.result().toHex());
        const QString sha1Hex = QString::fromLatin1(sha1.result().toHex());
        const QString crcHex =
            QStringLiteral("%1").arg(static_cast<quint32>(crc), 8, 16, QLatin1Char('0')).toUpper();

        emit rowReady(row, md5Hex, crcHex, sha1Hex);
    }

    emit progress(total, total);
    emit finished();
}

void ChecksumWorker::processLocal() {
    // First pass: total up the bytes we expect to read so the progress bar has
    // a meaningful denominator. Directories and stat failures contribute zero.
    qint64 total = 0;
    for (const QString &path : m_paths) {
        const QFileInfo info(path);
        if (info.isFile())
            total += info.size();
    }

    qint64 done = 0;
    emit progress(done, total);

    for (int row = 0; row < m_paths.size(); ++row) {
        if (m_cancel->load())
            return;

        const QString &path = m_paths.at(row);
        const QFileInfo info(path);

        if (info.isDir()) {
            const QString dir = tr("(directory)");
            emit rowReady(row, dir, dir, dir);
            continue;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            const QString err = tr("(unreadable)");
            emit rowReady(row, err, err, err);
            continue;
        }

        QCryptographicHash md5(QCryptographicHash::Md5);
        QCryptographicHash sha1(QCryptographicHash::Sha1);
        uLong crc = crc32(0L, Z_NULL, 0);
        bool failed = false;

        while (!file.atEnd()) {
            if (m_cancel->load()) {
                file.close();
                return;
            }

            const QByteArray chunk = file.read(kChunkSize);
            if (chunk.isEmpty()) {
                // Empty read before EOF means an I/O error.
                if (!file.atEnd())
                    failed = true;
                break;
            }

            md5.addData(chunk);
            sha1.addData(chunk);
            crc = crc32(crc, reinterpret_cast<const Bytef *>(chunk.constData()),
                        static_cast<uInt>(chunk.size()));

            done += chunk.size();
            emit progress(done, total);
        }
        file.close();

        if (failed || file.error() != QFile::NoError) {
            const QString err = tr("(read error)");
            emit rowReady(row, err, err, err);
            continue;
        }

        const QString md5Hex = QString::fromLatin1(md5.result().toHex());
        const QString sha1Hex = QString::fromLatin1(sha1.result().toHex());
        const QString crcHex =
            QStringLiteral("%1").arg(static_cast<quint32>(crc), 8, 16, QLatin1Char('0')).toUpper();

        emit rowReady(row, md5Hex, crcHex, sha1Hex);
    }

    emit progress(total, total);
    emit finished();
}

// ---------------------------------------------------------------------------
// ChecksumDialog
// ---------------------------------------------------------------------------

ChecksumDialog::ChecksumDialog(const QStringList &paths, QWidget *parent)
    : FramelessDialog(parent), m_paths(paths), m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    buildUi();
    startWorker(new ChecksumWorker(m_paths, m_cancel));
}

ChecksumDialog::ChecksumDialog(const QVector<FileInfo> &infos,
                               std::shared_ptr<FileProvider> provider, QWidget *parent)
    : FramelessDialog(parent), m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    m_paths.reserve(infos.size());
    for (const FileInfo &info : infos)
        m_paths.append(info.path());
    buildUi();
    startWorker(new ChecksumWorker(infos, std::move(provider), m_cancel));
}

void ChecksumDialog::startWorker(ChecksumWorker *worker) {
    // Spin up the worker on its own thread. The worker owns nothing that the UI
    // touches; it only communicates through queued signals.
    m_thread = new QThread(this);
    worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, worker, &ChecksumWorker::process);
    connect(worker, &ChecksumWorker::rowReady, this, &ChecksumDialog::onRowReady);
    connect(worker, &ChecksumWorker::progress, this, &ChecksumDialog::onProgress);
    connect(worker, &ChecksumWorker::finished, this, &ChecksumDialog::onFinished);
    // Tear the worker down once the thread's event loop stops.
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);

    m_thread->start();
}

ChecksumDialog::~ChecksumDialog() {
    stopWorker();
}

void ChecksumDialog::stopWorker() {
    if (!m_thread)
        return;
    // Signal the read loop to bail, then let the event loop drain and join.
    m_cancel->store(true);
    m_thread->quit();
    m_thread->wait();
    m_thread = nullptr;
}

void ChecksumDialog::buildUi() {
    setWindowTitle(tr("Checksums"));
    resize(720, 400);

    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(tr("Computing MD5, CRC32 and SHA1 for %1 file(s)…").arg(m_paths.size()),
                            this);
    layout->addWidget(info);

    m_table = new QTableWidget(m_paths.size(), ColCount, this);
    m_table->setHorizontalHeaderLabels(
        {tr("File"), tr("MD5"), tr("CRC32"), tr("SHA1")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->horizontalHeader()->setSectionResizeMode(ColFile, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColMd5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColCrc32, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColSha1, QHeaderView::ResizeToContents);

    for (int row = 0; row < m_paths.size(); ++row) {
        auto *nameItem = new QTableWidgetItem(QFileInfo(m_paths.at(row)).fileName());
        nameItem->setToolTip(m_paths.at(row));
        m_table->setItem(row, ColFile, nameItem);
        const QString pending = tr("…");
        m_table->setItem(row, ColMd5, new QTableWidgetItem(pending));
        m_table->setItem(row, ColCrc32, new QTableWidgetItem(pending));
        m_table->setItem(row, ColSha1, new QTableWidgetItem(pending));
    }
    layout->addWidget(m_table);

    auto *copySelectionShortcut = new QShortcut(QKeySequence::Copy, m_table);
    copySelectionShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySelectionShortcut, &QShortcut::activated, this, &ChecksumDialog::copySelection);
    connect(m_table, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        QAction *copy = menu.addAction(tr("Copy"));
        connect(copy, &QAction::triggered, this, &ChecksumDialog::copySelection);
        menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progressPercent = new QLabel(QStringLiteral("0%"), this);
    m_progressPercent->setObjectName(QStringLiteral("ChecksumProgressPercent"));
    auto *progressRow = new QHBoxLayout;
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_progressPercent);
    layout->addLayout(progressRow);

    auto *buttons = new QDialogButtonBox(this);
    m_copyButton = buttons->addButton(tr("Copy all"), QDialogButtonBox::ActionRole);
    m_copyButton->setEnabled(false);
    auto *closeButton = buttons->addButton(QDialogButtonBox::Close);
    ttc::localizeStandardButtons(buttons);
    connect(m_copyButton, &QPushButton::clicked, this, &ChecksumDialog::copyAll);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ChecksumDialog::onRowReady(int row, const QString &md5, const QString &crc32,
                                const QString &sha1) {
    if (!m_table || row < 0 || row >= m_table->rowCount())
        return;
    m_table->item(row, ColMd5)->setText(md5);
    m_table->item(row, ColCrc32)->setText(crc32);
    m_table->item(row, ColSha1)->setText(sha1);
}

void ChecksumDialog::onProgress(qint64 done, qint64 total) {
    if (!m_progress)
        return;
    const int percent = progressPercent(done, total);
    m_progress->setValue(percent);
    if (m_progressPercent)
        m_progressPercent->setText(QStringLiteral("%1%").arg(percent));
}

void ChecksumDialog::onFinished() {
    if (m_progress)
        m_progress->setValue(100);
    if (m_progressPercent)
        m_progressPercent->setText(QStringLiteral("100%"));
    if (m_copyButton)
        m_copyButton->setEnabled(true);
}

void ChecksumDialog::copySelection() {
    if (!m_table || !m_table->selectionModel())
        return;

    QModelIndexList indexes = m_table->selectionModel()->selectedIndexes();
    if (indexes.isEmpty())
        return;
    if (indexes.size() == 1) {
        QApplication::clipboard()->setText(indexes.first().data().toString());
        return;
    }

    int firstRow = indexes.first().row();
    int lastRow = firstRow;
    int firstColumn = indexes.first().column();
    int lastColumn = firstColumn;
    QHash<quint64, QString> selectedValues;
    for (const QModelIndex &index : indexes) {
        firstRow = qMin(firstRow, index.row());
        lastRow = qMax(lastRow, index.row());
        firstColumn = qMin(firstColumn, index.column());
        lastColumn = qMax(lastColumn, index.column());
        selectedValues.insert(cellKey(index.row(), index.column()), index.data().toString());
    }

    QStringList lines;
    for (int row = firstRow; row <= lastRow; ++row) {
        QStringList cells;
        for (int column = firstColumn; column <= lastColumn; ++column)
            cells.append(selectedValues.value(cellKey(row, column)));
        lines.append(cells.join(QLatin1Char('\t')));
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void ChecksumDialog::copyAll() {
    if (!m_table)
        return;
    QStringList lines;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QString name = m_table->item(row, ColFile)->text();
        const QString md5 = m_table->item(row, ColMd5)->text();
        const QString crc = m_table->item(row, ColCrc32)->text();
        const QString sha1 = m_table->item(row, ColSha1)->text();
        lines << QStringLiteral("%1\tMD5:%2\tCRC32:%3\tSHA1:%4")
                     .arg(name, md5, crc, sha1);
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}
