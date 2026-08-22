#pragma once

#include <QRect>
#include <QWidget>

#include <memory>

#include "config/Settings.h"

class AccountClient;
class CloudClipboardController;
class DeviceAgent;
class QCloseEvent;
struct CloudClipboardItem;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSplitter;

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

private slots:
    void rebuild();
    void copySelected();
    void deleteSelected();
    void downloadSelected();
    void send();
    void onAutoUploadToggled(bool enabled);
    void onAutoReceiveToggled(bool enabled);

private:
    void initialize(AccountClient *client, DeviceAgent *agent,
                    CloudClipboardController *existingController = nullptr);
    void applyDynamicSize();
    const CloudClipboardItem *selected() const;
    static QString itemLabel(const CloudClipboardItem &item);

    std::unique_ptr<Settings> m_ownedSettings;
    Settings &m_settings;
    CloudClipboardController *m_controller = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QSplitter *m_splitter = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_autoUpload = nullptr;
    QCheckBox *m_autoReceive = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_download = nullptr;
    QRect m_anchorRect;
    QRect m_appContentRect;
    int m_editorHeight = 0;
};
