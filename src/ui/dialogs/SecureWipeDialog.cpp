#include "SecureWipeDialog.h"
#include "ThemedDialogs.h"

#include <QByteArray>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStorageInfo>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QVector>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <io.h> // _get_osfhandle()
#else
#include <sys/stat.h>
#include <sys/sysmacros.h> // major()/minor()
#include <unistd.h>        // fsync()
#endif

namespace {

// Overwrite a mebibyte at a time so multi-gigabyte files never sit in memory.
constexpr qint64 kChunkSize = 1 << 20; // 1 MiB

// One concrete file to wipe: its size and how many overwrite passes its device
// wants. passes == 0 means "remove only" (symlinks -- overwriting would clobber
// the target, and the link itself holds no file data).
struct FileJob {
    QString path;
    qint64 size = 0;
    int passes = 0;
};

#ifndef Q_OS_WIN
// Passes for a device given its major:minor: 1 for a rotational disk, 3 for
// SSD/flash, 3 (the safer, more thorough choice) when the type can't be read.
int passesForDevice(unsigned maj, unsigned mn) {
    QDir d(QStringLiteral("/sys/dev/block/%1:%2").arg(maj).arg(mn));
    const QString canon = d.canonicalPath(); // resolve the sysfs symlink
    if (canon.isEmpty())
        return 3;
    // A partition dir (…/block/sda/sda2) has no queue/; its parent whole-disk
    // dir (…/block/sda) does. Climb until queue/rotational appears.
    QDir dir(canon);
    for (int i = 0; i < 4; ++i) {
        const QString rot = dir.filePath(QStringLiteral("queue/rotational"));
        if (QFile::exists(rot)) {
            QFile f(rot);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QByteArray v = f.readAll().trimmed();
                if (v == "1")
                    return 1; // rotational HDD
                if (v == "0")
                    return 3; // SSD / flash
            }
            return 3;
        }
        if (!dir.cdUp())
            break;
    }
    return 3;
}

// Passes for the device backing `path`, memoized by device id.
int passesForPath(const QString &path, QHash<QString, int> &cache) {
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return 3;
    const QString key = QStringLiteral("%1:%2").arg(major(st.st_dev)).arg(minor(st.st_dev));
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();
    const int passes = passesForDevice(major(st.st_dev), minor(st.st_dev));
    cache.insert(key, passes);
    return passes;
}
#else
// Windows does not expose a reliable rotational-media bit for every mounted
// volume. Use the conservative three-pass policy rather than guessing from the
// drive letter or silently weakening an erase on USB/SSD storage.
int passesForPath(const QString &path, QHash<QString, int> &cache) {
    Q_UNUSED(path);
    Q_UNUSED(cache);
    return 3;
}
#endif

// Flatten a selected path into the concrete files to wipe (recursing into
// directories) and the directories to remove afterwards (children before
// parents). Symlinks are removed but never followed or overwritten.
void gather(const QString &root, QVector<FileJob> &files, QStringList &dirs,
            QHash<QString, int> &cache) {
    const QFileInfo fi(root);
    if (fi.isSymLink()) {
        files.append({root, 0, 0}); // remove the link only
        return;
    }
    if (fi.isFile()) {
        files.append({root, fi.size(), passesForPath(root, cache)});
        return;
    }
    if (fi.isDir()) {
        const QDir dir(root);
        const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                               QDir::Hidden | QDir::System);
        for (const QFileInfo &e : entries)
            gather(e.absoluteFilePath(), files, dirs, cache);
        dirs.append(root); // appended after its children -> safe rmdir order
    }
}

} // namespace

// ---------------------------------------------------------------------------
// WipeWorker
// ---------------------------------------------------------------------------

WipeWorker::WipeWorker(QStringList paths, std::shared_ptr<std::atomic<bool>> cancel)
    : m_paths(std::move(paths)), m_cancel(std::move(cancel)) {}

void WipeWorker::process() {
    // Gather every concrete file (and directory to remove) per selected root,
    // and total up size x passes so the bar reflects the real overwrite work.
    QHash<QString, int> deviceCache;
    QVector<QVector<FileJob>> rootFiles(m_paths.size());
    QVector<QStringList> rootDirs(m_paths.size());
    qint64 total = 0;
    for (int i = 0; i < m_paths.size(); ++i) {
        gather(m_paths.at(i), rootFiles[i], rootDirs[i], deviceCache);
        for (const FileJob &job : rootFiles.at(i))
            total += job.size * job.passes;
    }

    qint64 done = 0;
    emit progress(done, total);

    QByteArray buf(static_cast<int>(kChunkSize), '\0');

    auto wipeOne = [&](const FileJob &job) -> bool {
        if (job.passes == 0 || job.size == 0)
            return QFile::remove(job.path); // symlink / empty file: just unlink

        QFile f(job.path);
        if (!f.open(QIODevice::ReadWrite))
            return false;
        const int fd = f.handle();

        for (int pass = 0; pass < job.passes; ++pass) {
            const bool randomPass = (pass == job.passes - 1); // last pass always random
            if (!randomPass)
                buf.fill(pass == 0 ? char(0x00) : char(0xFF));
            if (!f.seek(0)) {
                f.close();
                return false;
            }
            qint64 remaining = job.size;
            while (remaining > 0) {
                if (m_cancel->load()) {
                    f.close();
                    return false;
                }
                if (randomPass)
                    QRandomGenerator::system()->fillRange(
                        reinterpret_cast<quint32 *>(buf.data()),
                        static_cast<qsizetype>(kChunkSize / 4));
                const qint64 n = qMin<qint64>(kChunkSize, remaining);
                if (f.write(buf.constData(), n) != n) {
                    f.close();
                    return false;
                }
                remaining -= n;
                done += n;
                emit progress(done, total);
            }
            f.flush();
            if (fd >= 0) {
#ifdef Q_OS_WIN
                const intptr_t handle = _get_osfhandle(fd);
                if (handle != -1)
                    FlushFileBuffers(reinterpret_cast<HANDLE>(handle));
#else
                ::fsync(fd);
#endif
            }
        }
        f.close();
        return QFile::remove(job.path);
    };

    for (int i = 0; i < m_paths.size(); ++i) {
        if (m_cancel->load())
            return;
        bool ok = true;
        for (const FileJob &job : rootFiles.at(i)) {
            if (m_cancel->load())
                return;
            if (!wipeOne(job))
                ok = false;
        }
        // Remove emptied directories (children were appended before parents).
        for (const QString &d : rootDirs.at(i))
            if (!QDir().rmdir(d))
                ok = false;
        emit rowReady(i, ok ? tr("Wiped") : tr("Failed"));
    }

    emit progress(total, total);
    emit finished();
}

// ---------------------------------------------------------------------------
// SecureWipeDialog
// ---------------------------------------------------------------------------

namespace {
enum Column { ColFile = 0, ColStatus, ColCount };
}

SecureWipeDialog::SecureWipeDialog(const QStringList &paths, QWidget *parent)
    : FramelessDialog(parent), m_paths(paths), m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    buildUi();

    m_thread = new QThread(this);
    auto *worker = new WipeWorker(m_paths, m_cancel);
    worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, worker, &WipeWorker::process);
    connect(worker, &WipeWorker::rowReady, this, &SecureWipeDialog::onRowReady);
    connect(worker, &WipeWorker::progress, this, &SecureWipeDialog::onProgress);
    connect(worker, &WipeWorker::finished, this, &SecureWipeDialog::onFinished);
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    m_thread->start();
}

SecureWipeDialog::~SecureWipeDialog() { stopWorker(); }

void SecureWipeDialog::stopWorker() {
    if (!m_thread)
        return;
    m_cancel->store(true);
    m_thread->quit();
    m_thread->wait();
    m_thread = nullptr;
}

void SecureWipeDialog::buildUi() {
    setWindowTitle(tr("Secure Wipe"));
    resize(560, 360);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("Overwriting and deleting %1 item(s)…").arg(m_paths.size()), this));

    m_table = new QTableWidget(m_paths.size(), ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("File"), tr("Status")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(ColFile, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
    for (int row = 0; row < m_paths.size(); ++row) {
        auto *nameItem = new QTableWidgetItem(QFileInfo(m_paths.at(row)).fileName());
        nameItem->setToolTip(m_paths.at(row));
        m_table->setItem(row, ColFile, nameItem);
        m_table->setItem(row, ColStatus, new QTableWidgetItem(tr("…")));
    }
    layout->addWidget(m_table);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    layout->addWidget(m_progress);

    auto *buttons = new QDialogButtonBox(this);
    auto *closeButton = buttons->addButton(QDialogButtonBox::Close);
    ttc::localizeStandardButtons(buttons);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SecureWipeDialog::onRowReady(int row, const QString &status) {
    if (m_table && row >= 0 && row < m_table->rowCount())
        m_table->item(row, ColStatus)->setText(status);
}

void SecureWipeDialog::onProgress(qint64 done, qint64 total) {
    if (m_progress)
        m_progress->setValue(total > 0 ? static_cast<int>((done * 100) / total) : 100);
}

void SecureWipeDialog::onFinished() {
    if (m_progress)
        m_progress->setValue(100);
    emit filesChanged();
}
