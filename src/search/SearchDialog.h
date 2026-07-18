#pragma once

#include <QDialog>

class QLineEdit;
class QCheckBox;
class QPushButton;
class QListWidget;
class QLabel;
class SearchEngine;

// Ctrl+F filename search dialog. Streams results into a list as
// SearchEngine finds them; double-clicking a result asks MainWindow to
// navigate the active panel there.
class SearchDialog : public QDialog {
    Q_OBJECT

public:
    explicit SearchDialog(const QString &initialPath, QWidget *parent = nullptr);

signals:
    void navigateRequested(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void startSearch();
    void onResultFound(const QString &path);
    void onFinished();
    void onResultActivated();

private:
    SearchEngine *m_engine;
    QLineEdit *m_pathEdit;
    QLineEdit *m_patternEdit;
    QCheckBox *m_caseSensitiveCheck;
    QCheckBox *m_subdirsCheck;
    QPushButton *m_searchButton;
    QListWidget *m_resultsList;
    QLabel *m_statusLabel;
    bool m_closePending = false;
    int m_resultCount = 0;
};
