#pragma once

#include <QMainWindow>

class FilePanel;
class FunctionKeyBar;
class StatusBarWidget;
class OperationQueue;
class OperationProgressDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void setActivePanel(FilePanel *panel);
    void updateStatusBar();

    void viewCurrent();  // F3
    void editCurrent();  // F4 (stub for now, Phase 2 adds TextEditor)
    void copySelected();  // F5
    void moveSelected();  // F6
    void makeDirectory();// F7
    void deleteSelected(bool permanent = false); // F8 / Shift+F8
    void renameCurrent(); // F2

    void navigateBack();
    void navigateForward();
    void navigateUp();
    void refreshActivePanel();

private:
    void setupMenuAndToolbar();
    void setupShortcuts();
    FilePanel *otherPanel(FilePanel *panel) const;

    FilePanel *m_leftPanel;
    FilePanel *m_rightPanel;
    FilePanel *m_activePanel = nullptr;
    FunctionKeyBar *m_functionKeyBar;
    StatusBarWidget *m_statusBarWidget;
    OperationQueue *m_queue;
    OperationProgressDialog *m_progressDialog;
};
