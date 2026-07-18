#pragma once

#include <QDialog>

class QTableView;
class QLabel;
class ArchiveModel;

// Standalone browse/extract window opened by double-clicking a supported
// archive in a FilePanel. Browsing (double-click into subdirs, "..") and
// extraction are both wired in from the start -- unlike the old project,
// which shipped the libarchive engine without ever hooking up the UI.
class ArchiveBrowserDialog : public QDialog {
    Q_OBJECT

public:
    // defaultExtractDir is typically the *other* panel's current path,
    // mirroring how F5 copy works between the two main panels.
    ArchiveBrowserDialog(const QString &archivePath, const QString &defaultExtractDir,
                          QWidget *parent = nullptr);

private slots:
    void onActivated(const QModelIndex &index);
    void navigateUp();
    void extractSelectedToDefault();
    void extractSelectedToChosenDir();
    void showContextMenu(const QPoint &pos);

private:
    QStringList selectedEntryPaths() const;
    bool doExtract(const QString &destDir);
    void updatePathLabel();

    ArchiveModel *m_model;
    QTableView *m_view;
    QLabel *m_pathLabel;
    QString m_defaultExtractDir;
};
