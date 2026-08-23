#pragma once

#include <QRect>
#include <QWidget>

#include <memory>

#include "config/Settings.h"

class AccountClient;
class CloudClipboardController;
class DeviceAgent;
class QCloseEvent;
class QEvent;
struct ClipboardHistoryRecord;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QSplitter;
class QStackedWidget;

// Compatibility name for the existing "notepad" command. The popup is now an
// account-backed Cloud Clipboard rather than a local note store.
class NotepadPanel : public QWidget {
    Q_OBJECT

public:
    explicit NotepadPanel(QWidget *parent = nullptr);
    NotepadPanel(Settings &settings, QWidget *parent = nullptr);
    NotepadPanel(Settings &settings, AccountClient *client, DeviceAgent *agent = nullptr,
                 QWidget *parent = nullptr);
    NotepadPanel(Settings &settings, CloudClipboardController *controller,
                 QWidget *parent = nullptr);

    void popUpAbove(const QRect &anchorGlobalRect, const QRect &appContentGlobalRect);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void rebuild();
    void copySelected();
    void deleteSelected();
    void send();
    void updateSelection();

private:
    void initialize(AccountClient *client, DeviceAgent *agent,
                    CloudClipboardController *existingController = nullptr);
    void applyDynamicSize();
    const ClipboardHistoryRecord *selected() const;
    QString itemLabel(const ClipboardHistoryRecord &item) const;

    std::unique_ptr<Settings> m_ownedSettings;
    Settings &m_settings;
    CloudClipboardController *m_controller = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QSplitter *m_splitter = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_imagePreview = nullptr;
    QStackedWidget *m_contentStack = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_delete = nullptr;
    QRect m_anchorRect;
    QRect m_appContentRect;
    QSize m_anchorSize;
    int m_anchorRightInset = 0;
    int m_anchorBottomInset = 0;
    int m_editorHeight = 0;
};
