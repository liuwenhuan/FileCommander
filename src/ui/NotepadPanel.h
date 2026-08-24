#pragma once

#include <QHash>
#include <QPair>
#include <QRect>
#include <QStringList>
#include <QWidget>

#include <memory>

#include "config/Settings.h"

class AccountClient;
class CloudClipboardController;
class DeviceAgent;
class QCloseEvent;
class QCheckBox;
class QComboBox;
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
    void clearTransferProgress();
    void updateTransferProgress();
    void rebuildTargetDevices();
    const ClipboardHistoryRecord *selected() const;
    QStringList visibleRecordIds() const;
    QStringList selectedRecordIds() const;
    QString currentRecordId() const;
    QString itemLabel(const ClipboardHistoryRecord &item) const;

    std::unique_ptr<Settings> m_ownedSettings;
    Settings &m_settings;
    CloudClipboardController *m_controller = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QSplitter *m_splitter = nullptr;
    QPlainTextEdit *m_textPreview = nullptr;
    QLabel *m_imagePreview = nullptr;
    QStackedWidget *m_contentStack = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_delete = nullptr;
    QCheckBox *m_autoSend = nullptr;
    QComboBox *m_targetDevice = nullptr;
    QPushButton *m_send = nullptr;
    QString m_transferStatus;
    QHash<QString, QPair<qint64, qint64>> m_transferProgress;
    QRect m_anchorRect;
    QRect m_appContentRect;
    QSize m_anchorSize;
    int m_anchorRightInset = 0;
    int m_anchorBottomInset = 0;
};
