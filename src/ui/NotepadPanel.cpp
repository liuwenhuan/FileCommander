#include "NotepadPanel.h"

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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "CloudClipboardController.h"
#include "account/AccountClient.h"
#include "account/DeviceAgent.h"

namespace {
constexpr int kPanelWidth = 460;
constexpr int kPreviewPreferredHeight = 150;
constexpr int kPreviewMinimumHeight = 100;

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
    for (QPushButton *button : {m_copy, m_delete, clear})
        button->setFocusPolicy(Qt::NoFocus);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(4);
    tools->addWidget(m_search, 1);
    tools->addWidget(m_copy);
    tools->addWidget(m_delete);
    tools->addWidget(clear);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("CloudClipboardList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("CloudClipboardStatus"));
    m_status->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("CloudClipboardProgress"));
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);

    m_textPreview = new QPlainTextEdit;
    m_textPreview->setObjectName(QStringLiteral("CloudClipboardPreview"));
    m_textPreview->setReadOnly(true);
    m_textPreview->setTabChangesFocus(true);
    m_textPreview->setFrameShape(QFrame::NoFrame);
    m_textPreview->setMinimumHeight(kPreviewMinimumHeight);
    m_imagePreview = new QLabel;
    m_imagePreview->setObjectName(QStringLiteral("CloudClipboardImagePreview"));
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imagePreview->setMinimumHeight(kPreviewMinimumHeight);
    m_contentStack = new QStackedWidget;
    m_contentStack->setObjectName(QStringLiteral("CloudClipboardContentStack"));
    m_contentStack->addWidget(m_textPreview);
    m_contentStack->addWidget(m_imagePreview);
    m_send = new QPushButton(tr("Send to other devices"));
    m_send->setObjectName(QStringLiteral("CloudClipboardSendButton"));
    m_send->setFocusPolicy(Qt::StrongFocus);
    m_send->setVisible(false);

    auto *previewTools = new QHBoxLayout;
    previewTools->setContentsMargins(0, 0, 0, 0);
    previewTools->addStretch();
    previewTools->addWidget(m_send);
    auto *previewPane = new QWidget;
    auto *previewLayout = new QVBoxLayout(previewPane);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(4);
    previewLayout->addWidget(m_contentStack, 1);
    previewLayout->addLayout(previewTools);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setObjectName(QStringLiteral("CloudClipboardSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_list);
    m_splitter->addWidget(previewPane);
    m_splitter->setStretchFactor(1, 1);

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
    connect(m_controller, &CloudClipboardController::changed, this, &NotepadPanel::rebuild);
    connect(m_controller, &CloudClipboardController::transferStatusChanged, this,
            [this](const QString &status) {
                m_transferStatus = status;
                m_status->setText(status);
                m_status->setVisible(!status.isEmpty());
            });
    connect(m_controller, &CloudClipboardController::transferFinished, this,
            [this](const QString &recordId) {
                if (recordId == m_activeTransferId)
                    clearTransferProgress();
            });
    connect(m_controller, &CloudClipboardController::transferProgress, this,
            [this](const QString &recordId, qint64 completed, qint64 total) {
                if (recordId.isEmpty())
                    return;
                if (m_activeTransferId.isEmpty())
                    m_activeTransferId = recordId;
                if (m_activeTransferId != recordId)
                    return;
                const int percent = total > 0 ? int(completed * 100 / total) : 0;
                m_progress->setRange(0, 100);
                m_progress->setValue(percent);
                m_progress->setFormat(QStringLiteral("%1% (%2 / %3 bytes)")
                                          .arg(percent).arg(completed).arg(total));
                m_progress->setVisible(true);
                m_status->setText(tr("Transfer progress: %1% (%2 / %3 bytes)")
                                      .arg(percent).arg(completed).arg(total));
                m_status->setVisible(true);
            });
    connect(m_search, &QLineEdit::textChanged, this, &NotepadPanel::rebuild);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current) {
                m_controller->selectRecord(current ? current->data(Qt::UserRole).toString() : QString());
                updateSelection();
            });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { copySelected(); });
    connect(m_copy, &QPushButton::clicked, this, &NotepadPanel::copySelected);
    connect(m_delete, &QPushButton::clicked, this, &NotepadPanel::deleteSelected);
    connect(clear, &QPushButton::clicked, m_controller, &CloudClipboardController::clear);
    connect(m_send, &QPushButton::clicked, this, &NotepadPanel::send);
    rebuild();
}

QString NotepadPanel::itemLabel(const ClipboardHistoryRecord &item) const {
    const QString source = item.sourceDeviceName.isEmpty()
                               ? (m_controller ? m_controller->deviceName(item.sourceDeviceId)
                                               : tr("This device"))
                               : item.sourceDeviceName;
    const QDateTime when = item.created.toLocalTime();
    const QString time = when.isValid() ? when.toString(Qt::DefaultLocaleShortDate) : QString();
    if (item.kind == ClipboardRecordKind::Image)
        return QObject::tr("Image · %1 · %2").arg(source, time);
    return QStringLiteral("%1\n%2 · %3").arg(item.text.left(100), source, time);
}

void NotepadPanel::rebuild() {
    const bool loading = m_controller->state() == CloudClipboardController::State::Loading;
    m_search->setEnabled(!loading);
    m_search->setPlaceholderText(loading ? tr("Loading Cloud Clipboard...")
                                         : tr("Search Cloud Clipboard..."));
    const QString query = m_search->text().trimmed();
    const QString status = m_transferStatus.isEmpty()
                               ? stateText(m_controller->state(), m_controller->error())
                               : m_transferStatus;
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    const QString selectedId = m_list->currentItem() ?
        m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    {
        QSignalBlocker block(m_list);
        m_list->clear();
        for (const ClipboardHistoryRecord &item : m_controller->items()) {
            if (!query.isEmpty() && !itemLabel(item).contains(query, Qt::CaseInsensitive))
                continue;
            auto *row = new QListWidgetItem(itemLabel(item), m_list);
            row->setData(Qt::UserRole, item.id);
            if (item.kind == ClipboardRecordKind::Image && !item.imagePath.isEmpty())
                row->setIcon(QIcon(item.imagePath));
            if (item.id == selectedId)
                m_list->setCurrentItem(row);
        }
    }
    m_controller->selectRecord(m_list->currentItem()
                                   ? m_list->currentItem()->data(Qt::UserRole).toString()
                                   : QString());
    updateSelection();
    applyDynamicSize();
}

void NotepadPanel::clearTransferProgress() {
    m_activeTransferId.clear();
    m_progress->setValue(0);
    m_progress->setVisible(false);
}

const ClipboardHistoryRecord *NotepadPanel::selected() const {
    const auto *row = m_list->currentItem();
    if (!row)
        return nullptr;
    const QString id = row->data(Qt::UserRole).toString();
    for (const ClipboardHistoryRecord &item : m_controller->items()) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

void NotepadPanel::updateSelection() {
    const ClipboardHistoryRecord *item = selected();
    const bool has = item != nullptr;
    const bool local = has && item->origin == ClipboardRecordOrigin::Local;
    m_copy->setEnabled(has);
    m_delete->setEnabled(has);
    m_send->setVisible(local);
    m_send->setEnabled(local);
    if (!item) {
        m_textPreview->clear();
        m_contentStack->setCurrentWidget(m_textPreview);
        return;
    }
    if (item->kind != ClipboardRecordKind::Image) {
        m_textPreview->setPlainText(item->text);
        m_contentStack->setCurrentWidget(m_textPreview);
        return;
    }

    const QImage image(item->imagePath);
    if (image.isNull()) {
        m_imagePreview->setPixmap(QPixmap());
        m_imagePreview->setText(tr("Could not load image preview."));
    } else {
        const QImage scaled = image.scaled(420, 220, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
        m_imagePreview->setPixmap(QPixmap::fromImage(scaled));
        m_imagePreview->setText(QString());
    }
    m_contentStack->setCurrentWidget(m_imagePreview);
}

void NotepadPanel::copySelected() {
    if (const ClipboardHistoryRecord *item = selected())
        m_controller->copyRecordToClipboard(item->id);
}

void NotepadPanel::deleteSelected() {
    if (const ClipboardHistoryRecord *item = selected())
        m_controller->removeRecord(item->id);
}

void NotepadPanel::send() {
    if (const ClipboardHistoryRecord *item = selected();
        item && item->origin == ClipboardRecordOrigin::Local) {
        m_controller->sendRecord(item->id);
    }
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
    m_search->setFocus();
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
    const int chrome = contentsMargins().top() + contentsMargins().bottom() + 18;
    const int splitHeight = qMax(0, popupHeight - toolbar - status - progress - chrome);
    const int preview = qMax(kPreviewMinimumHeight, kPreviewPreferredHeight);
    const int listHeight = qMax(28, splitHeight - preview);
    m_splitter->setSizes({listHeight, qMax(kPreviewMinimumHeight, splitHeight - listHeight)});

    int x = m_appContentRect.right() - kPanelWidth + 1;
    if (QScreen *screen = QGuiApplication::screenAt(m_anchorRect.center())) {
        const QRect available = screen->availableGeometry();
        x = qBound(available.left(), x, available.right() - kPanelWidth + 1);
    }
    setGeometry(x, m_appContentRect.top(), kPanelWidth, popupHeight);
}
