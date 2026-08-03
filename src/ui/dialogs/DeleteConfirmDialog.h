#pragma once

#include "FramelessDialog.h"

#include <QStringList>

class FileSystemModel;
class DirectoryStatisticsTask;
class QLabel;

// What a delete selection consists of, taken from the listing the panel already
// holds. Folders contribute nothing to `listedBytes` -- a directory's own entry
// has no meaningful size, and what it contains is not known without a walk --
// which is exactly why the dialog measures rather than reporting this number as
// the answer.
struct DeleteSelectionSummary {
    int fileCount = 0;
    int folderCount = 0;
    qint64 listedBytes = 0; // sum of the listed sizes of the selected files
};

// Reads the summary out of `model`'s listing rather than out of QFileInfo: on a
// network tab the paths are the server's, where a QFileInfo describes a
// same-named LOCAL file or nothing at all.
DeleteSelectionSummary summarizeDeleteSelection(const FileSystemModel *model,
                                                const QStringList &paths);

// The delete confirmation. Its reason to exist over a plain message box is the
// size: selecting folders used to contribute zero bytes to the total, so
// "delete 5 items (2.1 KB)" could stand in front of gigabytes. Local selections
// are measured for real, in the background, and the figure fills in while the
// dialog is already on screen -- the user is never made to wait for a number
// before being allowed to answer.
class DeleteConfirmDialog : public FramelessDialog {
    Q_OBJECT

public:
    // `measureLocally` is false for a network or archive tab, where the paths
    // are not this filesystem's and walking them would mean thousands of round
    // trips to answer a question the user is about to dismiss. Those show what
    // the listing knows, and say so.
    DeleteConfirmDialog(const QStringList &paths, const DeleteSelectionSummary &summary,
                        bool permanent, bool measureLocally, QWidget *parent = nullptr);

    static bool ask(QWidget *parent, const QStringList &paths,
                    const DeleteSelectionSummary &summary, bool permanent, bool measureLocally);

    QString summaryText() const;
    QString sizeText() const;
    bool isMeasuring() const { return m_measuring; }

signals:
    // The background walk has finished and sizeText() is now the real total.
    void measurementFinished();

private:
    // How much of the truth the figure on screen currently is.
    enum class SizeState {
        Exact,      // nothing selected needs walking
        Measuring,  // folders are being walked; the number will grow
        FilesOnly,  // folders are selected and cannot be walked from here
    };
    void showSize(qint64 bytes, SizeState state);

    DeleteSelectionSummary m_summary;
    bool m_measureLocally = false;
    bool m_measuring = false;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_sizeLabel = nullptr;
    DirectoryStatisticsTask *m_task = nullptr;
};
