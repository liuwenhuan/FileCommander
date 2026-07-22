#pragma once

#include "FramelessDialog.h"
#include <QStringList>

class QLineEdit;
class QCheckBox;
class QPushButton;
class QListWidget;
class QLabel;
class SearchEngine;

// Ctrl+F filename search dialog. Streams results into a list as
// SearchEngine finds them; double-clicking a result asks MainWindow to
// navigate the active panel there.
class SearchDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit SearchDialog(const QString &initialPath, QWidget *parent = nullptr);

signals:
    void navigateRequested(const QString &path);
    // Emitted by the "Send to panel" button with every result path, so the
    // active file panel can list them all TC "feed-to-listbox" style (they span
    // many directories, so a single navigate wouldn't show them together).
    void feedToPanelRequested(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    // Put keyboard focus on the name-pattern field (not the directory field)
    // whenever the dialog is shown, so the user can type a filename immediately.
    void showEvent(QShowEvent *event) override;

private slots:
    void startSearch();
    void onResultFound(const QString &path);
    void onFinished();
    void onResultActivated();
    void feedToPanel();

private:
    SearchEngine *m_engine;
    QLineEdit *m_pathEdit;
    QLineEdit *m_patternEdit;
    QCheckBox *m_caseSensitiveCheck;
    QCheckBox *m_subdirsCheck;
    QPushButton *m_searchButton;
    QPushButton *m_feedButton;
    QListWidget *m_resultsList;
    QLabel *m_statusLabel;
    bool m_closePending = false;
    int m_resultCount = 0;
};
