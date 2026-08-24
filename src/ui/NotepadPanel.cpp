#include "NotepadPanel.h"

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QStyleOptionViewItem>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "CloudClipboardController.h"
#include "CloudClipboardRowDelegate.h"
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
    for (QPushButton *button : {m_delete, clear})
        button->setFocusPolicy(Qt::NoFocus);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(4);
    tools->addWidget(m_search, 1);
    tools->addWidget(m_delete);
    tools->addWidget(clear);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("CloudClipboardList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setUniformItemSizes(true);
    m_list->setItemDelegate(new CloudClipboardRowDelegate(m_list));
    m_list->installEventFilter(this);
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
    m_autoSend = new QCheckBox(tr("Auto Send"), this);
    m_autoSend->setObjectName(QStringLiteral("CloudClipboardAutoSend"));
    m_targetDevice = new QComboBox(this);
    m_targetDevice->setObjectName(QStringLiteral("CloudClipboardTargetDeviceCombo"));
    m_targetDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targetDevice->setMinimumContentsLength(10);
    m_targetDevice->setMinimumWidth(120);
    m_targetDevice->setMaximumWidth(180);
    m_send = new QPushButton(tr("Send"), this);
    m_send->setObjectName(QStringLiteral("CloudClipboardSendButton"));
    m_send->setFocusPolicy(Qt::StrongFocus);

    auto *previewTools = new QHBoxLayout;
    previewTools->setContentsMargins(0, 0, 0, 0);
    previewTools->addWidget(m_copy);
    previewTools->addStretch();
    previewTools->addWidget(m_autoSend);
    previewTools->addWidget(m_targetDevice);
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
    m_autoSend->setChecked(m_controller->autoSendEnabled());
    rebuildTargetDevices();
    connect(m_controller, &CloudClipboardController::changed, this, &NotepadPanel::rebuild);
    connect(m_controller, &CloudClipboardController::devicesChanged, this,
            [this] {
                rebuildTargetDevices();
                updateSelection();
            });
    connect(m_controller, &CloudClipboardController::autoSendChanged, this,
            [this](bool enabled) {
                QSignalBlocker block(m_autoSend);
                m_autoSend->setChecked(enabled);
                updateSelection();
            });
    connect(m_controller, &CloudClipboardController::selectedTargetDeviceChanged, this,
            [this](const QString &) {
                rebuildTargetDevices();
                updateSelection();
            });
    connect(m_controller, &CloudClipboardController::transferStatusChanged, this,
            [this](const QString &status) {
                m_transferStatus = status;
                m_status->setText(status);
                m_status->setVisible(!status.isEmpty());
            });
    connect(m_controller, &CloudClipboardController::transferReset, this,
            &NotepadPanel::clearTransferProgress);
    connect(m_controller, &CloudClipboardController::transferFinished, this,
            [this](const QString &recordId) {
                m_transferProgress.remove(recordId);
                updateTransferProgress();
            });
    connect(m_controller, &CloudClipboardController::transferProgress, this,
            [this](const QString &recordId, qint64 completed, qint64 total) {
                if (recordId.isEmpty())
                    return;
                m_transferProgress.insert(recordId, qMakePair(completed, total));
                updateTransferProgress();
            });
    connect(m_search, &QLineEdit::textChanged, this, &NotepadPanel::rebuild);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current) {
                m_controller->selectRecord(current ? current->data(CloudClipboardRowDelegate::RecordIdRole).toString()
                                                   : QString());
                updateSelection();
            });
    connect(m_list, &QListWidget::itemSelectionChanged, this, &NotepadPanel::updateSelection);
    connect(m_controller, &CloudClipboardController::selectionChanged, this,
            [this](const QString &) { updateSelection(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { copySelected(); });
    connect(m_copy, &QPushButton::clicked, this, &NotepadPanel::copySelected);
    connect(m_delete, &QPushButton::clicked, this, &NotepadPanel::deleteSelected);
    connect(clear, &QPushButton::clicked, m_controller, &CloudClipboardController::clear);
    connect(m_autoSend, &QCheckBox::toggled, m_controller,
            &CloudClipboardController::setAutoSendEnabled);
    connect(m_targetDevice, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index >= 0)
                    m_controller->setSelectedTargetDeviceId(m_targetDevice->itemData(index).toString());
            });
    connect(m_send, &QPushButton::clicked, this, &NotepadPanel::send);
    QWidget::setTabOrder(m_textPreview, m_copy);
    QWidget::setTabOrder(m_copy, m_autoSend);
    QWidget::setTabOrder(m_autoSend, m_targetDevice);
    QWidget::setTabOrder(m_targetDevice, m_send);
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

void NotepadPanel::rebuildTargetDevices() {
    const QString targetId = m_controller->selectedTargetDeviceId();
    QSignalBlocker block(m_targetDevice);
    m_targetDevice->clear();
    m_targetDevice->addItem(tr("All devices"), QString());
    m_targetDevice->setItemData(0, tr("All devices"), Qt::ToolTipRole);

    bool foundTarget = targetId.isEmpty();
    for (const AccountDeviceInfo &device : m_controller->devices()) {
        if (device.self || device.id.isEmpty())
            continue;
        const QString name = device.name.isEmpty() ? device.id : device.name;
        const QString state = device.online ? tr("Online") : tr("Offline");
        const QString label = tr("%1 — %2").arg(name, state);
        m_targetDevice->addItem(label, device.id);
        m_targetDevice->setItemData(m_targetDevice->count() - 1, label, Qt::ToolTipRole);
        foundTarget = foundTarget || device.id == targetId;
    }
    if (!foundTarget) {
        const QString label = tr("Unavailable device");
        m_targetDevice->addItem(label, targetId);
        m_targetDevice->setItemData(m_targetDevice->count() - 1, label, Qt::ToolTipRole);
    }

    const int index = m_targetDevice->findData(targetId);
    m_targetDevice->setCurrentIndex(index >= 0 ? index : 0);
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
    if (!m_transferProgress.isEmpty())
        updateTransferProgress();

    const QStringList selectedIds = selectedRecordIds();
    const QString oldCurrentId = currentRecordId();
    const QString controllerCurrentId = m_controller->selectedRecordId();
    const bool restoreFocus = m_list->hasFocus();
    const int scrollValue = m_list->verticalScrollBar()->value();
    QListWidgetItem *currentRow = nullptr;
    {
        QSignalBlocker block(m_list);
        m_list->clear();
        for (const ClipboardHistoryRecord &item : m_controller->items()) {
            if (!query.isEmpty() && !itemLabel(item).contains(query, Qt::CaseInsensitive))
                continue;

            const QString source = item.sourceDeviceName.isEmpty()
                                       ? m_controller->deviceName(item.sourceDeviceId)
                                       : item.sourceDeviceName;
            const QDateTime when = item.created.toLocalTime();
            const QString time = when.isValid() ? QLocale().toString(when, QLocale::ShortFormat)
                                                : QString();
            auto *row = new QListWidgetItem(m_list);
            row->setData(CloudClipboardRowDelegate::RecordIdRole, item.id);
            row->setData(CloudClipboardRowDelegate::KindRole,
                         item.kind == ClipboardRecordKind::Image ? QStringLiteral("image")
                                                                  : QStringLiteral("text"));
            row->setData(CloudClipboardRowDelegate::ContentRole, item.text);
            row->setData(CloudClipboardRowDelegate::ImagePathRole, item.imagePath);
            row->setData(CloudClipboardRowDelegate::TimeRole, time);
            row->setData(CloudClipboardRowDelegate::DeviceRole, source);
            row->setData(Qt::AccessibleTextRole,
                         item.kind == ClipboardRecordKind::Image
                             ? tr("Image, %1, %2").arg(time, source)
                             : tr("%1, %2, %3").arg(item.text, time, source));
            row->setSizeHint(m_list->itemDelegate()->sizeHint(
                QStyleOptionViewItem(), m_list->model()->index(m_list->row(row), 0)));
            if (selectedIds.contains(item.id))
                row->setSelected(true);
            if (item.id == controllerCurrentId ||
                (controllerCurrentId.isEmpty() && item.id == oldCurrentId)) {
                currentRow = row;
            }
        }
        if (currentRow)
            m_list->selectionModel()->setCurrentIndex(
                m_list->model()->index(m_list->row(currentRow), 0), QItemSelectionModel::NoUpdate);
    }
    m_list->verticalScrollBar()->setValue(scrollValue);
    if (currentRow && m_list->selectedItems().isEmpty())
        currentRow->setSelected(true);
    if (currentRow && oldCurrentId != controllerCurrentId)
        m_list->scrollToItem(currentRow, QAbstractItemView::EnsureVisible);
    if (restoreFocus)
        m_list->setFocus();
    updateSelection();
    applyDynamicSize();
}

void NotepadPanel::clearTransferProgress() {
    m_transferProgress.clear();
    m_progress->setValue(0);
    m_progress->setVisible(false);
}

void NotepadPanel::updateTransferProgress() {
    if (m_transferProgress.isEmpty()) {
        m_progress->setValue(0);
        m_progress->setVisible(false);
        return;
    }

    qint64 completed = 0;
    qint64 total = 0;
    for (auto it = m_transferProgress.cbegin(); it != m_transferProgress.cend(); ++it) {
        completed += qMax<qint64>(0, it.value().first);
        total += qMax<qint64>(0, it.value().second);
    }
    const int percent = total > 0 ? qBound(0, int(completed * 100 / total), 100) : 0;
    m_progress->setRange(0, 100);
    m_progress->setValue(percent);
    m_progress->setFormat(QStringLiteral("%1% (%2 / %3 bytes)")
                              .arg(percent).arg(completed).arg(total));
    m_progress->setVisible(true);
    m_status->setText(tr("Transfer progress: %1% (%2 / %3 bytes)")
                          .arg(percent).arg(completed).arg(total));
    m_status->setVisible(true);
}

const ClipboardHistoryRecord *NotepadPanel::selected() const {
    const QString id = currentRecordId();
    if (id.isEmpty())
        return nullptr;
    for (const ClipboardHistoryRecord &item : m_controller->items()) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

QStringList NotepadPanel::visibleRecordIds() const {
    QStringList ids;
    for (int row = 0; row < m_list->count(); ++row)
        ids.append(m_list->item(row)->data(CloudClipboardRowDelegate::RecordIdRole).toString());
    return ids;
}

QStringList NotepadPanel::selectedRecordIds() const {
    QStringList ids;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        if (item->isSelected())
            ids.append(item->data(CloudClipboardRowDelegate::RecordIdRole).toString());
    }
    return ids;
}

QString NotepadPanel::currentRecordId() const {
    const QListWidgetItem *row = m_list->currentItem();
    return row ? row->data(CloudClipboardRowDelegate::RecordIdRole).toString() : QString();
}

void NotepadPanel::updateSelection() {
    const ClipboardHistoryRecord *item = selected();
    const QStringList selectedIds = selectedRecordIds();
    bool hasLocalSelection = false;
    for (const QString &id : selectedIds) {
        const ClipboardHistoryRecord record = m_controller->record(id);
        if (record.origin == ClipboardRecordOrigin::Local) {
            hasLocalSelection = true;
            break;
        }
    }
    m_copy->setEnabled(item != nullptr);
    m_delete->setEnabled(!selectedIds.isEmpty());
    m_send->setEnabled(hasLocalSelection && m_controller->targetDeviceIsAvailable());
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
    const QStringList ids = selectedRecordIds();
    if (ids.isEmpty())
        return;

    const QStringList visible = visibleRecordIds();
    QSet<QString> deleted;
    for (const QString &id : ids)
        deleted.insert(id);
    int first = visible.size();
    int last = -1;
    for (int index = 0; index < visible.size(); ++index) {
        if (!deleted.contains(visible.at(index)))
            continue;
        first = qMin(first, index);
        last = qMax(last, index);
    }

    QString successor;
    for (int index = last + 1; index < visible.size(); ++index) {
        if (!deleted.contains(visible.at(index))) {
            successor = visible.at(index);
            break;
        }
    }
    for (int index = first - 1; successor.isEmpty() && index >= 0; --index) {
        if (!deleted.contains(visible.at(index))) {
            successor = visible.at(index);
            break;
        }
    }

    if (m_controller->removeRecords(ids, successor))
        m_list->setFocus();
}

void NotepadPanel::send() {
    if (!m_controller->targetDeviceIsAvailable())
        return;
    m_controller->sendRecords(selectedRecordIds(), m_targetDevice->currentData().toString());
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
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers modifiers = key->modifiers();
        if (key->key() == Qt::Key_Delete &&
            (modifiers == Qt::NoModifier || modifiers == Qt::KeypadModifier)) {
            deleteSelected();
            event->accept();
            return true;
        }
    }
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
