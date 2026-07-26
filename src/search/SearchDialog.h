#pragma once

#include "FramelessDialog.h"
#include "SearchEngine.h"

#include <QStringList>
#include <QVector>
#include <memory>

class QLineEdit;
class QCheckBox;
class QPushButton;
class QListWidget;
class QLabel;
class FileProvider;

// Ctrl+F filename search dialog. Streams results into a list as
// SearchEngine finds them; double-clicking a result asks MainWindow to
// navigate the active panel there.
class SearchDialog : public FramelessDialog {
    Q_OBJECT

public:
    // `provider` is the backend the panel is browsing, and must be non-null
    // exactly when that is a network tab: those tabs address provider-internal
    // paths that do not exist on this machine, so the search has to go through
    // the backend instead of the local filesystem. Held by shared_ptr because
    // the search outlives neither more nor less than itself -- the tab may be
    // closed while it runs. Null keeps the local (QDirIterator) search.
    explicit SearchDialog(const QString &initialPath, std::shared_ptr<FileProvider> provider = {},
                          QWidget *parent = nullptr);

signals:
    // isDir comes from the walk itself; the receiver cannot recover it for a
    // remote path (see SearchHit).
    void navigateRequested(const QString &path, bool isDir);
    // Emitted by the "Send to panel" button with the search keyword and every
    // result path, so the active file panel can open them as a flat
    // "feed-to-listbox" listing in a new tab titled after the keyword (they span
    // many directories, so a single navigate wouldn't show them together).
    void feedToPanelRequested(const QString &keyword, const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    // ESC / reject() reach a QDialog through done(), not closeEvent() -- and
    // done()'s WA_DeleteOnClose handling would delete us (and the child
    // SearchEngine) while a background search still runs. Override it to route
    // through the same "cancel then defer teardown" guard as closeEvent().
    void done(int r) override;
    // Put keyboard focus on the name-pattern field (not the directory field)
    // whenever the dialog is shown, so the user can type a filename immediately.
    void showEvent(QShowEvent *event) override;

private slots:
    // Toggles between starting a search and stopping the running one, matching
    // the search button's current label.
    void onSearchButtonClicked();
    void startSearch();
    void onResultsFound(const QVector<SearchHit> &hits);
    void onScanning(const QString &dir);
    void onFinished();
    void onResultActivated();
    void feedToPanel();

private:
    // Truncates a long path from the left for the status line, so the directory
    // name (the part that shows progress) stays visible.
    QString elideDir(const QString &dir) const;

    std::shared_ptr<FileProvider> m_provider; // null for a local search
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
