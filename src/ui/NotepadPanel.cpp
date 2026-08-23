#include "NotepadPanel.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "CloudClipboardController.h"
#include "account/AccountClient.h"
#include "account/DeviceAgent.h"

namespace {
constexpr int kPanelWidth = 460;
constexpr int kEditorPreferredHeight = 150;
constexpr int kEditorMinimumHeight = 100;

class CloudClipboardEditor final : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
    void setController(CloudClipboardController *controller) { m_controller = controller; }

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override {
        return CloudClipboardController::acceptsImage(source) ||
               QPlainTextEdit::canInsertFromMimeData(source);
    }
    void insertFromMimeData(const QMimeData *source) override {
        if (m_controller && m_controller->sendImageFromMimeData(source))
            return;
        QPlainTextEdit::insertFromMimeData(source);
    }

private:
    CloudClipboardController *m_controller = nullptr;
};

QString stateText(CloudClipboardController::State state, const QString &error) {
    switch (state) {
    case CloudClipboardController::State::SignedOut: return QObject::tr("Sign in to use Cloud Clipboard.");
    case CloudClipboardController::State::Loading: return QString();
    case CloudClipboardController::State::Empty: return QObject::tr("Your Cloud Clipboard is empty.");
    case CloudClipboardController::State::Error: return error;
    case CloudClipboardController::State::Ready: return QString();
    }
    return {};
}
} // namespace

NotepadPanel::NotepadPanel(QWidget *parent)
    : QWidget(parent, Qt::Popup), m_ownedSettings(std::make_unique<Settings>()),
      m_settings(*m_ownedSettings) {
    initialize(nullptr, nullptr);
}

NotepadPanel::NotepadPanel(Settings &settings, QWidget *parent)
    : QWidget(parent, Qt::Popup), m_settings(settings) {
    initialize(nullptr, nullptr);
}

NotepadPanel::NotepadPanel(Settings &settings, AccountClient *client, DeviceAgent *agent, QWidget *parent)
    : QWidget(parent, Qt::Popup), m_settings(settings) {
    initialize(client, agent);
}

NotepadPanel::NotepadPanel(Settings &settings, CloudClipboardController *controller,
                           QWidget *parent)
    : QWidget(parent, Qt::Popup), m_settings(settings) {
    initialize(nullptr, nullptr, controller);
}

void NotepadPanel::initialize(AccountClient *client, DeviceAgent *agent,
                              CloudClipboardController *existingController) {
    setObjectName(QStringLiteral("CloudClipboardPanel"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground, true);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("CloudClipboardSearch"));
    m_search->setPlaceholderText(tr("Search Cloud Clipboard..."));
    m_search->setClearButtonEnabled(true);
    m_copy = new QPushButton(tr("Copy"), this);
    m_copy->setObjectName(QStringLiteral("CloudClipboardCopyButton"));
    m_delete = new QPushButton(tr("Delete"), this);
    m_delete->setObjectName(QStringLiteral("CloudClipboardDeleteButton"));
    auto *clear = new QPushButton(tr("Clear"), this);
    clear->setObjectName(QStringLiteral("CloudClipboardClearButton"));
    m_download = new QPushButton(tr("Download Original"), this);
    m_download->setObjectName(QStringLiteral("CloudClipboardDownloadButton"));
    for (QPushButton *button : {m_copy, m_delete, clear, m_download})
        button->setFocusPolicy(Qt::NoFocus);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(4);
    tools->addWidget(m_search, 1);
    tools->addWidget(m_copy);
    tools->addWidget(m_delete);
    tools->addWidget(clear);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("CloudClipboardList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("CloudClipboardStatus"));
    m_status->setWordWrap(true);
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("CloudClipboardImageProgress"));
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);

    m_editor = new CloudClipboardEditor;
    m_editor->setObjectName(QStringLiteral("CloudClipboardEditor"));
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setMinimumHeight(kEditorMinimumHeight);
    m_editor->setPlaceholderText(tr("Type text to send to your devices..."));
    m_imagePreview = new QLabel;
    m_imagePreview->setObjectName(QStringLiteral("CloudClipboardImagePreview"));
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imagePreview->setMinimumHeight(kEditorMinimumHeight);
    m_imagePreview->setText(tr("Paste or select an image to preview it here."));
    m_contentStack = new QStackedWidget;
    m_contentStack->setObjectName(QStringLiteral("CloudClipboardContentStack"));
    m_contentStack->addWidget(m_editor);
    m_contentStack->addWidget(m_imagePreview);
    auto *send = new QPushButton(tr("Send"));
    send->setObjectName(QStringLiteral("CloudClipboardSendButton"));
    auto *upload = new QCheckBox(tr("Auto-upload clipboard"));
    upload->setObjectName(QStringLiteral("CloudClipboardAutoUpload"));
    auto *receive = new QCheckBox(tr("Auto-receive text"));
    receive->setObjectName(QStringLiteral("CloudClipboardAutoReceive"));
    m_autoUpload = upload;
    m_autoReceive = receive;

    auto *editorTools = new QHBoxLayout;
    editorTools->setContentsMargins(0, 0, 0, 0);
    editorTools->addWidget(upload);
    editorTools->addWidget(receive);
    editorTools->addStretch();
    editorTools->addWidget(m_download);
    editorTools->addWidget(send);
    auto *editorPane = new QWidget;
    auto *editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(4);
    editorLayout->addWidget(m_contentStack, 1);
    editorLayout->addLayout(editorTools);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setObjectName(QStringLiteral("CloudClipboardSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_list);
    m_splitter->addWidget(editorPane);
    m_splitter->setStretchFactor(1, 1);
    m_editorHeight = m_settings.cloudClipboardEditorHeight();
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        const QList<int> sizes = m_splitter->sizes();
        if (sizes.size() == 2 && sizes.at(1) > 0) {
            m_editorHeight = sizes.at(1);
            m_settings.setCloudClipboardEditorHeight(m_editorHeight);
            applyDynamicSize();
        }
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->addLayout(tools);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_splitter, 1);

    m_controller = existingController ? existingController
                                      : new CloudClipboardController(m_settings, client, agent, this);
    static_cast<CloudClipboardEditor *>(m_editor)->setController(m_controller);
    upload->setChecked(m_controller->autoUpload());
    receive->setChecked(m_controller->autoReceive());
    receive->setEnabled(upload->isChecked());
    connect(m_controller, &CloudClipboardController::changed, this, &NotepadPanel::rebuild);
    connect(m_controller, &CloudClipboardController::localImagePreview, this,
            [this](const QImage &image) {
                const QPixmap pixmap = QPixmap::fromImage(image).scaled(
                    420, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                m_imagePreview->setPixmap(pixmap);
                m_contentStack->setCurrentWidget(m_imagePreview);
            });
    connect(m_controller, &CloudClipboardController::localImagePublished, this, [this] {
        m_imagePreview->setPixmap(QPixmap());
        m_imagePreview->setText(tr("Paste or select an image to preview it here."));
        m_contentStack->setCurrentWidget(m_editor);
        m_editor->clear();
        if (isVisible())
            m_editor->setFocus();
    });
    connect(m_controller, &CloudClipboardController::privacyWarningRequired, this, [this] {
        const auto choice = QMessageBox::warning(this, tr("Cloud Clipboard privacy"),
            tr("Automatic upload sends copied text and images to your signed-in devices. "
               "Private, file and URL clipboard data is excluded. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        m_controller->confirmAutoUpload(choice == QMessageBox::Yes);
        m_autoUpload->setChecked(m_controller->autoUpload());
        m_autoReceive->setEnabled(m_autoUpload->isChecked());
    });
    connect(m_controller, &CloudClipboardController::imageDownloadProgress, this,
            [this](const QString &, qint64 received, qint64 total) {
                const int percent = total > 0 ? int(received * 100 / total) : 0;
                m_progress->setValue(percent);
                m_progress->setVisible(true);
                m_status->setText(tr("Synchronizing original image: %1% (%2 / %3 bytes)")
                                      .arg(percent).arg(received).arg(total));
                m_status->setVisible(true);
            });
    connect(m_controller, &CloudClipboardController::imageDownloadFailed, this,
            [this](const QString &, const QString &error) {
                m_progress->setVisible(false);
                m_status->setText(error);
                m_status->setVisible(true);
            });
    connect(m_controller, &CloudClipboardController::imageCopied, this,
            [this](const QString &) {
                m_progress->setVisible(false);
                m_status->setText(tr("Original image synchronized and copied. You can paste it now."));
                m_status->setVisible(true);
            });
    connect(m_search, &QLineEdit::textChanged, this, &NotepadPanel::rebuild);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this] { updateSelection(); });
    connect(m_copy, &QPushButton::clicked, this, &NotepadPanel::copySelected);
    connect(m_delete, &QPushButton::clicked, this, &NotepadPanel::deleteSelected);
    connect(clear, &QPushButton::clicked, m_controller, &CloudClipboardController::clear);
    connect(m_download, &QPushButton::clicked, this, &NotepadPanel::downloadSelected);
    connect(send, &QPushButton::clicked, this, &NotepadPanel::send);
    connect(upload, &QCheckBox::toggled, this, &NotepadPanel::onAutoUploadToggled);
    connect(receive, &QCheckBox::toggled, this, &NotepadPanel::onAutoReceiveToggled);
    rebuild();
}

QString NotepadPanel::itemLabel(const CloudClipboardItem &item) const {
    const QString source = m_controller ? m_controller->deviceName(item.sourceDeviceId)
                                        : tr("Other device");
    const QDateTime when = QDateTime::fromString(item.created, Qt::ISODate).toLocalTime();
    const QString time = when.isValid() ? when.toString(Qt::DefaultLocaleShortDate) : QString();
    if (item.type == QLatin1String("image"))
        return QObject::tr("Image · %1 · %2").arg(source, time);
    return QStringLiteral("%1\n%2 · %3").arg(item.text.left(100), source, time);
}

void NotepadPanel::rebuild() {
    const bool loading = m_controller->state() == CloudClipboardController::State::Loading;
    m_search->setEnabled(!loading);
    m_search->setPlaceholderText(loading ? tr("Loading Cloud Clipboard...")
                                         : tr("Search Cloud Clipboard..."));
    const QString query = m_search->text().trimmed();
    const QString status = stateText(m_controller->state(), m_controller->error());
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    const QString selectedId = m_list->currentItem() ?
        m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    m_list->clear();
    for (const CloudClipboardItem &item : m_controller->items()) {
        if (!query.isEmpty() && !itemLabel(item).contains(query, Qt::CaseInsensitive))
            continue;
        auto *row = new QListWidgetItem(itemLabel(item), m_list);
        row->setData(Qt::UserRole, item.id);
        if (item.type == QLatin1String("image")) {
            if (!item.thumbnail.isEmpty()) {
                QPixmap pixmap;
                pixmap.loadFromData(item.thumbnail);
                row->setIcon(QIcon(pixmap));
            } else {
                m_controller->requestThumbnail(item.id);
            }
        }
        if (item.id == selectedId)
            m_list->setCurrentItem(row);
    }
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateSelection();
    applyDynamicSize();
}

const CloudClipboardItem *NotepadPanel::selected() const {
    const auto *row = m_list->currentItem();
    if (!row)
        return nullptr;
    const QString id = row->data(Qt::UserRole).toString();
    for (const CloudClipboardItem &item : m_controller->items()) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

void NotepadPanel::updateSelection() {
    const CloudClipboardItem *item = selected();
    const bool has = item != nullptr;
    m_copy->setEnabled(has);
    m_delete->setEnabled(has);
    m_download->setEnabled(has && item->type == QLatin1String("image"));
    if (!item || item->type != QLatin1String("image")) {
        m_contentStack->setCurrentWidget(m_editor);
        return;
    }
    if (item->thumbnail.isEmpty()) {
        m_imagePreview->setPixmap(QPixmap());
        m_imagePreview->setText(tr("Loading image preview..."));
        m_controller->requestThumbnail(item->id);
    } else {
        QPixmap pixmap;
        pixmap.loadFromData(item->thumbnail);
        m_imagePreview->setPixmap(pixmap.scaled(420, 220, Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
        m_imagePreview->setText(QString());
    }
    m_contentStack->setCurrentWidget(m_imagePreview);
}

void NotepadPanel::copySelected() {
    const CloudClipboardItem *item = selected();
    if (!item)
        return;
    if (item->type == QLatin1String("text"))
        QGuiApplication::clipboard()->setText(item->text);
    else if (item->type == QLatin1String("image"))
        m_controller->requestOriginal(*item);
}

void NotepadPanel::deleteSelected() {
    if (const CloudClipboardItem *item = selected())
        m_controller->deleteItem(item->id);
}

void NotepadPanel::downloadSelected() {
    if (const CloudClipboardItem *item = selected())
        m_controller->requestOriginal(*item);
}

void NotepadPanel::send() {
    if (m_contentStack->currentWidget() == m_imagePreview && m_controller->hasStagedImage()) {
        if (m_controller->sendStagedImage()) {
            m_imagePreview->setPixmap(QPixmap());
            m_imagePreview->setText(tr("Paste or select an image to preview it here."));
            m_contentStack->setCurrentWidget(m_editor);
            m_editor->clear();
            m_editor->setFocus();
        }
        return;
    }
    const QString text = m_editor->toPlainText();
    if (text.isEmpty())
        m_controller->sendCurrentClipboard();
    else {
        m_controller->sendText(text);
        m_editor->clear();
    }
}

void NotepadPanel::onAutoUploadToggled(bool enabled) {
    m_controller->setAutoUpload(enabled);
    m_autoReceive->setEnabled(m_controller->autoUpload());
    if (!m_controller->autoUpload())
        m_autoReceive->setChecked(false);
}

void NotepadPanel::onAutoReceiveToggled(bool enabled) {
    m_controller->setAutoReceive(enabled);
    if (enabled != m_controller->autoReceive())
        m_autoReceive->setChecked(m_controller->autoReceive());
}

void NotepadPanel::popUpAbove(const QRect &anchorGlobalRect, const QRect &appContentGlobalRect) {
    m_anchorRect = anchorGlobalRect;
    m_appContentRect = appContentGlobalRect;
    m_anchorSize = anchorGlobalRect.size();
    m_anchorRightInset = appContentGlobalRect.right() - anchorGlobalRect.right();
    m_anchorBottomInset = appContentGlobalRect.bottom() - anchorGlobalRect.bottom();
    if (parentWidget())
        parentWidget()->installEventFilter(this);
    m_controller->refresh();
    show();
    applyDynamicSize();
    raise();
    m_editor->setFocus();
}

void NotepadPanel::closeEvent(QCloseEvent *event) {
    if (parentWidget())
        parentWidget()->removeEventFilter(this);
    QWidget::closeEvent(event);
}

bool NotepadPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
         event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show)) {
        const QRect local = parentWidget()->contentsRect();
        m_appContentRect = QRect(parentWidget()->mapToGlobal(local.topLeft()), local.size());
        const int right = m_appContentRect.right() - m_anchorRightInset;
        const int bottom = m_appContentRect.bottom() - m_anchorBottomInset;
        m_anchorRect = QRect(QPoint(right - m_anchorSize.width() + 1,
                                   bottom - m_anchorSize.height() + 1), m_anchorSize);
        QTimer::singleShot(0, this, [this] { applyDynamicSize(); });
    }
    return QWidget::eventFilter(watched, event);
}

void NotepadPanel::applyDynamicSize() {
    if (m_anchorRect.isNull() || m_appContentRect.isNull())
        return;
    const int popupHeight = qMax(0, m_anchorRect.top() - m_appContentRect.top());
    const int toolbar = m_search->sizeHint().height();
    const int status = m_status->isVisible() ? m_status->sizeHint().height() + 6 : 0;
    const int progress = m_progress->isVisible() ? m_progress->sizeHint().height() + 6 : 0;
    const int editor = qMax(kEditorMinimumHeight,
                            m_editorHeight > 0 ? m_editorHeight : kEditorPreferredHeight);
    const int chrome = contentsMargins().top() + contentsMargins().bottom() + 18;
    const int splitHeight = qMax(0, popupHeight - toolbar - status - progress - chrome);
    const int listHeight = qMax(28, splitHeight - editor);
    m_splitter->setSizes({listHeight, qMax(kEditorMinimumHeight, splitHeight - listHeight)});

    int x = m_appContentRect.right() - kPanelWidth + 1;
    if (QScreen *screen = QGuiApplication::screenAt(m_anchorRect.center())) {
        const QRect available = screen->availableGeometry();
        x = qBound(available.left(), x, available.right() - kPanelWidth + 1);
    }
    setGeometry(x, m_appContentRect.top(), kPanelWidth, popupHeight);
}
