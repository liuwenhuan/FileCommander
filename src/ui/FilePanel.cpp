#include "FilePanel.h"

#include "DirectorySizeTask.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QStandardPaths>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>

#include "ThemedDialogs.h"
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QSet>
#include <QStackedWidget>
#include <QListView>
#include <QShortcut>
#include <QStorageInfo>
#include <QTimer>
#include <QTreeView>
#include <QSplitter>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "BreadcrumbBar.h"
#include "FileListView.h"
#include "IconFileView.h"
#include "FileProvider.h"
#include "filesystem/ComputerProvider.h"
#include "LocalFileProvider.h"
#include "filesystem/IconCache.h"
#include "ArchiveLayout.h"
#include "ArchiveProvider.h"
#include "StatusBarWidget.h"
#include "TabBar.h"
#include "network/NetworkSession.h"
#include "devices/RemovableDeviceMonitor.h"
#include "tree/DirectoryTreeModel.h"
#include "tree/NetworkTreeRegistry.h"
#include "tree/TreeDirLister.h"
#include "tree/TreeRootBuilder.h"
#include "ThumbnailCache.h"
#include "ThumbnailDelegate.h"
#include "MotionPolicy.h"

#include <algorithm>

namespace {

constexpr int kTreeAnimationVisibleRowLimit = 500;
constexpr int kTreeInputCadenceIntervalMs = 100;
constexpr int kDirectorySizeBusyDelayMs = 500;
constexpr auto kTreeFeedbackAnimationName = "DirectoryTreeDisclosureFeedbackAnimation";
constexpr int kTreeGlyphMaxWidth = 14;
constexpr qreal kTreeGlyphLineWidth = 1.2;

class TreeToggleButton final : public QToolButton {
public:
    explicit TreeToggleButton(QWidget *parent = nullptr) : QToolButton(parent) {
        setProperty("compactTreeGlyph", true);
        setProperty("treeGlyphMaxWidth", kTreeGlyphMaxWidth);
        setText(QString());
        setToolButtonStyle(Qt::ToolButtonIconOnly);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QToolButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        QColor fg = palette().color(group, QPalette::ButtonText);
        if (!fg.isValid())
            fg = palette().color(group, QPalette::WindowText);
        painter.setPen(QPen(fg, kTreeGlyphLineWidth, Qt::SolidLine, Qt::SquareCap));

        const qreal lineWidth = qMin<qreal>(kTreeGlyphMaxWidth, qMax<qreal>(8.0, width() * 0.46));
        const qreal left = (width() - lineWidth) / 2.0;
        const qreal right = left + lineWidth;
        const qreal centerY = height() / 2.0;
        for (qreal y : {centerY - 5.0, centerY, centerY + 5.0})
            painter.drawLine(QPointF(left, y), QPointF(right, y));
    }
};

} // namespace

class DirectoryTreeView final : public QTreeView {
public:
    explicit DirectoryTreeView(QWidget *parent = nullptr) : QTreeView(parent) {
        // The tree's built-in animation moves row geometry. Disclosure feedback
        // is painted locally below, leaving every real expand/collapse immediate.

        m_feedbackAnimation = new QVariantAnimation(this);
        m_feedbackAnimation->setObjectName(QString::fromLatin1(kTreeFeedbackAnimationName));
        m_feedbackAnimation->setStartValue(0.0);
        m_feedbackAnimation->setEndValue(0.0);
        connect(m_feedbackAnimation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
                    const QRect dirty = feedbackPaintRect();
                    m_feedbackOpacity = value.toReal();
                    m_lastFeedbackRect = feedbackPaintRect();
                    viewport()->update(dirty.united(m_lastFeedbackRect));
                });
        connect(m_feedbackAnimation, &QVariantAnimation::finished, this,
                [this] { finishFeedback(); });

        m_inputCadenceTimer = new QTimer(this);
        m_inputCadenceTimer->setSingleShot(true);
        m_inputCadenceTimer->setInterval(kTreeInputCadenceIntervalMs);

        MotionPolicy::observeReduced(this, [this](bool reduced) {
            if (reduced)
                finishFeedback();
        });
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        const QPersistentModelIndex candidate(indexAt(event->pos()));
        const bool disclosure = event->button() == Qt::LeftButton &&
                                candidate.isValid() &&
                                model()->hasChildren(candidate) &&
                                disclosureRect(candidate).contains(event->pos());
        const bool wasExpanded = disclosure && isExpanded(candidate);
        const bool withinRowBound = disclosure && hasFewerThanFeedbackRowLimit();

        QTreeView::mousePressEvent(event);

        if (disclosure && candidate.isValid() && isExpanded(candidate) != wasExpanded)
            handleUserToggle(candidate, withinRowBound);
    }

    void keyPressEvent(QKeyEvent *event) override {
        const QPersistentModelIndex candidate(currentIndex());
        const bool wasExpanded = candidate.isValid() && isExpanded(candidate);
        const bool withinRowBound =
            candidate.isValid() && hasFewerThanFeedbackRowLimit();

        QTreeView::keyPressEvent(event);

        if (candidate.isValid() && isExpanded(candidate) != wasExpanded)
            handleUserToggle(candidate, withinRowBound);
    }

    void paintEvent(QPaintEvent *event) override {
        QTreeView::paintEvent(event);
        if (m_feedbackOpacity <= 0.0 || !m_feedbackIndex.isValid())
            return;

        const QRect indicator = disclosureRect(m_feedbackIndex);
        if (!indicator.isValid() || !viewport()->rect().intersects(indicator))
            return;

        QColor accent = palette().color(QPalette::Highlight);
        accent.setAlphaF(qBound<qreal>(0.0, accent.alphaF() * 0.55 * m_feedbackOpacity, 1.0));
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(accent, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(indicator.adjusted(3, 3, -3, -3));
    }

private:
    bool hasFewerThanFeedbackRowLimit() const {
        if (!model())
            return false;

        QModelIndex index = model()->index(0, 0, rootIndex());
        int rows = 0;
        for (; index.isValid() && rows < kTreeAnimationVisibleRowLimit;
             ++rows, index = indexBelow(index)) {
        }
        return rows < kTreeAnimationVisibleRowLimit;
    }

    QRect disclosureRect(const QModelIndex &index) const {
        if (!index.isValid())
            return {};
        const QRect row = visualRect(index);
        if (!row.isValid())
            return {};
        if (layoutDirection() == Qt::RightToLeft)
            return QRect(row.right() + 1, row.top(), indentation(), row.height());
        return QRect(row.left() - indentation(), row.top(), indentation(), row.height());
    }

    QRect feedbackPaintRect() const {
        QRect current;
        if (m_feedbackIndex.isValid())
            current = disclosureRect(m_feedbackIndex).adjusted(-2, -2, 2, 2);
        return m_lastFeedbackRect.united(current);
    }

    void handleUserToggle(const QModelIndex &index, bool withinRowBound) {
        const InputCadence cadence =
            m_inputCadenceTimer->isActive() ? InputCadence::Rapid : InputCadence::Normal;
        m_inputCadenceTimer->start();

        if (!withinRowBound || !MotionPolicy::allowFor(cadence)) {
            finishFeedback();
            return;
        }

        const int duration = MotionPolicy::duration(MotionDuration::Fast);
        if (duration == 0) {
            finishFeedback();
            return;
        }

        finishFeedback();
        m_feedbackIndex = index;
        m_feedbackOpacity = 1.0;
        m_lastFeedbackRect = feedbackPaintRect();
        m_feedbackAnimation->setDuration(duration);
        m_feedbackAnimation->setEasingCurve(MotionPolicy::easing());
        m_feedbackAnimation->setStartValue(1.0);
        m_feedbackAnimation->setEndValue(0.0);
        m_feedbackAnimation->start();
    }

    void finishFeedback() {
        const QRect dirty = feedbackPaintRect();
        m_feedbackAnimation->stop();
        m_feedbackAnimation->setStartValue(0.0);
        m_feedbackAnimation->setEndValue(0.0);
        m_feedbackAnimation->setCurrentTime(0);
        m_feedbackOpacity = 0.0;
        m_feedbackIndex = QPersistentModelIndex();
        m_lastFeedbackRect = {};
        if (dirty.isValid())
            viewport()->update(dirty);
    }

    QVariantAnimation *m_feedbackAnimation = nullptr;
    QTimer *m_inputCadenceTimer = nullptr;
    QPersistentModelIndex m_feedbackIndex;
    QRect m_lastFeedbackRect;
    qreal m_feedbackOpacity = 0.0;
};

FilePanel::FilePanel(QWidget *parent) : FilePanel(QFont(), parent) {}

FilePanel::FilePanel(const QFont &initialListFont, QWidget *parent) : QWidget(parent) {
    // A plain QWidget subclass does not paint a stylesheet background unless
    // it is told to; without this the CRT theme's scanline texture stops at
    // the Qt-provided widgets and this one stays flat. light.qss/dark.qss
    // declare `background: transparent` for this class so their appearance is
    // unchanged -- it shows the parent, exactly as it did before.
    setAttribute(Qt::WA_StyledBackground, true);
    m_focusAnimation = new QPropertyAnimation(this, "focusProgress", this);
    m_networkStatusRevealTimer = new QTimer(this);
    m_networkStatusRevealTimer->setSingleShot(true);
    m_networkStatusRevealTimer->setInterval(150);
    connect(m_networkStatusRevealTimer, &QTimer::timeout, this, [this] {
        showNetworkStatus(m_pendingNetworkStatus, m_pendingNetworkAttempt);
    });

    m_networkStatusColorAnimation = new QVariantAnimation(this);
    m_networkStatusColorAnimation->setObjectName(QStringLiteral("NetworkStatusColorAnimation"));
    connect(m_networkStatusColorAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                m_networkStatusDotColor = value.value<QColor>();
                refreshTabIcons();
            });
    // Long tab and address text must clip inside this splitter pane rather
    // than turning into a minimum width for the whole pane.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_model = new FileSystemModel(this);
    m_view = new FileListView(this);
    if (!initialListFont.family().isEmpty() && initialListFont.pointSize() > 0) {
        m_view->viewport()->setFont(initialListFont);
        m_view->setFont(initialListFont);
        // See setListTypography()'s matching call: the header does not reliably
        // keep tracking the view's font on its own, so it needs the same
        // explicit assignment here at construction too, or it starts pinned to
        // whatever font it resolved before this ran (menu font size, not list).
        m_view->horizontalHeader()->setFont(initialListFont);
        m_lastFontPt = initialListFont.pointSize();
        const int iconPx = qBound(16, QFontMetrics(initialListFont).height() + 4, 48);
        m_view->setIconSize(QSize(iconPx, iconPx));
    }
    m_view->setModel(m_model);
    m_view->installEventFilter(this);

    m_tabManager = new TabManager(this);
    m_tabBar = new TabBar(this);
    m_tabBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // "+" at the far right of the tab strip opens a new tab in this panel,
    // lined up directly above the address row's "✳" button.
    m_addTabButton = new QToolButton(this);
    m_addTabButton->setObjectName(QStringLiteral("PanelAddTabButton"));
    m_addTabButton->setText(QStringLiteral("+"));
    m_addTabButton->setAutoRaise(true);
    m_addTabButton->setFocusPolicy(Qt::NoFocus);
    m_addTabButton->setToolTip(tr("New Tab"));
    connect(m_addTabButton, &QToolButton::clicked, this, [this]() {
        emit panelActivated(this); // act on this panel
        newTab();
    });
    m_addressBar = new BreadcrumbBar(this);
    m_addressBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_addressBar->setFocusPolicy(Qt::ClickFocus); // keep it out of the Tab chain

    // Folder-tree toggle: first item in the address row. The button paints a
    // compact three-line glyph itself so it stays visually aligned with the
    // lightweight back/forward arrows in every theme.
    m_treeButton = new TreeToggleButton(this);
    m_treeButton->setObjectName(QStringLiteral("PanelTreeButton"));
    m_treeButton->setAutoRaise(true);
    m_treeButton->setCheckable(true);
    m_treeButton->setFocusPolicy(Qt::NoFocus);
    m_treeButton->setToolTip(tr("Folder tree"));
    connect(m_treeButton, &QToolButton::toggled, this, [this](bool on) {
        DirectoryTreeView *tree = on ? ensureDirectoryTree() : m_dirTree;
        if (tree)
            tree->setVisible(on);
        if (on) {
            ensureTreeRootsReady();
            syncTreeToPath(m_model->rootPath());
        }
    });

    // Back/Forward live inline at the head of the path row (no separate
    // toolbar) to save a full row of vertical space.
    m_backButton = new QToolButton(this);
    m_backButton->setText(QStringLiteral("←"));
    m_backButton->setAutoRaise(true);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    m_backButton->setToolTip(tr("Back"));
    connect(m_backButton, &QToolButton::clicked, this, &FilePanel::goBack);

    m_forwardButton = new QToolButton(this);
    m_forwardButton->setText(QStringLiteral("→"));
    m_forwardButton->setAutoRaise(true);
    m_forwardButton->setFocusPolicy(Qt::NoFocus);
    m_forwardButton->setToolTip(tr("Forward"));
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePanel::goForward);

    // "*" opens a menu of all keyboard shortcuts (click a row to run it).
    m_starButton = new QToolButton(this);
    m_starButton->setText(QStringLiteral("✳"));
    m_starButton->setAutoRaise(true);
    m_starButton->setFocusPolicy(Qt::NoFocus);
    m_starButton->setToolTip(tr("Commands / shortcuts"));
    connect(m_starButton, &QToolButton::clicked, this, [this]() {
        emit panelActivated(this); // act on this panel
        emit shortcutMenuRequested(
            this, m_starButton->mapToGlobal(QPoint(0, m_starButton->height())));
    });

    // Computer view: the drives, user folders, removable media, saved servers
    // and discovered hosts, listed in this panel. Sits between the navigation
    // arrows and the path because that is where it belongs in the reading order
    // -- it is the level above every path the breadcrumb can show.
    m_computerButton = new QToolButton(this);
    m_computerButton->setObjectName(QStringLiteral("PanelComputerButton"));
    m_computerButton->setIcon(
        IconCache::instance().glyphIcon(QStringLiteral(":/icons/computer.svg")));
    m_computerButton->setAutoRaise(true);
    m_computerButton->setFocusPolicy(Qt::NoFocus);
    m_computerButton->setToolTip(tr("Computer"));
    connect(m_computerButton, &QToolButton::clicked, this, [this]() {
        emit panelActivated(this); // act on this panel
        emit computerViewRequested(this);
    });

    // Keep the tree toggle visually aligned with the navigation controls even
    // though the menu glyph has a different natural text width.
    QSize addressButtonSize = m_treeButton->sizeHint();
    for (QToolButton *button : {m_backButton, m_forwardButton, m_starButton})
        addressButtonSize = addressButtonSize.expandedTo(button->sizeHint());
    for (QToolButton *button :
         {m_treeButton, m_backButton, m_forwardButton, m_computerButton, m_starButton})
        button->setFixedSize(addressButtonSize);

    auto *addressRow = new QWidget(this);
    // Named so a theme can reach it. It is a bare QWidget, which Qt DOES paint
    // the sheet's `QWidget { background }` onto -- covering the panel behind it
    // with a flat colour. The CRT sheet makes it transparent instead, so the
    // panel's own texture shows through; letting each container tile its own
    // copy would also put the scanlines out of phase between neighbours, since
    // a background-image's origin is the widget's own top-left corner.
    addressRow->setObjectName(QStringLiteral("PanelAddressRow"));
    auto *addressLayout = new QHBoxLayout(addressRow);
    addressLayout->setContentsMargins(0, 0, 0, 0);
    addressLayout->setSpacing(2);
    addressLayout->addWidget(m_treeButton);
    addressLayout->addWidget(m_backButton);
    addressLayout->addWidget(m_forwardButton);
    addressLayout->addWidget(m_computerButton);
    addressLayout->addWidget(m_addressBar, 1);
    addressLayout->addWidget(m_starButton);

    m_filterBar = new QLineEdit(this);
    m_filterBar->setClearButtonEnabled(true);
    m_filterBar->setPlaceholderText(tr("Filter: type to narrow the list, Esc to clear"));
    m_filterBar->hide(); // revealed on demand via showQuickFilter()
    m_filterBar->installEventFilter(this);

    m_statusBar = new StatusBarWidget(this);

    // Tab row: tab strip, then the trailing "+".
    // The tree toggle and navigation controls live together in the address row.
    auto *tabRow = new QWidget(this);
    m_tabRow = tabRow;
    auto *tabRowLayout = new QHBoxLayout(tabRow);
    tabRowLayout->setContentsMargins(0, 0, 0, 0);
    tabRowLayout->setSpacing(2);
    tabRowLayout->addWidget(m_tabBar, 1);
    tabRowLayout->addWidget(m_addTabButton, 0, Qt::AlignVCenter);

    m_bodyStack = new QStackedWidget(this);
    m_bodyStack->addWidget(m_view);

    m_bodySplitter = new QSplitter(Qt::Horizontal, this);
    m_bodySplitter->addWidget(m_bodyStack);
    m_bodySplitter->setStretchFactor(0, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // setSpacing() is one uniform value for the whole layout -- it cannot be toggled
    // between addWidget() calls to get a different gap for only some pairs, so the
    // baseline is 0 and the two gaps that are actually wanted are inserted explicitly.
    layout->setSpacing(0);
    layout->addWidget(tabRow);
    layout->addSpacing(2);
    layout->addWidget(addressRow);
    // No spacing from here down: the address row draws its own left/right border
    // (AddressRowBorderOverlay) and the file list draws its own (`QTableView { border:
    // ... }` in the theme), each scoped to just its own widget's height. The 2px
    // inter-widget spacing this used to inherit left an unstyled gap between those two
    // borders, which read as the left/right lines breaking partway down instead of
    // running the panel's full height as one piece.
    layout->addWidget(m_filterBar);
    layout->addWidget(m_bodySplitter, 1);
    layout->addSpacing(2);
    layout->addWidget(m_statusBar);

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] {
                updateStatus();
            });
    // The computer view's disk readout follows the cursor, so it has to track
    // currentChanged rather than selectionChanged: the arrow keys move the
    // cursor without altering the selection. Guarded on the view so a normal
    // directory does not pay a QStorageInfo call on every keystroke.
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] {
                if (m_computerProvider)
                    updateDiskInfo();
            });

    connect(m_statusBar, &StatusBarWidget::zoomOutRequested, this, &FilePanel::zoomViewOut);
    connect(m_statusBar, &StatusBarWidget::zoomInRequested, this, &FilePanel::zoomViewIn);
    connect(m_view, &FileListView::zoomRequested, this, [this](int direction) {
        if (direction > 0)
            zoomViewIn();
        else
            zoomViewOut();
    });
    connect(m_view, &FileListView::rowSpaced, this, &FilePanel::calculateDirSizeForRow);

    // Keep the chrome (tabs, breadcrumb, column header) as short as one file
    // row so the panel reads as a single dense list rather than stacked bars.
    const int rowH = qMax(m_view->fontMetrics().height(), 16) + 6;
    m_view->verticalHeader()->setDefaultSectionSize(rowH);
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->horizontalHeader()->setFixedHeight(rowH);
    // The tab bar keeps its natural height so the style vertically centres the
    // tab text; forcing it shorter left the text stuck at the bottom.
    m_addressBar->setFixedHeight(rowH);
    m_treeButton->setFixedSize(rowH, rowH);
    m_backButton->setFixedSize(rowH, rowH);
    m_forwardButton->setFixedSize(rowH, rowH);
    m_computerButton->setFixedSize(rowH, rowH);
    m_starButton->setFixedSize(rowH, rowH);
    m_addTabButton->setFixedSize(rowH, rowH);
    updateNavButtons();

    connect(m_filterBar, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_model->setNameFilter(text); });

    connect(m_view, &QAbstractItemView::activated, this, &FilePanel::onActivated);
    connect(m_addressBar, &BreadcrumbBar::pathActivated, this, &FilePanel::onAddressBarEntered);
    connect(m_model, &FileSystemModel::loadFinished, this, [this](int) {
        // In flat (search-results) mode there is no single directory: skip the
        // breadcrumb/tree/disk-usage sync, which all key off rootPath().
        const bool flat = m_model->isFlatMode();
        if (m_computerProvider) {
            // "computer://" is an identifier, not a path: split on "/" it would
            // render as segments that navigate nowhere, and the folder tree has
            // nothing to highlight for it either.
            m_addressBar->setCaption(tr("Computer"));
        } else {
            m_addressBar->setPath(flat ? QString() : m_model->rootPath());
            if (!flat)
                syncTreeToPath(m_model->rootPath());
        }
        if (!m_pendingSelection.isEmpty()) {
            QItemSelectionModel *sel = m_view->selectionModel();
            QModelIndex first;
            for (int row = 0; row < m_model->rowCount(); ++row) {
                if (m_pendingSelection.contains(m_model->fileInfoAt(row).path())) {
                    const QModelIndex idx = m_model->index(row, 0);
                    sel->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                    if (!first.isValid())
                        first = idx;
                }
            }
            // Keep focus on (and scroll to) the first restored row -- e.g. the
            // file just renamed -- rather than letting the reload jump to the
            // top. NoUpdate leaves the selection above intact.
            if (first.isValid()) {
                sel->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
                activeView()->scrollTo(first, QAbstractItemView::EnsureVisible);
            }
            m_pendingSelection.clear();
        }
        if (m_view->model()->rowCount() > 0 && !m_view->currentIndex().isValid())
            m_view->setCurrentIndex(m_view->model()->index(0, 0));
        updateStatus();
        updateDiskInfo();
        // A new set of rows: begin filling their thumbnails. Cheap and a no-op
        // on a local tab, so it costs nothing to call unconditionally.
        restartThumbnailSweep();
    });

    // Network connection status -> centred status-line message; the "Retry" link
    // in the failed state asks the model's session to reconnect.
    connect(m_model, &FileSystemModel::networkStateChanged, this,
            &FilePanel::onNetworkStateChanged);
    // A listing the backend could explain away (a share the server refuses to
    // enumerate). The connection is still up, so this is reported on its own
    // rather than through the connection-state path.
    connect(m_model, &FileSystemModel::listingFailed, this, &FilePanel::showListingError);
    connect(m_model, &FileSystemModel::networkSessionReused, this,
            &FilePanel::showReusedSessionNotice);
    connect(m_statusBar, &StatusBarWidget::retryRequested, this, [this] {
        // Same link for both states: re-prompt for credentials if the user had
        // cancelled the login, otherwise ask the session to reconnect.
        if (m_awaitingLogin)
            emit loginRequested(this);
        else
            m_model->retryNetwork();
    });

    connect(m_tabBar, &QTabBar::currentChanged, this, &FilePanel::onTabBarCurrentChanged);
    connect(m_tabBar, &TabBar::closeTabRequested, this, &FilePanel::closeTabAt);
    // Right-click on the tab strip opens the directory-favorites menu (owned by
    // MainWindow, which has the settings-backed favorites list).
    connect(m_tabBar, &TabBar::favoritesMenuRequested, this,
            [this](const QPoint &pos, int tabIndex) {
                emit panelActivated(this);
                emit favoritesMenuRequested(pos, tabIndex);
            });

    const int firstTab = m_tabManager->addTab(QString());
    m_tabBar->blockSignals(true);
    m_tabBar->addTab(tr("New Tab"));
    m_tabBar->setCurrentIndex(firstTab);
    m_tabBar->blockSignals(false);
    m_tabManager->setActiveIndex(firstTab);

    auto *upShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
    upShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(upShortcut, &QShortcut::activated, this, &FilePanel::navigateUp);

    auto *backShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    backShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(backShortcut, &QShortcut::activated, this, &FilePanel::goBack);

    auto *fwdShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
    fwdShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(fwdShortcut, &QShortcut::activated, this, &FilePanel::goForward);

    auto *selectAllShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_A), this);
    selectAllShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAllShortcut, &QShortcut::activated, this, &FilePanel::selectAll);

    auto *deselectAllShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), this);
    deselectAllShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deselectAllShortcut, &QShortcut::activated, this, &FilePanel::deselectAll);

    auto *invertShortcut = new QShortcut(QKeySequence(Qt::Key_Asterisk), this);
    invertShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(invertShortcut, &QShortcut::activated, this, &FilePanel::invertSelection);

    // TC-style: gray +/- select/unselect by wildcard mask (Ctrl+A / Ctrl+Shift+A
    // still select/deselect everything).
    auto *selectPlusShortcut = new QShortcut(QKeySequence(Qt::Key_Plus), this);
    selectPlusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectPlusShortcut, &QShortcut::activated, this, [this] { selectByPattern(true); });

    auto *selectMinusShortcut = new QShortcut(QKeySequence(Qt::Key_Minus), this);
    selectMinusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectMinusShortcut, &QShortcut::activated, this, [this] { selectByPattern(false); });
}

bool FilePanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_dirTree && event->type() == QEvent::Show) {
        ensureTreeRootsReady();
        syncTreeToPath(m_model->rootPath());
    }

    const bool panelInput = watched == m_view || watched == m_iconView ||
                            watched == m_dirTree || watched == m_filterBar;
    if (panelInput && event->type() == QEvent::FocusIn) {
        animateFocus(true);
        if (watched == m_view || watched == m_iconView)
            emit panelActivated(this);
    } else if (panelInput && event->type() == QEvent::FocusOut) {
        // The focus widget is updated after the outgoing widget's event. Defer
        // the check so focus moving inside this panel leaves its border intact.
        QTimer::singleShot(0, this, [this] {
            QWidget *focus = QApplication::focusWidget();
            animateFocus(focus && (focus == this || isAncestorOf(focus)));
        });
    }

    // Plain Tab (not Ctrl+Tab, which cycles tabs) jumps to the other panel.
    // Both views, not just the list: the panel keeps working the same way after
    // switching to thumbnails, and Tab silently doing nothing there is the kind
    // of gap that only shows up once someone actually uses that mode.
    if ((watched == m_view || watched == m_iconView) && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) &&
            !(ke->modifiers() & Qt::ControlModifier)) {
            emit switchPanelRequested();
            return true;
        }
    }

    // Single-click-on-already-selected-thumbnail rename, mirroring
    // FileListView's mousePress/Release/DoubleClick handling: record the index
    // on press (only if it was already the sole selection), start the rename
    // timer on release over the same cell, and cancel on a double-click (which
    // opens instead via QAbstractItemView::activated).
    if (m_iconView && watched == m_iconView->viewport()
        && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        m_iconRenameClickTimer->stop();
        m_iconRenameClickIndex = QModelIndex();
        if (me->button() == Qt::LeftButton &&
            !(me->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
            const QModelIndex idx = m_iconView->indexAt(me->pos());
            // Note: QListView (icon mode) selects single items (column 0), not
            // whole rows, so selectedRows()/isRowSelected() are always empty
            // here -- use isSelected()/selectedIndexes() instead.
            if (idx.isValid() && !m_model->isParentEntry(idx.row()) &&
                (m_model->flags(idx) & Qt::ItemIsEditable) &&
                m_iconView->selectionModel()->isSelected(idx) &&
                m_iconView->selectionModel()->selectedIndexes().size() == 1)
                m_iconRenameClickIndex = idx;
        }
    } else if (m_iconView && watched == m_iconView->viewport()
               && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_iconRenameClickIndex.isValid() &&
            m_iconView->indexAt(me->pos()) == QModelIndex(m_iconRenameClickIndex))
            m_iconRenameClickTimer->start(QApplication::doubleClickInterval() + 10);
        else
            m_iconRenameClickIndex = QModelIndex();
    } else if (m_iconView && watched == m_iconView->viewport()
               && event->type() == QEvent::MouseButtonDblClick) {
        m_iconRenameClickTimer->stop();
        m_iconRenameClickIndex = QModelIndex();
    }

    if (watched == m_filterBar && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            hideQuickFilter();
            return true;
        }
        // Enter / Down hand control to the list without losing the filter.
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter ||
            ke->key() == Qt::Key_Down) {
            m_view->setFocus();
            if (m_model->rowCount() > 0 && !m_view->currentIndex().isValid())
                m_view->setCurrentIndex(m_model->index(0, 0));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FilePanel::paintEvent(QPaintEvent *event) {
    // No border around the active panel. Which panel is active is already said
    // by its selection colour (QTableView[panelActive] in the themes) and by
    // where the cursor frame is; an accent rectangle on top of that was a third
    // way of saying the same thing, and a heavy one.
    //
    // focusProgress is still animated -- the property, its animation and the
    // tests that pin them are untouched -- so anything else that wants to
    // follow focus still can.
    QWidget::paintEvent(event);
}

void FilePanel::setFocusProgress(qreal progress) {
    progress = qBound<qreal>(0.0, progress, 1.0);
    if (qFuzzyCompare(m_focusProgress, progress))
        return;
    m_focusProgress = progress;
    update();
}

void FilePanel::animateFocus(bool focused) {
    const qreal target = focused ? 1.0 : 0.0;
    const qreal start = m_focusProgress;
    m_focusAnimation->stop();

    const int duration = MotionPolicy::duration(MotionDuration::Fast);
    if (duration == 0) {
        setFocusProgress(target);
        return;
    }

    setFocusProgress(start);
    m_focusAnimation->setDuration(duration);
    m_focusAnimation->setEasingCurve(MotionPolicy::easing());
    m_focusAnimation->setStartValue(start);
    m_focusAnimation->setEndValue(target);
    m_focusAnimation->start();
}

void FilePanel::showQuickFilter() {
    m_filterBar->show();
    m_filterBar->setFocus();
    m_filterBar->selectAll();
}

void FilePanel::hideQuickFilter() {
    m_filterBar->clear(); // textChanged -> setNameFilter("") restores full list
    m_filterBar->hide();
    m_view->setFocus();
}

FilePanel::NavEntry FilePanel::currentLocation() const {
    // Archive locations ("/" virtual roots) can't be restored by history, so they
    // are never recorded -- leaving an archive already drops to its host dir.
    if (m_archiveProvider)
        return {};
    // The computer view IS recorded: it is rebuilt from scratch on the way back
    // (the rows are re-collected, so nothing stale is restored), and without an
    // entry Back from a drive opened out of the view had nowhere to return to.
    if (m_computerProvider) {
        NavEntry computer;
        computer.computerView = true;
        computer.dir = m_computerExitDir; // makes it valid(), and the fallback
        computer.conn = m_model->peekConnection();
        if (auto tab = m_tabManager->activeTab()) {
            computer.connScheme = tab->connScheme;
            computer.connLabel = tab->connLabel;
            computer.connInfo = tab->connInfo;
        }
        return computer;
    }
    NavEntry e;
    if (m_model->isFlatMode()) {
        e.flat = true;
        e.flatPaths = m_flatPaths;
    } else {
        e.dir = m_model->rootPath();
    }
    // Record the backend so Back/Forward can return to a live server connection
    // (and its "user@host" tab label), not merely re-walk the path locally.
    e.conn = m_model->peekConnection();
    if (auto tab = m_tabManager->activeTab()) {
        e.connScheme = tab->connScheme;
        e.connLabel = tab->connLabel;
        e.connInfo = tab->connInfo;
    }
    return e;
}

void FilePanel::applyHistoryEntry(const NavEntry &entry) {
    // Restoring a history entry: never re-push (goBack/goForward manage the stacks).
    // Leaving an archive first puts back the backend it was entered from; the
    // attachConnection() below then installs the one this entry was viewed
    // through, which for the entry the archive was entered from is the same
    // still-running session.
    leaveVirtualBackend();
    if (m_filterBar->isVisible()) {
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    // Restore the backend this location was viewed through BEFORE listing, so a
    // remote entry lists through its (re-adopted) server and a local entry lists
    // locally. attachConnection() also re-points the model's active session.
    m_model->attachConnection(entry.conn);
    if (auto tab = m_tabManager->activeTab()) {
        tab->provider = entry.conn.provider;
        tab->session = entry.conn.session;
        tab->connScheme = entry.connScheme;
        tab->connLabel = entry.connLabel;
        tab->authLabel = entry.conn.label;
        tab->authFactory = entry.conn.authRetry;
        tab->connInfo = entry.connInfo;
    }
    if (!m_model->hasNetworkSession()) {
        resetNetworkStatusFeedback();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
    }

    if (entry.computerView) {
        // Same rebuild-don't-restore rule as a backgrounded tab: ask the owner
        // for a fresh set of rows. Suppressed history, because goBack/goForward
        // own the stacks and showComputer would otherwise push onto them.
        m_flatPaths.clear();
        m_restoringComputerView = true;
        emit computerViewRequested(this);
        m_restoringComputerView = false;
        if (m_computerProvider) {
            m_computerExitDir = entry.dir;
            if (auto tab = m_tabManager->activeTab()) {
                tab->path = entry.dir;
                tab->flatPaths.clear();
            }
        } else {
            // Nothing rebuilt it; fall back to the directory the entry carries
            // rather than leaving the panel where it was.
            m_model->setRootPath(entry.dir);
            emit pathChanged(entry.dir);
        }
    } else if (entry.flat) {
        m_flatPaths = entry.flatPaths;
        m_model->setFlatEntries(entry.flatPaths);
    } else {
        m_flatPaths.clear();
        m_model->setRootPath(entry.dir);
        emit pathChanged(entry.dir);
        if (auto tab = m_tabManager->activeTab()) {
            tab->path = entry.dir;
            tab->flatPaths.clear(); // no longer a search-results listing
            tab->title.clear();     // drop the keyword title -> path-derived label
        }
    }
    // Refresh the tab label + protocol icon for BOTH branches: a flat (search)
    // history entry can still carry a connection, and used to restore with a
    // stale label/icon because this refresh sat only in the non-flat branch.
    if (auto tab = m_tabManager->activeTab()) {
        const int idx = m_tabManager->activeIndex();
        m_tabBar->setTabText(idx, tabLabelFor(tab));
        refreshTabIcons();
    }
}

void FilePanel::cancelRemoteThumbnails() {
    // Whatever the icon grid asked for belongs to the listing we are leaving, so
    // drop it rather than spend the link on thumbnails for rows about to
    // disappear. Harmless on a local tab (no provider is registered with the
    // fetcher, so this is a no-op).
    // Stop feeding rows in as well: the sweep is what would otherwise keep
    // re-submitting for the listing we are walking away from, refilling the
    // queue as fast as the cancellation empties it.
    m_thumbSweep.reset(0);
    if (m_model->hasNetworkSession())
        ThumbnailCache::instance().cancelRemote(m_model->provider());
}

void FilePanel::restartThumbnailSweep() {
    // Only network listings pay a per-row price worth scheduling around; local
    // thumbnails are cheap and the ordinary repaint path covers them.
    if (m_model->connectionId().isEmpty()) {
        m_thumbSweep.reset(0);
        return;
    }
    m_thumbSweep.reset(m_model->rowCount());
    // Start where the user is looking rather than at row 0: after a refresh the
    // view often keeps its scroll position.
    int first = 0, last = 0;
    if (m_iconView && m_iconView->visibleRowRange(&first, &last))
        m_thumbSweep.focusVisibleRowsWithAdjacentViewports(first, last);
    pumpThumbnailSweep();
}

void FilePanel::prefetchVisibleThumbnails(int firstRow, int lastRow) {
    // Scrolling stopped somewhere new: serve those rows next. Rows already
    // fetched stay fetched, and rows skipped past are picked up later when the
    // sweep wraps -- so this is a re-prioritisation, not a restart.
    m_thumbSweep.focusVisibleRowsWithAdjacentViewports(firstRow, lastRow);
    pumpThumbnailSweep();
}

void FilePanel::pumpThumbnailSweep() {
    const QString connectionId = m_model->connectionId();
    if (connectionId.isEmpty() || !m_thumbnailDelegate || m_thumbSweep.complete())
        return;
    // The listing changed size underneath the sweep (rows removed, filter
    // applied). Re-base rather than index off the end.
    if (m_thumbSweep.rowCount() != m_model->rowCount()) {
        restartThumbnailSweep();
        return;
    }

    // Device pixels, not the logical icon size: the delegate paints what it
    // asked for at that same size, and the thumbnail cache keys on it. Asking
    // for the logical size here would fill the cache under a key paint() never
    // looks up -- every row fetched twice, and the sweep never getting ahead.
    const int iconSize = m_thumbnailDelegate->thumbnailPixelSize();
    std::shared_ptr<FileProvider> provider = m_model->providerPtr();

    // Feed rows in until the fetcher pushes back. Deferred means its queue is
    // full: put that row back so it is retried, and stop -- the next completed
    // fetch pumps again. This is the whole flow-control mechanism; without the
    // put-back a full queue would silently swallow rows.
    while (!m_thumbSweep.complete()) {
        bool foreground = false;
        const int row = m_thumbSweep.next(&foreground);
        if (row < 0)
            break;
        const FileInfo info = m_model->fileInfoAt(row);
        const QString path = info.path();
        if (path.isEmpty() || info.isDir() || !ThumbnailCache::canThumbnail(path))
            continue; // nothing to fetch for this row; the sweep moves on
        const auto outcome = ThumbnailCache::instance().requestRemoteThumbnail(
            provider, connectionId, path, info.modified().toSecsSinceEpoch(), info.size(),
            iconSize, nullptr, foreground ? ThumbnailCache::CacheIntent::Display
                                           : ThumbnailCache::CacheIntent::PersistOnly);
        if (outcome == ThumbnailCache::Request::Busy) {
            m_thumbSweep.putBack(row);
            break;
        }
    }
}

void FilePanel::navigateTo(const QString &path) {
    cancelDirectorySizeTask();
    cancelRemoteThumbnails();

    // An ordinary path means the user is on their way out of the computer view --
    // and every route out arrives here: the folder tree, a favourite, Back, "go
    // to the other panel's directory", a drop, an activated row. Leaving has to
    // happen before the provider is read below, because the synthetic backend
    // cannot answer for a real path: isDir() says no for anything that is not
    // its own root, so the guard further down silently dropped the navigation,
    // and the callers that reach setRootPath() directly instead got an empty
    // listing out of it. Handled here rather than at each call site so a route
    // added later cannot forget.
    const bool leavingComputerView =
        m_computerProvider && !ComputerProvider::isComputerPath(path);
    // Snapshot before the backend goes: currentLocation() can only describe the
    // computer view while it is still installed, and this is the entry Back
    // needs in order to come back to it.
    const NavEntry fromComputerView = leavingComputerView ? currentLocation() : NavEntry();
    if (leavingComputerView)
        leaveComputerView();

    // Go through the model's provider so this works for remote backends too;
    // for the local provider these are the same QDir/QFileInfo calls as before.
    FileProvider *provider = m_model->provider();
    const QString cleaned = provider->cleanPath(path);
    // For a network tab, DON'T do a synchronous isDir() here: on a remote backend
    // it is a blocking GUI-thread round-trip AND, right after connectNetwork, it
    // fails because the provider hasn't connected yet (which used to abort the
    // very first remote navigation, so nothing ever listed). Detect "network" by
    // the presence of a session, not displayName() -- the latter is empty until
    // the connect lands. The async listing that follows reveals a bad path. Local
    // and archive backends keep the cheap in-process isDir guard.
    if (!m_model->hasNetworkSession() && !provider->isDir(cleaned))
        return;
    // Snapshot where we're leaving (dir or flat listing) BEFORE mutating the view.
    // Stepping out of the computer view uses the snapshot taken above, since by
    // now the backend that could describe it is gone.
    const NavEntry from = leavingComputerView ? fromComputerView : currentLocation();
    if (m_filterBar->isVisible()) {
        // setRootPath() clears the model filter; just tidy the (now stale) bar.
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    if (m_suppressHistoryOnce)
        m_suppressHistoryOnce = false; // the initial post-connect listing; don't record the transient local dir
    else if (from.isValid())
        pushHistory(from);
    m_forwardHistory.clear();
    m_flatPaths.clear(); // leaving any flat listing for a real directory
    m_model->setRootPath(cleaned);
    emit pathChanged(cleaned);

    if (auto tab = m_tabManager->activeTab()) {
        tab->path = cleaned;
        tab->flatPaths.clear(); // no longer a search-results listing
        tab->title.clear();     // drop the keyword title -> path-derived label
        updateActiveTabLabel();
    }
    updateNavButtons();
}

void FilePanel::onNetworkStateChanged(int state, int attempt) {
    // Any genuine session state change supersedes a pending manual-login prompt.
    m_awaitingLogin = false;

    // Connecting feedback is intentionally deferred so a cached or local-network
    // session does not flash a transient status. Failure remains immediate so its
    // retry control can always be used at once.
    switch (state) {
    case NetworkSession::Connecting:
    case NetworkSession::Reconnecting:
        m_pendingNetworkStatus = state;
        m_pendingNetworkAttempt = attempt;
        m_networkStatusVisible = false;
        m_networkStatusColorAnimation->stop();
        m_networkStatusDotColor = QColor();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
        m_networkStatusRevealTimer->start();
        break;
    case NetworkSession::Failed:
        m_networkStatusRevealTimer->stop();
        showNetworkStatus(state, attempt);
        break;
    case NetworkSession::Connected:
        resetNetworkStatusFeedback();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
        // displayName() (user@host) is only known once connected, and the tab
        // label was first built before that -- refresh it so the connected tab
        // shows "user@host : dir" instead of a bare directory name.
        updateActiveTabLabel();
        break;
    case NetworkSession::Idle:
    default:
        resetNetworkStatusFeedback();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
        break;
    }
    // Keep the active tab's status-dot badge in sync with the live session state.
    refreshTabIcons();
}

void FilePanel::showNetworkStatus(int state, int attempt) {
    m_networkStatusVisible = true;
    switch (state) {
    case NetworkSession::Connecting:
        m_statusBar->setConnectionStatus(tr("正在等待连接…"), StatusBarWidget::ConnConnecting);
        animateNetworkStatusColor(QColor(0x9a, 0x9a, 0x9a));
        break;
    case NetworkSession::Reconnecting:
        m_statusBar->setConnectionStatus(
            tr("断线，正在重连（%1/%2）…").arg(attempt).arg(NetworkSession::kMaxReconnects),
            StatusBarWidget::ConnReconnecting);
        animateNetworkStatusColor(QColor(0xd0, 0x8a, 0x2a));
        break;
    case NetworkSession::Failed: {
        const QString err = m_model->lastNetworkError();
        m_statusBar->setConnectionStatus(
            err.isEmpty() ? tr("多次重连失败") : tr("连接失败：%1").arg(err),
            StatusBarWidget::ConnFailed);
        animateNetworkStatusColor(QColor(0xe0, 0x4a, 0x4a));
        break;
    }
    default:
        m_networkStatusVisible = false;
        break;
    }
    refreshTabIcons();
}

void FilePanel::showReusedSessionNotice(const QString &user) {
    // The connection works, so nothing here is an error or offers a retry. What
    // it is, is a change of identity the user did not choose: Windows will not
    // open a second session to one server under a different account, so the
    // browse runs as whoever already had one. Saying so is the whole point --
    // silently browsing as someone else is the thing being fixed.
    m_networkStatusRevealTimer->stop();
    m_networkStatusColorAnimation->stop();
    m_awaitingLogin = false;
    m_networkStatusVisible = true;
    m_statusBar->setConnectionStatus(user.isEmpty()
                                         ? tr("正在复用已有会话")
                                         : tr("正在复用已有会话（用户：%1）").arg(user),
                                     StatusBarWidget::ConnNotice);
    m_networkStatusDotColor = QColor();
    refreshTabIcons();
}

void FilePanel::showListingError(const QString &reason) {
    if (reason.isEmpty())
        return;
    // Reported at the "failed" level so it is unmistakable and carries the
    // status line's Retry link, but WITHOUT touching the connection state: the
    // link is up, it is this directory that could not be read. The reveal timer
    // is stopped so a pending "connecting" message cannot overwrite this a
    // moment later.
    m_networkStatusRevealTimer->stop();
    m_networkStatusColorAnimation->stop();
    m_awaitingLogin = false;
    m_networkStatusVisible = true;
    m_statusBar->setConnectionStatus(tr("无法列出目录：%1").arg(reason),
                                     StatusBarWidget::ConnFailed);
    m_networkStatusDotColor = QColor(0xe0, 0x4a, 0x4a);
    refreshTabIcons();
}

void FilePanel::animateNetworkStatusColor(const QColor &target) {
    m_networkStatusColorAnimation->stop();
    if (MotionPolicy::reduced()) {
        m_networkStatusDotColor = target;
        refreshTabIcons();
        return;
    }

    QColor start = m_networkStatusDotColor;
    if (!start.isValid()) {
        start = target;
        start.setAlpha(0);
    }
    m_networkStatusColorAnimation->setDuration(MotionPolicy::duration(MotionDuration::Normal));
    m_networkStatusColorAnimation->setEasingCurve(MotionPolicy::easing());
    m_networkStatusColorAnimation->setStartValue(start);
    m_networkStatusColorAnimation->setEndValue(target);
    m_networkStatusColorAnimation->start();
}

void FilePanel::resetNetworkStatusFeedback() {
    m_networkStatusRevealTimer->stop();
    m_networkStatusColorAnimation->stop();
    m_networkStatusDotColor = QColor();
    m_pendingNetworkStatus = -1;
    m_pendingNetworkAttempt = 0;
    m_networkStatusVisible = false;
}

void FilePanel::showLoginPrompt() {
    // Called after the user cancelled the credential dialog: leave a clear,
    // actionable "需要登录" status with a login link rather than a blank tab that
    // looks connected. Set the flag AFTER any state-driven clear (the Idle state
    // change fires first and would otherwise wipe this).
    m_awaitingLogin = true;
    m_networkStatusRevealTimer->stop();
    m_networkStatusColorAnimation->stop();
    m_networkStatusDotColor = QColor(0xd0, 0x8a, 0x2a);
    m_networkStatusVisible = true;
    m_statusBar->setConnectionStatus(tr("需要登录"), StatusBarWidget::ConnNeedsAuth);
    refreshTabIcons();
}

void FilePanel::navigateTabTo(int tabIndex, const QString &path) {
    // Make the requested tab current first so navigateTo() (which always acts on
    // the active tab) mutates it. Setting the tab-bar index drives
    // onTabBarCurrentChanged(), which saves the outgoing tab and loads this one.
    if (tabIndex >= 0 && tabIndex < m_tabBar->count() &&
        tabIndex != m_tabBar->currentIndex())
        m_tabBar->setCurrentIndex(tabIndex);
    navigateTo(path);
}

void FilePanel::showSearchResultsInNewTab(const QString &keyword, const QStringList &paths) {
    if (paths.isEmpty())
        return;
    cancelDirectorySizeTask();
    // The results are a temporary virtual directory: give them their own tab
    // (titled after the search keyword) so the previously-browsed directory in
    // the current tab is left untouched.
    saveCurrentTabState();
    const int idx = m_tabManager->addTab(QString());
    if (auto tab = m_tabManager->tabAt(idx)) {
        tab->flatPaths = paths;
        tab->title = keyword.isEmpty() ? tr("Search results") : keyword;
    }
    m_tabManager->setActiveIndex(idx);
    m_tabBar->blockSignals(true);
    m_tabBar->addTab(tabLabelFor(m_tabManager->tabAt(idx)));
    m_tabBar->setCurrentIndex(idx);
    m_tabBar->blockSignals(false);

    // Leave archive browsing if active: a flat listing is a set of paths on the
    // backend the archive was entered from, not inside the archive. The computer
    // view goes for the same reason -- its rows are places, not paths.
    leaveVirtualBackend();
    if (m_filterBar->isVisible()) {
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    // A fresh tab has no prior location to return to; Back/Forward start empty.
    m_backHistory.clear();
    m_forwardHistory.clear();
    m_flatPaths = paths;
    m_model->setFlatEntries(paths);
    updateNavButtons();
    m_tabBar->animateCurrentTabActivation();
}

void FilePanel::pushHistory(const NavEntry &entry) {
    m_backHistory.append(entry);
}

void FilePanel::updateNavButtons() {
    m_backButton->setEnabled(!m_backHistory.isEmpty());
    m_forwardButton->setEnabled(!m_forwardHistory.isEmpty());
}

void FilePanel::updateDiskInfo() {
    // QStorageInfo answers about THIS machine's mount table, so it can only be
    // asked about a path that is on this machine. Handing it a server's path
    // does not fail -- it walks up until some local mount point matches, which
    // is how a share sitting in "/home" reported this machine's /home partition
    // byte for byte (the local panel beside it showed the identical numbers),
    // a share in "/" reported the local root partition, and a share in "/video"
    // reported nothing at all. The server's real free space was never among the
    // answers.
    //
    // Blank instead. It is not a placeholder for a better answer: reading a
    // server's free space means a per-protocol call that only some of these
    // backends have (SMB has smbc_statvfs, FTP has nothing at all), so a number
    // would appear on some tabs and not others with no way for the user to tell
    // which kind they are looking at. An empty readout says "not known here",
    // which is the truth, and it costs nothing to replace later.
    //
    // Flat search results span many directories and never had a readout either.
    if (m_model->isFlatMode()) {
        m_statusBar->setDiskInfo(0, 0);
        return;
    }
    // In the computer view the panel is not pointed at a directory, so there is
    // no "current disk" -- but the row under the cursor may name one, and the
    // figures for it belong exactly where the figures for a directory's disk
    // normally go. Only drives carry them; every other row leaves the readout
    // blank rather than showing the last drive's numbers.
    if (m_computerProvider) {
        const QModelIndex current = activeView() ? activeView()->currentIndex() : QModelIndex();
        ComputerEntry entry;
        if (current.isValid() && !m_model->isParentEntry(current.row()))
            entry = m_computerProvider->entryFor(m_model->fileInfoAt(current.row()).path());
        if (entry.kind == ComputerEntry::Kind::Drive && entry.bytesTotal > 0)
            m_statusBar->setDiskInfo(qMax<qint64>(0, entry.bytesFree), entry.bytesTotal);
        else
            m_statusBar->setDiskInfo(0, 0);
        return;
    }
    FileProvider *prov = m_model->provider();
    if (!prov || !prov->isLocalFilesystem()) {
        m_statusBar->setDiskInfo(0, 0);
        return;
    }
    const QStorageInfo storage(m_model->rootPath());
    m_statusBar->setDiskInfo(storage.bytesAvailable(), storage.bytesTotal());
}

QVector<int> FilePanel::selectedRowNumbers() const {
    QVector<int> rows;
    QSet<int> seen;
    const QModelIndexList indexes = m_view->selectionModel()->selectedIndexes();
    rows.reserve(indexes.size());
    for (const QModelIndex &idx : indexes) {
        if (!idx.isValid() || seen.contains(idx.row()))
            continue;
        seen.insert(idx.row());
        if (m_model->isParentEntry(idx.row()))
            continue;
        rows.append(idx.row());
    }
    // Selection order is whatever the user clicked; callers (copy, move, size)
    // want the on-screen order instead, so the destination list matches the
    // panel the user was looking at.
    std::sort(rows.begin(), rows.end());
    return rows;
}

void FilePanel::updateStatus() {
    const QVector<int> rows = selectedRowNumbers();
    int selectedCount = 0;
    qint64 selectedBytes = 0;
    for (int row : rows) {
        const FileInfo info = m_model->fileInfoAt(row);
        if (!info.isValid())
            continue;
        ++selectedCount;
        if (!info.isDir())
            selectedBytes += info.size();
    }
    // Total objects excludes the synthesized ".." row.
    int total = m_model->rowCount();
    if (total > 0 && m_model->isParentEntry(0))
        --total;
    m_statusBar->setSelectionInfo(selectedCount, selectedBytes, total);
}

void FilePanel::navigateUp() {
    // Reuse the ".." row the provider already put in the listing: it carries the
    // correct target for every backend -- the parent dir locally, the parent
    // virtual dir inside an archive, or the host directory when leaving an archive
    // root. This makes Backspace behave exactly like double-clicking ".." (which
    // otherwise QDir::cdUp() couldn't do for an archive's virtual "/" root).
    if (m_model->rowCount() > 0 && m_model->isParentEntry(0)) {
        onActivated(m_model->index(0, 0));
        return;
    }
    // No ".." row (e.g. the local filesystem root): fall back to QDir.
    const QString child = m_model->rootPath();
    QDir dir(child);
    if (dir.cdUp()) {
        navigateTo(dir.absolutePath());
        // Land the cursor on the directory we just came out of, so going up then
        // back down is one keystroke -- matches Explorer/Nautilus/TC. Normalise so
        // it matches the parent listing's entry path exactly.
        if (FileProvider *prov = m_model->provider())
            selectPathAfterReload(prov->cleanPath(child));
    }
}

void FilePanel::goBack() {
    if (m_backHistory.isEmpty())
        return;
    const NavEntry cur = currentLocation();
    if (cur.isValid())
        m_forwardHistory.append(cur);
    applyHistoryEntry(m_backHistory.takeLast());
    updateNavButtons();
}

void FilePanel::goForward() {
    if (m_forwardHistory.isEmpty())
        return;
    const NavEntry cur = currentLocation();
    if (cur.isValid())
        m_backHistory.append(cur);
    applyHistoryEntry(m_forwardHistory.takeLast());
    updateNavButtons();
}

void FilePanel::refresh() {
    if (m_computerProvider) {
        // Re-listing would replay the snapshot the view already holds. What a
        // refresh means here is "go and look again" -- a device may have been
        // plugged in or pulled out -- so the rows are re-collected. Reached from
        // F5 and from the panel refresh that follows a delete/move/copy.
        m_restoringComputerView = true; // a refresh is not a navigation
        emit computerViewRequested(this);
        m_restoringComputerView = false;
        return;
    }
    if (!m_model->rootPath().isEmpty())
        m_model->setRootPath(m_model->rootPath());
}

void FilePanel::exchangeLocationWith(FilePanel *other) {
    if (!other || other == this)
        return;

    cancelDirectorySizeTask();
    other->cancelDirectorySizeTask();

    // Both panels are about to show a different listing, so neither's queued
    // thumbnail fetches are wanted any more.
    cancelRemoteThumbnails();
    other->cancelRemoteThumbnails();

    // What moves below is each panel's backend, and an archive's isn't one the
    // other panel can be given -- so a panel inside an archive swaps the location
    // it entered it from, with its connection intact.
    backOutOfVirtualBackend();
    other->backOutOfVirtualBackend();

    const QString myPath = currentPath();
    const QString theirPath = other->currentPath();

    // Move the backends themselves rather than the paths. A path only means
    // anything to the backend it came from -- "/home" exists on an SMB share and
    // on this machine, so handing one panel the other's path would quietly land
    // it somewhere else entirely. Detaching first leaves both models local for
    // an instant, which is safe: nothing lists until setRootPath below.
    FileSystemModel::NetworkConn mine = m_model->detachConnection();
    FileSystemModel::NetworkConn theirs = other->m_model->detachConnection();
    m_model->attachConnection(std::move(theirs));
    other->m_model->attachConnection(std::move(mine));

    // The tab strip labels tabs from the connection recorded in TabState, so
    // both need re-stamping now that the connections have moved.
    stampActiveConnection();
    other->stampActiveConnection();

    // Land each panel on the location that came with its new backend. This does
    // what navigateTo's tail does -- relist, update the address bar, and record
    // the path on the active tab -- but without navigateTo's isDir() guard,
    // which would test the path against the backend that just arrived and, on a
    // still-connecting remote one, reject it.
    settleAtSwappedPath(theirPath);
    other->settleAtSwappedPath(myPath);

    refreshTabIcons();
    other->refreshTabIcons();
}

void FilePanel::settleAtSwappedPath(const QString &path) {
    m_flatPaths.clear(); // a swapped-in location is a real directory, not a search
    m_model->setRootPath(path);
    emit pathChanged(path);
    if (auto tab = m_tabManager->activeTab()) {
        tab->path = path;
        tab->flatPaths.clear();
        tab->title.clear(); // drop any keyword title so the label follows the path
        updateActiveTabLabel();
    }
    updateNavButtons();
}

bool FilePanel::isThumbnailMode() const {
    return m_bodyStack && m_bodyStack->currentWidget() == m_iconView;
}

DirectoryTreeView *FilePanel::ensureDirectoryTree() {
    if (m_dirTree)
        return m_dirTree;

    m_dirTreeModel = new DirectoryTreeModel(this);
    m_dirTree = new DirectoryTreeView(this);
    m_dirTree->setModel(m_dirTreeModel);
    m_dirTree->setHeaderHidden(true);
    m_dirTree->setFocusPolicy(Qt::ClickFocus);
    m_dirTree->setFont(m_view->font());
    m_dirTree->viewport()->setFont(m_view->viewport()->font());
    m_dirTree->installEventFilter(this);
    m_dirTree->hide();
    connect(m_dirTree, &QTreeView::activated, this, &FilePanel::activateTreeIndex);
    connect(m_dirTreeModel, &DirectoryTreeModel::childrenLoaded, this,
            [this](const QModelIndex &) { advanceTreeSync(); });

    m_bodySplitter->insertWidget(0, m_dirTree);
    m_bodySplitter->setStretchFactor(0, 0);
    m_bodySplitter->setStretchFactor(1, 1);
    m_bodySplitter->setSizes({200, 600});
    return m_dirTree;
}

IconFileView *FilePanel::ensureIconView() {
    if (m_iconView)
        return m_iconView;

    m_iconView = new IconFileView(this);
    m_iconView->setModel(m_model);
    m_iconView->setModelColumn(0);
    m_iconView->setViewMode(QListView::IconMode);
    m_iconView->setIconSize(QSize(64, 64));
    m_iconView->setResizeMode(QListView::Adjust);
    m_iconView->setMovement(QListView::Static);
    m_iconView->setDragEnabled(true);
    m_iconView->setDragDropMode(QAbstractItemView::DragDrop);
    m_iconView->setDefaultDropAction(Qt::CopyAction);
    m_iconView->viewport()->setAcceptDrops(true);
    m_iconView->setUniformItemSizes(true);
    m_iconView->setWordWrap(true);
    m_iconView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_iconView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_iconView->setSelectionModel(m_view->selectionModel());
    m_iconView->setFont(m_view->font());
    m_iconView->viewport()->setFont(m_view->viewport()->font());
    m_iconView->installEventFilter(this);
    m_iconView->viewport()->installEventFilter(this);
    connect(m_iconView, &QAbstractItemView::activated, this, &FilePanel::onActivated);
    connect(m_iconView, &IconFileView::visibleRangeSettled, this,
            &FilePanel::prefetchVisibleThumbnails);
    connect(m_iconView, &IconFileView::zoomRequested, this, [this](int direction) {
        if (direction > 0)
            zoomViewIn();
        else
            zoomViewOut();
    });
    connect(&ThumbnailCache::instance(), &ThumbnailCache::thumbnailReady, this,
            [this](const QString &) { pumpThumbnailSweep(); });

    m_thumbnailDelegate = new ThumbnailDelegate(m_iconView);
    m_thumbnailDelegate->setView(m_iconView);
    m_iconView->setItemDelegate(m_thumbnailDelegate);
    applyThumbnailFontSize(m_view->font().pointSize());

    m_iconRenameClickTimer = new QTimer(this);
    m_iconRenameClickTimer->setSingleShot(true);
    connect(m_iconRenameClickTimer, &QTimer::timeout, this, [this]() {
        const QModelIndex idx(m_iconRenameClickIndex);
        m_iconRenameClickIndex = QModelIndex();
        if (idx.isValid() && m_iconView->selectionModel()
            && m_iconView->selectionModel()->selectedIndexes().size() == 1
            && m_iconView->selectionModel()->isSelected(idx))
            m_iconView->edit(idx);
    });

    m_bodyStack->addWidget(m_iconView);
    emit iconViewCreated(m_iconView);
    return m_iconView;
}

QAbstractItemView *FilePanel::activeView() const {
    return isThumbnailMode() ? static_cast<QAbstractItemView *>(m_iconView)
                              : static_cast<QAbstractItemView *>(m_view);
}

void FilePanel::beginRenameCurrent() {
    QAbstractItemView *view = activeView();
    const QModelIndex idx = view->currentIndex();
    if (!idx.isValid() || m_model->isParentEntry(idx.row()))
        return;
    view->edit(idx.siblingAtColumn(FileSystemModel::NameColumn));
}

void FilePanel::toggleViewMode() {
    if (!m_bodyStack)
        return;
    const bool toThumb = !isThumbnailMode();
    IconFileView *iconView = toThumb ? ensureIconView() : m_iconView;
    m_bodyStack->setCurrentWidget(toThumb ? static_cast<QWidget *>(iconView)
                                          : static_cast<QWidget *>(m_view));
    if (toThumb)
        restartThumbnailSweep();
    // Carry the keyboard cursor across so arrow keys resume where they were.
    if (const QModelIndex cur = m_view->currentIndex(); cur.isValid())
        (toThumb ? static_cast<QAbstractItemView *>(iconView)
                 : static_cast<QAbstractItemView *>(m_view))
            ->setCurrentIndex(cur);
    (toThumb ? static_cast<QWidget *>(iconView) : static_cast<QWidget *>(m_view))->setFocus();
}

void FilePanel::setTreeSources(RemovableDeviceMonitor *devices, NetworkTreeRegistry *connections) {
    m_deviceMonitor = devices;
    m_connRegistry = connections;
    // Hot-plug and connect/disconnect both rebuild the top level. Both are
    // existing signals; nothing here polls.
    if (m_deviceMonitor)
        connect(m_deviceMonitor, &RemovableDeviceMonitor::devicesChanged, this,
                &FilePanel::markTreeRootsDirty);
    if (m_connRegistry)
        connect(m_connRegistry, &NetworkTreeRegistry::changed, this, &FilePanel::markTreeRootsDirty);
    markTreeRootsDirty();
}

void FilePanel::markTreeRootsDirty() {
    m_treeRootsDirty = true;
    if (m_dirTree && m_dirTree->isVisible())
        ensureTreeRootsReady();
}

void FilePanel::ensureTreeRootsReady() {
    if (m_treeRootsDirty)
        rebuildTreeRoots();
}

void FilePanel::rebuildTreeRoots() {
    if (!m_dirTreeModel)
        return;
    m_treeRootsDirty = false;

    QVector<RemovableDevice> devices;
    QStringList removableMounts;
    if (m_deviceMonitor) {
        devices = m_deviceMonitor->devices();
        for (const RemovableDevice &device : devices)
            if (device.isMounted && !device.mountPoint.isEmpty())
                removableMounts.append(device.mountPoint);
    }

    QVector<NetworkTreeEntry> networks;
    QHash<QString, std::shared_ptr<NetworkSession>> sessions;
    if (m_connRegistry) {
        for (const RegisteredConnection &conn : m_connRegistry->connections()) {
            auto live = conn.session.lock();
            if (!live)
                continue;
            NetworkTreeEntry entry;
            entry.connectionId = conn.connectionId;
            entry.label = conn.label;
            entry.scheme = conn.scheme;
            // Start at the topmost candidate and let the model walk down when the
            // server refuses a level (see TreeRoot::basePathFallbacks).
            const QStringList candidates = TreeRootBuilder::networkBaseCandidates(conn.basePath);
            entry.basePath = candidates.value(0, QStringLiteral("/"));
            entry.basePathFallbacks = candidates.mid(1);
            entry.ownedByThisPanel = (conn.owner == this);
            networks.append(entry);
            sessions.insert(conn.connectionId, live);
        }
    }

    const QVector<LocalVolume> volumes = TreeRootBuilder::enumerateLocalVolumes(removableMounts);
    const QVector<TreeRoot> roots = TreeRootBuilder::build(volumes, devices, networks);

    const bool showHidden = m_model->showHiddenFiles();
    m_dirTreeModel->setShowHidden(showHidden);
    m_dirTreeModel->setRoots(roots, [&](const TreeRoot &root) -> TreeDirLister * {
        if (root.kind == TreeRoot::Network) {
            const auto session = sessions.value(root.connectionId);
            if (!session)
                return nullptr;
            // The SAME session the tab's file list uses: it is a single-threaded
            // actor, so tree listings queue behind file-list ones instead of
            // racing them. That serialisation is what makes this safe for
            // libsmbclient, whose global state cannot survive concurrent use.
            auto *lister = new NetworkTreeLister(session);
            lister->setShowHidden(showHidden);
            return lister;
        }
        auto *lister = new LocalTreeLister;
        lister->setShowHidden(showHidden);
        return lister;
    });

    // A single local root stands for the whole filesystem, exactly as the tree
    // always looked: make it the view's root so its children (/bin, /home, ...)
    // are the top-level rows and no device header appears. With several roots
    // the device rows ARE the point, so the real top level is shown.
    if (roots.size() == 1 && roots.first().kind == TreeRoot::LocalFilesystem) {
        const QModelIndex rootIndex = m_dirTreeModel->index(0, 0);
        m_dirTree->setRootIndex(rootIndex);
        m_dirTree->expand(rootIndex); // populate the level now shown at the top
    } else {
        m_dirTree->setRootIndex(QModelIndex());
    }

    // The rebuild reset the model, so re-establish the selection.
    if (!m_model->isFlatMode())
        syncTreeToPath(m_model->rootPath());
}

void FilePanel::refreshThemeIcons() {
    // The computer button's glyph is recoloured from the palette when it is
    // built, so it has to be rebuilt when the palette changes -- unlike its
    // neighbours in the address row, which draw themselves and pick the colour
    // up automatically.
    if (m_computerButton)
        m_computerButton->setIcon(
            IconCache::instance().glyphIcon(QStringLiteral(":/icons/computer.svg")));
    if (m_dirTreeModel)
        m_dirTreeModel->refreshIcons();
    refreshTabIcons();
    if (m_view)
        m_view->viewport()->update();
    if (m_iconView)
        m_iconView->viewport()->update();
}

void FilePanel::syncTreeToPath(const QString &path) {
    if (!m_dirTree || !m_dirTree->isVisible() || path.isEmpty())
        return;
    m_treeTargetPath = path;
    m_treeTargetConnId = m_model->connectionId();
    advanceTreeSync();
}

void FilePanel::advanceTreeSync() {
    if (m_treeTargetPath.isEmpty() || !m_dirTree || !m_dirTree->isVisible())
        return;

    const QModelIndex exact = m_dirTreeModel->indexForPath(m_treeTargetConnId, m_treeTargetPath);
    if (exact.isValid()) {
        m_dirTree->setCurrentIndex(exact);
        m_dirTree->scrollTo(exact);
        m_treeTargetPath.clear();
        return;
    }

    // Not materialised yet: expand the deepest ancestor we do have and wait for
    // its children. Each arrival re-enters here one level deeper, so the walk
    // costs one listing per level and never blocks -- which is the only way this
    // can work over a network connection.
    const QModelIndex ancestor =
        m_dirTreeModel->deepestLoadedAncestor(m_treeTargetConnId, m_treeTargetPath);
    if (!ancestor.isValid()) {
        m_treeTargetPath.clear(); // no root covers this path (e.g. an archive)
        return;
    }
    if (m_dirTreeModel->canFetchMore(ancestor)) {
        m_dirTree->expand(ancestor); // triggers fetchMore; we resume on childrenLoaded
        return;
    }
    // The ancestor is fully loaded yet the target is still absent -- the
    // directory does not exist in the tree (hidden, or removed since). Settle on
    // the ancestor rather than looping.
    m_dirTree->expand(ancestor);
    m_dirTree->setCurrentIndex(ancestor);
    m_dirTree->scrollTo(ancestor);
    m_treeTargetPath.clear();
}

QString FilePanel::connectionIdOfTab(int index) const {
    auto tab = m_tabManager->tabAt(index);
    if (!tab || tab->connScheme.isEmpty() || tab->connLabel.isEmpty())
        return {};
    return tab->connScheme + QStringLiteral("://") + tab->connLabel;
}

int FilePanel::tabIndexForConnection(const QString &connectionId) const {
    if (connectionId.isEmpty())
        return -1;
    for (int i = 0; i < m_tabManager->count(); ++i)
        if (connectionIdOfTab(i) == connectionId)
            return i;
    return -1;
}

void FilePanel::activateTreeIndex(const QModelIndex &index) {
    if (!index.isValid() || !m_dirTreeModel->isActivatable(index))
        return;
    const QString path = m_dirTreeModel->pathAt(index);
    if (path.isEmpty())
        return;
    const QString connId = m_dirTreeModel->connectionIdAt(index);

    // A local node: a disk, a removable volume, or a folder under one. The tree
    // gives every non-network root an empty connection id, so this branch used
    // to hand a LOCAL path to whatever backend the tab happened to be on -- and
    // since every backend here is POSIX-rooted, clicking the local "/home" while
    // the tab was on a share listed the *server's* /home instead, with no hint
    // that it had. navigateTo() cannot catch it either: on a network tab it
    // skips the isDir() guard on purpose (see the comment there).
    //
    // So take the tab local first. openLocalInTab() parks the connection into
    // Back history rather than dropping it, which is what makes this safe to do
    // silently: one Back press returns to the server, tab label and all.
    if (connId.isEmpty()) {
        FileProvider *prov = m_model->provider();
        if (prov && !prov->isLocalFilesystem()) {
            openLocalInTab(-1, path); // -1 = this tab
            return;
        }
        navigateTo(path);
        return;
    }
    // A node on the connection this tab is already using: straight there.
    if (connId == m_model->connectionId()) {
        navigateTo(path);
        return;
    }
    // Another of THIS panel's tabs owns the connection: switch to that tab (which
    // swaps its parked connection back in) and navigate there.
    const int tabIndex = tabIndexForConnection(connId);
    if (tabIndex >= 0) {
        navigateTabTo(tabIndex, path);
        return;
    }
    // Belongs to the other panel. The model marks those nodes non-activatable,
    // so this is unreachable in practice; ignoring it is the right fallback --
    // adopting another panel's session would break per-tab connection ownership.
}

void FilePanel::activateCurrentEntry() {
    // Reuse the double-click/Enter path so directories, archives and files are
    // all handled through the active provider -- not QDesktopServices on a
    // fabricated local path, which silently no-ops for network providers.
    const QModelIndex idx = activeView()->currentIndex();
    if (idx.isValid())
        onActivated(idx);
}

void FilePanel::onActivated(const QModelIndex &index) {
    const FileInfo info = m_model->fileInfoAt(index.row());
    if (!info.isValid())
        return;

    // A computer-view row is a place, not a path: navigating to info.path()
    // would hand "computer://server/<uuid>" to the model as a directory. Report
    // it instead; the receiver knows how to mount a device and open a
    // connection, and calls leaveComputerView() before it navigates.
    if (m_computerProvider) {
        const ComputerEntry entry = m_computerProvider->entryFor(info.path());
        if (!entry.target.isEmpty())
            emit computerEntryActivated(this, entry);
        return;
    }

    // Exiting an archive: ".." at the archive root is the one entry whose path is
    // the directory the archive was entered from -- on disk, or on the server the
    // panel was browsing, in which case leaveArchive() puts that connection back
    // before we navigate.
    if (info.isParentEntry() && m_archiveProvider && info.path() == m_archiveExitDir) {
        const QString exitDir = m_archiveExitDir;
        // Select the archive we're stepping out of, so leaving then re-entering is
        // one keystroke (matches going up out of a normal sub-directory).
        const QString archiveFile = m_archiveSourcePath;
        leaveArchive();
        navigateTo(exitDir);
        selectPathAfterReload(archiveFile);
        return;
    }

    if (info.isParentEntry()) {
        // Going up via "..": remember the directory we're leaving so the parent
        // listing lands the cursor on it (same behaviour as the Backspace/navigateUp
        // path). info.path() is already the parent directory.
        const QString child = m_model->provider()
                                  ? m_model->provider()->cleanPath(m_model->rootPath())
                                  : m_model->rootPath();
        navigateTo(info.path());
        selectPathAfterReload(child);
        return;
    }
    if (info.isDir()) {
        navigateTo(info.path());
        return;
    }

    // Entering an archive: no nested-archive browse, so not while already inside
    // one. Read-only smart browse via ArchiveProvider. Disabled when the "archive
    // as folder" preference is off -- archives then open as plain files.
    if (m_archiveAsFolder && !m_archiveProvider) {
        const bool network = m_model->hasNetworkSession();
        // On a network tab only the suffix can decide: isArchivePath() also
        // sniffs magic bytes for extension-less AppImages, and it reads them
        // through the LOCAL filesystem, where a server path names nothing. (An
        // AppImage on a share therefore opens as a plain file, as before.)
        const bool isArchive = network ? ArchiveLayout::hasArchiveSuffix(info.path())
                                       : ArchiveProvider::isArchivePath(info.path());
        if (isArchive) {
            if (network) {
                // Nothing here can read the server's copy in place; ask for a
                // local one. The browse starts when it lands (or never, if the
                // user cancels the download).
                emit archiveDownloadRequested(this, info.path());
                return;
            }
            if (enterArchive(info.path(), info.path(), false))
                return;
            // Not a usable archive (or unreadable): fall through to opening it.
        }
    }

    emit openRequested(info.path());
}

bool FilePanel::enterArchive(const QString &localArchivePath, const QString &sourcePath,
                             bool ownsLocalCopy) {
    if (m_archiveProvider)
        return false; // no nested-archive browse yet
    QString error;
    auto provider = std::make_shared<ArchiveProvider>(localArchivePath, &error);
    if (!provider->isValid())
        return false;

    // Both of these have to be read through the CURRENT backend, before it is
    // swapped out below: on a network tab the archive's directory is the
    // server's answer to parentPath(), not this filesystem's.
    const QString exitDir = m_model->provider()->parentPath(sourcePath);
    // Record where we came from (a real dir or a flat search-results list) BEFORE
    // switching to the archive provider, so Back returns to it. currentLocation()
    // reports {} once m_archiveProvider is set, so the navigateTo("/") below
    // can't snapshot it -- we must push it here.
    const NavEntry from = currentLocation();

    provider->setExitPath(exitDir);
    provider->setOwnsArchiveFile(ownsLocalCopy);
    // Park the server connection rather than let setProvider() tear it down: the
    // archive is read off local disk, but ".." leads back onto the server and
    // has to find it still connected.
    m_archiveExitConn = m_model->detachConnection();
    m_archiveProvider = provider;
    m_archiveName = QFileInfo(sourcePath).fileName();
    m_archiveSourcePath = sourcePath;
    m_archiveExitDir = exitDir;
    m_model->setProvider(provider);
    if (from.isValid())
        pushHistory(from);
    navigateTo(QStringLiteral("/")); // archive virtual root (won't re-push)
    return true;
}

void FilePanel::leaveArchive() {
    if (!m_archiveProvider)
        return;
    m_archiveProvider.reset(); // last drop deletes a downloaded copy
    m_archiveName.clear();
    m_archiveSourcePath.clear();
    m_archiveExitDir.clear();
    // Re-adopt whatever was parked on the way in. An empty bundle means the
    // archive came off local disk, and attaching it is the same "back to the
    // local provider" that this used to do explicitly.
    m_model->attachConnection(std::move(m_archiveExitConn));
    m_archiveExitConn = FileSystemModel::NetworkConn();
    if (!m_model->hasNetworkSession()) {
        resetNetworkStatusFeedback();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
    }
}

void FilePanel::backOutOfArchive() {
    if (!m_archiveProvider)
        return;
    const QString exitDir = m_archiveExitDir;
    leaveArchive();
    m_model->setRootPath(exitDir);
    emit pathChanged(exitDir);
}

void FilePanel::showComputer(const QVector<ComputerEntry> &entries) {
    if (m_computerProvider) {
        // Already here: a device was plugged in or a host was discovered. Swap
        // the rows and re-list rather than re-entering, which would push another
        // history entry for a place the user never left.
        m_computerProvider->setEntries(entries);
        m_model->setRootPath(ComputerProvider::rootPath());
        return;
    }

    // An archive cannot travel here -- its provider would be replaced and the
    // connection it parked stranded -- so step out of it first, exactly as the
    // other backend-replacing paths do.
    backOutOfVirtualBackend();

    // Restoring a backgrounded tab is not a navigation: the entry for the place
    // the view was opened from is already on the stack from the first time.
    const NavEntry from = m_restoringComputerView ? NavEntry() : currentLocation();
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries(entries);
    // Park the server connection instead of letting setProvider() tear it down:
    // opening a drive from here has to leave the tab's connection intact so Back
    // returns to a live session rather than a dead path.
    m_computerExitConn = m_model->detachConnection();
    m_computerExitDir = m_model->rootPath();
    m_computerProvider = provider;
    m_model->setProvider(provider);
    if (from.isValid()) {
        pushHistory(from);
        m_forwardHistory.clear();
    }
    // Mark the tab so switching away and back returns here rather than to the
    // directory underneath, and label it for what it shows -- the path it
    // carries is the exit directory, which would otherwise name the tab after a
    // folder it is not displaying.
    if (auto tab = m_tabManager->activeTab()) {
        tab->computerView = true;
        tab->title = tr("Computer");
    }
    navigateTo(ComputerProvider::rootPath()); // won't re-push; currentLocation() is {} now
    updateActiveTabLabel();
}

void FilePanel::leaveComputerView() {
    if (!m_computerProvider)
        return;
    // Genuinely leaving: the tab should not come back here. The two paths that
    // merely park the view while the tab goes to the background re-set this
    // afterwards, because for them "leaving" is temporary.
    if (auto tab = m_tabManager->activeTab()) {
        tab->computerView = false;
        if (tab->title == tr("Computer"))
            tab->title.clear();
    }
    m_computerProvider.reset();
    m_computerExitDir.clear();
    m_model->attachConnection(std::move(m_computerExitConn));
    m_computerExitConn = FileSystemModel::NetworkConn();
    if (!m_model->hasNetworkSession()) {
        resetNetworkStatusFeedback();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
    }
}

void FilePanel::backOutOfComputerView() {
    if (!m_computerProvider)
        return;
    const QString exitDir = m_computerExitDir;
    leaveComputerView();
    m_model->setRootPath(exitDir);
    emit pathChanged(exitDir);
}

void FilePanel::leaveVirtualBackend() {
    // At most one of these is ever active -- entering either steps out of the
    // other first -- so the order is immaterial and both are no-ops otherwise.
    leaveArchive();
    leaveComputerView();
}

void FilePanel::backOutOfVirtualBackend() {
    backOutOfArchive();
    backOutOfComputerView();
}

void FilePanel::onAddressBarEntered(const QString &path) {
    // Leaving the computer view is navigateTo()'s job now. Doing it here as well
    // would be worse than redundant: it would hide the transition from
    // navigateTo, which would then snapshot the synthetic root as the place
    // being left and push it onto the history stack.
    navigateTo(path);
}

QString FilePanel::currentEntryPath() const {
    const QModelIndex idx = m_view->currentIndex();
    if (!idx.isValid())
        return {};
    return m_model->fileInfoAt(idx.row()).path();
}

bool FilePanel::currentEntryIsDir() const {
    const QModelIndex idx = m_view->currentIndex();
    return idx.isValid() && m_model->fileInfoAt(idx.row()).isDir();
}

qint64 FilePanel::currentEntrySize() const {
    const QModelIndex idx = m_view->currentIndex();
    return idx.isValid() ? m_model->fileInfoAt(idx.row()).size() : 0;
}

FileInfo FilePanel::currentEntryInfo() const {
    const QModelIndex idx = m_view->currentIndex();
    return idx.isValid() ? m_model->fileInfoAt(idx.row()) : FileInfo();
}

QStringList FilePanel::selectedPaths() const {
    // Nothing is selectable in the computer view: its rows are places, not
    // files. Every file operation -- copy, move, delete, compress, checksum --
    // starts here, and a synthetic "computer://drive/C:/" reaching one of them
    // is at best a confusing failure. Reporting an empty selection is both true
    // and what makes those operations do nothing, in one place instead of a
    // guard in each of them. Row-based work (counting a folder's size) goes
    // through selectedRowNumbers() and is unaffected.
    if (m_computerProvider)
        return {};
    QStringList paths;
    for (int row : selectedRowNumbers())
        paths.append(m_model->fileInfoAt(row).path());
    if (paths.isEmpty()) {
        const QString cur = currentEntryPath();
        if (!cur.isEmpty())
            paths.append(cur);
    }
    return paths;
}

QVector<FileInfo> FilePanel::selectedEntryInfos() const {
    if (m_computerProvider)
        return {}; // see selectedPaths()
    QVector<FileInfo> infos;
    for (int row : selectedRowNumbers())
        infos.append(m_model->fileInfoAt(row));
    // Same fall-back as selectedPaths(): with nothing ticked, the entry under
    // the cursor is what the user means.
    if (infos.isEmpty()) {
        const FileInfo cur = currentEntryInfo();
        if (cur.isValid() && !cur.isParentEntry())
            infos.append(cur);
    }
    return infos;
}

QString FilePanel::currentPreviewPath() {
    const QString entry = currentEntryPath();
    if (entry.isEmpty() || !isArchive())
        return entry; // local filesystem: the path is already real
    // Inside an archive: directories aren't previewable; files are extracted to
    // a temp file (name + extension preserved) so the viewers see a real path.
    if (m_model->provider()->isDir(entry))
        return {};
    auto *ap = static_cast<ArchiveProvider *>(m_archiveProvider.get());
    const QString real = ap->materialize(entry);
    prefetchArchiveNeighbors();
    return real.isEmpty() ? QString() : real;
}

void FilePanel::prefetchArchiveNeighbors() {
    if (!m_archiveProvider)
        return;
    const QModelIndex cur = m_view->currentIndex();
    if (!cur.isValid())
        return;
    const int row = cur.row();
    // Keep the provider alive for the duration of the async extraction even if
    // the user exits the archive in the meantime.
    auto prov = m_archiveProvider;
    for (int r : {row - 1, row + 1}) {
        if (r < 0 || r >= m_model->rowCount())
            continue;
        const FileInfo fi = m_model->fileInfoAt(r);
        if (!fi.isValid() || fi.isDir() || fi.isParentEntry())
            continue;
        const QString p = fi.path();
        QtConcurrent::run(
            [prov, p]() { static_cast<ArchiveProvider *>(prov.get())->materialize(p); });
    }
}

void FilePanel::setListFontFamily(const QString &family) {
    setListTypography(family, m_view->font().pointSize());
}

void FilePanel::applyChromeFont(const QFont &font) {
    auto apply = [&font](QWidget *widget) {
        if (!widget)
            return;
        if (widget->font() != font)
            widget->setFont(font);
        for (QWidget *child : widget->findChildren<QWidget *>()) {
            if (child->font() != font)
                child->setFont(font);
        }
    };
    apply(m_tabBar);
    apply(m_addTabButton);
    apply(m_addressBar);
    apply(m_treeButton);
    apply(m_backButton);
    apply(m_forwardButton);
    apply(m_starButton);
    apply(m_filterBar);
    apply(m_statusBar);
}

void FilePanel::setListTypography(const QString &family, int pt) {
    const QString effectiveFamily = family.isEmpty() ? QApplication::font().family() : family;
    pt = qBound(7, pt, 24);
    auto resolvedFont = [&effectiveFamily, pt](QWidget *widget) {
        QFont font = widget->font();
        font.setFamily(effectiveFamily);
        font.setPointSize(pt);
        return font;
    };
    auto applyFont = [](QWidget *widget, const QFont &font) {
        if (widget && widget->font() != font)
            widget->setFont(font);
    };

    const QFont listFont = resolvedFont(m_view);
    // Set the explicit viewport font first. The following parent change then
    // cannot propagate another FontChange into the inline-editor surface.
    applyFont(m_view->viewport(), listFont);
    applyFont(m_view, listFont);
    // The header is a QHeaderView child of m_view, but headers do not reliably
    // keep tracking a parent's font through later setFont() calls (observed:
    // it stayed frozen at whatever font it first resolved, following neither
    // the list nor the menu font size setting afterward) -- so it needs the
    // same explicit assignment as every other surface here rather than being
    // left to inherit.
    applyFont(m_view->horizontalHeader(), listFont);

    const QFontMetrics fm(listFont);
    const int iconPx = qBound(16, fm.height() + 4, 48);
    m_view->setIconSize(QSize(iconPx, iconPx));
    applyListRowHeight();

    if (m_iconView) {
        const QFont iconFont = resolvedFont(m_iconView);
        applyFont(m_iconView->viewport(), iconFont);
        applyFont(m_iconView, iconFont);
        applyThumbnailFontSize(pt);
    }

    if (m_dirTree) {
        const QFont treeFont = resolvedFont(m_dirTree);
        applyFont(m_dirTree->viewport(), treeFont);
        applyFont(m_dirTree, treeFont);
    }
}

void FilePanel::setTabBarVisible(bool visible) {
    if (m_tabBar)
        m_tabBar->setVisible(visible);
    if (m_addTabButton)
        m_addTabButton->setVisible(visible);
}

void FilePanel::setDirectoryTreeVisible(bool visible) {
    m_treeButton->setChecked(visible);
}

void FilePanel::setListFontSize(int pt) {
    setListTypography(m_view->font().family(), pt);
}

void FilePanel::applyThumbnailFontSize(int pt) {
    m_lastFontPt = pt;
    if (!m_iconView)
        return;
    // Grow the thumbnail cell with the font: icon scales from a 64px baseline at
    // 12pt; the grid reserves room for the icon plus a two-line elided label.
    // An explicit m_thumbIconSize (from the -/+ buttons) overrides the
    // font-derived size, so zooming persists independent of the font setting.
    const int iconPx = m_thumbIconSize > 0 ? m_thumbIconSize : qBound(48, 64 * pt / 12, 160);
    applyThumbnailIconSize(iconPx);
    if (m_thumbnailDelegate)
        m_thumbnailDelegate->setFontPointSize(pt); // label text always tracks the list font
}

void FilePanel::applyThumbnailIconSize(int iconPx) {
    if (!m_iconView)
        return;
    m_iconView->setIconSize(QSize(iconPx, iconPx));
    if (m_thumbnailDelegate) {
        m_thumbnailDelegate->setIconSize(iconPx);
        // The grid height MUST equal the delegate's painted cell height, or the
        // vertical layout step is shorter than the cell and the selection tile
        // (drawn on option.rect) overlaps the rows above/below and clips the
        // label. Width adds a little inter-column breathing room. cellSizeHint()
        // is the shared source of truth with the delegate's sizeHint().
        const QSize cell = m_thumbnailDelegate->cellSizeHint(m_iconView->font());
        m_iconView->setGridSize(QSize(cell.width() + 26, cell.height()));
        m_iconView->doItemsLayout();
    }
}

void FilePanel::applyListRowHeight() {
    const QFontMetrics fm(m_view->font());
    const int autoRowH = qMax(qMax(fm.height(), m_view->iconSize().height()), 16) + 6;
    const int rowH = m_listRowHeightOverride > 0 ? m_listRowHeightOverride : autoRowH;
    m_view->verticalHeader()->setDefaultSectionSize(rowH);
    m_view->horizontalHeader()->setFixedHeight(rowH);
}

void FilePanel::setThumbnailIconSize(int px) {
    m_thumbIconSize = px > 0 ? qBound(48, px, 192) : 0;
    applyThumbnailFontSize(m_lastFontPt); // reapply: override (or auto) at the current font size
}

void FilePanel::setListRowHeight(int h) {
    m_listRowHeightOverride = h > 0 ? qBound(16, h, 64) : 0;
    applyListRowHeight();
}

void FilePanel::zoomThumbnails(int deltaPx) {
    if (!m_iconView)
        return;
    const int current = m_thumbIconSize > 0 ? m_thumbIconSize : m_iconView->iconSize().width();
    m_thumbIconSize = qBound(48, current + deltaPx, 192);
    applyThumbnailIconSize(m_thumbIconSize);
    emit viewScaleChanged();
}

void FilePanel::zoomListRows(int deltaPx) {
    const QFontMetrics fm(m_view->font());
    const int autoRowH = qMax(qMax(fm.height(), m_view->iconSize().height()), 16) + 6;
    const int current = m_listRowHeightOverride > 0 ? m_listRowHeightOverride : autoRowH;
    m_listRowHeightOverride = qBound(16, current + deltaPx, 64);
    applyListRowHeight();
    emit viewScaleChanged();
}

void FilePanel::zoomViewOut() {
    if (isThumbnailMode())
        zoomThumbnails(-16);
    else
        zoomListRows(-4);
}

void FilePanel::zoomViewIn() {
    if (isThumbnailMode())
        zoomThumbnails(16);
    else
        zoomListRows(4);
}

void FilePanel::selectAll() {
    m_view->selectAll();
}

void FilePanel::deselectAll() {
    m_view->clearSelection();
}

void FilePanel::selectPathAfterReload(const QString &path) {
    if (!path.isEmpty() && !m_pendingSelection.contains(path))
        m_pendingSelection.append(path);
}

void FilePanel::settleAfterRemoval(const QStringList &paths) {
    FileProvider *prov = m_model->provider();
    // QFileInfo::exists() answers about THIS machine, so on a network or archive
    // tab it is answering about a different file than the one that was deleted.
    // It says "still there" for every path that has a local namesake -- so
    // nothing is dropped and the deleted rows sit there looking alive -- or
    // "gone" for every path that has none, which takes the whole selection out
    // of the listing, including the entries a partly-failed remote delete left
    // standing on the server. And since the source panel was then deliberately
    // NOT refreshed, that wrong answer was the last word.
    //
    // Asking the provider path by path would be a blocking round trip each, on
    // the GUI thread. One relist costs a single round trip, answers for every
    // entry at once, and is what the server actually says. The cursor returning
    // to the top is the price; being told files are gone when they are still
    // there is worse.
    if (!prov || !prov->isLocalFilesystem()) {
        refresh();
        return;
    }
    QStringList gone;
    for (const QString &path : paths)
        if (!QFileInfo::exists(path)) // keep any that survived a failed delete/move
            gone.append(path);
    // Nothing matched in the listing (the rows were already gone, or the panel
    // has since navigated): fall back to a full rescan, as before.
    if (!removeDeletedAndSelectNext(gone))
        refresh();
}

bool FilePanel::removeDeletedAndSelectNext(const QStringList &paths) {
    if (paths.isEmpty())
        return false;
    // Flat search-results listings have no directory to rescan -- refresh() is a
    // no-op there because the model's rootPath is empty. So splice the deleted
    // rows out of the model in place; removePaths() handles a flat listing the
    // same as a directory one. Keep the panel's own m_flatPaths mirror in sync
    // too, or a Back/Forward history snapshot would resurrect the deleted rows.
    const bool flat = m_model->isFlatMode();
    const int anchor = m_model->removePaths(paths);
    if (anchor < 0)
        return false; // nothing matched (e.g. the delete failed) -> caller refreshes
    if (flat)
        for (const QString &p : paths)
            m_flatPaths.removeAll(p);

    const int rows = m_model->rowCount();
    if (rows <= 0)
        return true; // directory emptied; nothing left to select

    // The removed block's old top row now holds the "next" file. If we deleted
    // the tail of the list, fall back to the new last row.
    int target = anchor >= rows ? rows - 1 : anchor;
    // Prefer a real entry over the ".." parent row when something else remains.
    if (m_model->isParentEntry(target) && rows > 1)
        target = 1;

    QAbstractItemView *view = isThumbnailMode() ? static_cast<QAbstractItemView *>(m_iconView)
                                                : static_cast<QAbstractItemView *>(m_view);
    const QModelIndex idx = m_model->index(target, 0);
    QItemSelectionModel *sel = view->selectionModel();
    sel->clearSelection();
    sel->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    sel->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
    view->scrollTo(idx, QAbstractItemView::EnsureVisible);
    updateStatus();
    return true;
}

void FilePanel::setActive(bool active) {
    m_view->setPanelActive(active);
}

void FilePanel::invertSelection() {
    QItemSelection full(m_model->index(0, 0),
                         m_model->index(m_model->rowCount() - 1, m_model->columnCount() - 1));
    m_view->selectionModel()->select(full, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

void FilePanel::toggleHiddenFiles() {
    // Re-scans the current directory; loadFinished refreshes the status line.
    m_model->setShowHiddenFiles(!m_model->showHiddenFiles());
}

void FilePanel::selectByPattern(bool select) {
    bool ok = false;
    const QString mask = ttc::getText(
        this, select ? tr("Select by Pattern") : tr("Unselect by Pattern"),
        tr("Wildcard mask (e.g. *.txt):"), QLineEdit::Normal, QStringLiteral("*"), &ok);
    if (!ok || mask.isEmpty())
        return;

    QItemSelectionModel *sel = m_view->selectionModel();
    const auto command = (select ? QItemSelectionModel::Select : QItemSelectionModel::Deselect) |
                         QItemSelectionModel::Rows;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->isParentEntry(row))
            continue;
        const FileInfo info = m_model->fileInfoAt(row);
        if (info.isValid() && FileSystemModel::matchesFilter(info.name(), mask))
            sel->select(m_model->index(row, 0), command);
    }
}

FilePanel::~FilePanel() {
    cancelDirectorySizeTask();
}

qreal FilePanel::focusProgress() const {
    return m_focusProgress;
}

QString FilePanel::currentPath() const {
    // The computer view has no working directory -- its rows are places, and
    // its root is an identifier, not a path. Roughly two dozen callers ask this
    // for somewhere to act: a new folder, a new file, a copy or extract
    // destination, a terminal's cwd, the command line's directory. Handing them
    // "computer://" is not merely useless, it is destructive-adjacent:
    // QDir("computer://").filePath("x") yields "computer:/x", and creating that
    // makes a real directory named "computer:" wherever the process happens to
    // be. So this reports the directory the view was opened from -- a real
    // place, the one the panel returns to on the way out, and the same answer a
    // backgrounded tab records.
    if (m_computerProvider && !m_computerExitDir.isEmpty())
        return m_computerExitDir;
    return m_model->rootPath();
}

QString FilePanel::connectionId() const {
    return m_model->connectionId();
}

FileSystemModel *FilePanel::model() const {
    return m_model;
}

void FilePanel::cancelDirectorySizeTask() {
    ++m_directorySizeRequestId;
    m_pendingDirectorySizeRequest.reset();
    if (m_model)
        m_model->clearDirectorySizeCalculating();
    if (!m_directorySizeTask)
        return;
    // Keep the task alive until its watcher reports completion. Its provider call
    // may still be blocked, and destroying the watcher would lose the lane wakeup.
    m_directorySizeTask->cancel();
}

void FilePanel::calculateDirSizes() {
    QStringList dirs;
    QHash<QString, qint64> symlinkRootSizes;
    QHash<QString, QString> rowPaths;
    auto addDirectory = [&](const FileInfo &info) {
        const QString target = measurableDirectoryFor(info);
        if (target.isEmpty())
            return;
        dirs << target;
        if (info.isSymLink())
            symlinkRootSizes.insert(target, info.size());
        if (target != info.path())
            rowPaths.insert(target, info.path());
    };
    for (int row : selectedRowNumbers()) {
        const FileInfo info = m_model->fileInfoAt(row);
        addDirectory(info);
    }
    if (dirs.isEmpty()) {
        // Nothing selected: fall back to the directory under the cursor.
        const QModelIndex cur = m_view->currentIndex();
        if (cur.isValid() && !m_model->isParentEntry(cur.row())) {
            const FileInfo info = m_model->fileInfoAt(cur.row());
            addDirectory(info);
        }
    }

    submitDirectorySizeRequest(std::move(dirs), std::move(symlinkRootSizes),
                               std::move(rowPaths));
}

// Total Commander's Space behaviour: pressing Space on a directory also counts
// its contents, so the size replaces "<DIR>" in the list. Only the directory
// under the cursor is counted -- unlike calculateDirSizes(), which takes the
// whole selection. See FileListView::rowSpaced for why this does not carry TC's
// "only when unselected" restriction.
QString FilePanel::measurableDirectoryFor(const FileInfo &info) const {
    if (!info.isValid() || !info.isDir())
        return {};
    if (!m_computerProvider)
        return info.path();
    const ComputerEntry entry = m_computerProvider->entryFor(info.path());
    switch (entry.kind) {
    case ComputerEntry::Kind::Drive:
    case ComputerEntry::Kind::UserFolder:
        return entry.target; // a real directory on this machine
    default:
        return {}; // a server, or a device with no mount point yet
    }
}

void FilePanel::calculateDirSizeForRow(int row) {
    if (!m_model || row < 0 || row >= m_model->rowCount() || m_model->isParentEntry(row))
        return;
    const FileInfo info = m_model->fileInfoAt(row);
    const QString target = measurableDirectoryFor(info);
    if (target.isEmpty())
        return;

    QStringList dirs{target};
    QHash<QString, qint64> symlinkRootSizes;
    if (info.isSymLink())
        symlinkRootSizes.insert(target, info.size());
    QHash<QString, QString> rowPaths;
    if (target != info.path())
        rowPaths.insert(target, info.path()); // file the result under the row
    submitDirectorySizeRequest(std::move(dirs), std::move(symlinkRootSizes),
                               std::move(rowPaths));
}

void FilePanel::submitDirectorySizeRequest(QStringList dirs,
                                           QHash<QString, qint64> symlinkRootSizes,
                                           QHash<QString, QString> rowPaths) {
    cancelDirectorySizeTask();
    m_directorySizeRowPaths = std::move(rowPaths);
    if (dirs.isEmpty())
        return;

    // Capture the provider (shared) so the task survives provider changes. A
    // new request generation makes every earlier progress/result callback stale.
    std::shared_ptr<FileProvider> provider = m_model->providerPtr();
    // The computer view's backend cannot walk a directory -- it only knows its
    // own rows -- and the paths above have already been translated to real ones,
    // so the walk goes to the local filesystem.
    if (m_computerProvider)
        provider = std::shared_ptr<FileProvider>(LocalFileProvider::instance(),
                                                 [](FileProvider *) {});
    const quint64 requestId = ++m_directorySizeRequestId;
    DirectorySizeRequest request{requestId, std::move(provider), std::move(dirs),
                                 std::move(symlinkRootSizes)};
    if (m_directorySizeTask) {
        // One running request owns the provider lane; repeated replacements only
        // replace this pending slot.
        m_pendingDirectorySizeRequest = std::move(request);
        return;
    }
    startDirectorySizeTask(std::move(request));
}

void FilePanel::startDirectorySizeTask(DirectorySizeRequest request) {
    const quint64 requestId = request.requestId;
    const QStringList dirs = request.directories;
    auto *task =
        new DirectorySizeTask(requestId, std::move(request.provider),
                              std::move(request.directories), this,
                              std::move(request.symlinkRootSizes));
    m_directorySizeTask = task;
    connect(task, &DirectorySizeTask::directorySizeReady, this,
            [this, task, requestId](const QString &path, qint64 bytes) {
                if (requestId != m_directorySizeRequestId || m_directorySizeTask != task)
                    return;
                // Filed under the row's own path: in the computer view the
                // directory that was walked and the row that displays the
                // answer are two different strings.
                m_model->setComputedDirSize(m_directorySizeRowPaths.value(path, path), bytes);
            });
    connect(task, &DirectorySizeTask::finished, this,
            [this, task](quint64, qint64, bool) {
                if (m_directorySizeTask != task) {
                    task->deleteLater();
                    return;
                }
                m_model->clearDirectorySizeCalculating();
                m_directorySizeTask = nullptr;
                task->deleteLater();
                if (!m_pendingDirectorySizeRequest)
                    return;
                DirectorySizeRequest pending =
                    std::move(*m_pendingDirectorySizeRequest);
                m_pendingDirectorySizeRequest.reset();
                if (pending.requestId == m_directorySizeRequestId)
                    startDirectorySizeTask(std::move(pending));
            });
    task->start();
    QTimer::singleShot(kDirectorySizeBusyDelayMs, this, [this, task, requestId, dirs] {
        if (requestId != m_directorySizeRequestId || m_directorySizeTask != task)
            return;
        for (const QString &dir : dirs)
            m_model->setDirectorySizeCalculating(m_directorySizeRowPaths.value(dir, dir), true);
    });
}

QString FilePanel::tabLabelFor(const QSharedPointer<TabState> &tab) const {
    // A search-results tab carries an explicit title (the search keyword) and no
    // single directory path, so honour it before the path-derived label.
    if (tab && !tab->title.isEmpty())
        return tab->title;
    if (!tab || tab->path.isEmpty())
        return tr("New Tab");
    // Directory-name label (unchanged local behaviour).
    QString label;
    const QString name = QFileInfo(tab->path).fileName();
    if (!name.isEmpty()) {
        label = name;
    } else if (!m_archiveName.isEmpty() &&
               tab == m_tabManager->tabAt(m_tabManager->activeIndex())) {
        // The archive virtual root is "/" (no file name): label the active tab
        // with the archive's own file name instead of a bare "/".
        label = m_archiveName;
    } else {
        label = tab->path; // real root "/"
    }
    // On a network tab, prefix the connection identity (user@host) so the tab
    // shows which host it is browsing, not just the directory. This is stored per
    // tab (stampActiveConnection) rather than read from the panel-wide provider,
    // so EVERY network tab shows its own host -- not only the active one -- and
    // the prefix survives a sibling tab switching to a local folder. Local tabs
    // have an empty connLabel and keep the plain directory label.
    if (tab && !tab->connLabel.isEmpty())
        return tab->connLabel + QStringLiteral(" : ") + label;
    return label;
}

void FilePanel::setConnectingLabel(const QString &label, const QString &scheme) {
    auto tab = m_tabManager->activeTab();
    if (!tab)
        return;
    tab->connScheme = scheme;
    tab->connLabel = label;
    const int idx = m_tabManager->activeIndex();
    m_tabBar->setTabText(idx, tabLabelFor(tab));
    refreshTabIcons();
    // A fresh connection resets this tab's navigation history: the transient
    // pre-connect local dir isn't a place Back should return to. Suppress the one
    // history push the imminent initial navigateTo(remote root) would make.
    m_backHistory.clear();
    m_forwardHistory.clear();
    m_suppressHistoryOnce = true;
    updateNavButtons();
    // Note: a later stampActiveConnection() while still connecting leaves connLabel
    // untouched (it only overwrites once isConnected()), so this prefix persists.
}

void FilePanel::stampActiveConnection() {
    // Snapshot the live backend's identity into the active tab so its icon/label
    // are the tab's own property from now on, independent of provider swaps.
    auto tab = m_tabManager->activeTab();
    if (!tab)
        return;
    if (FileProvider *provider = m_model->provider()) {
        const QString scheme = provider->scheme(); // cheap: inline constant, no lock
        tab->connScheme = scheme;
        if (scheme.isEmpty()) {
            tab->connLabel.clear();
        } else if (m_model->isConnected()) {
            // Only read displayName() once the link is up. A network provider
            // holds its mutex for the whole (possibly slow/stalled) connect, and
            // displayName() takes that same mutex -- calling it mid-connect would
            // freeze the GUI thread until the attempt times out. Until connected
            // we keep whatever prefix we had (empty on a brand-new tab).
            tab->connLabel = provider->displayName();
        }
    } else {
        tab->connScheme.clear();
        tab->connLabel.clear();
    }

    // Publish the connection to the shared registry so both panels' trees can
    // show a root for it. The label is snapshotted here rather than read by the
    // tree later: displayName() takes the provider's mutex, which a slow connect
    // holds for its whole duration, and the tree runs on the GUI thread.
    if (m_connRegistry && !tab->connScheme.isEmpty() && !tab->connLabel.isEmpty()
        && m_model->hasNetworkSession()) {
        const FileSystemModel::NetworkConn conn = m_model->peekConnection();
        if (conn.session) {
            RegisteredConnection entry;
            entry.connectionId = m_model->connectionId();
            entry.label = tab->connLabel;
            entry.scheme = tab->connScheme;
            entry.basePath = m_model->rootPath();
            entry.session = conn.session;
            entry.owner = this;
            if (!entry.connectionId.isEmpty())
                m_connRegistry->registerConnection(entry);
        }
    }
}

void FilePanel::updateActiveTabLabel() {
    stampActiveConnection();
    const int idx = m_tabManager->activeIndex();
    if (auto tab = m_tabManager->tabAt(idx))
        m_tabBar->setTabText(idx, tabLabelFor(tab));
    refreshTabIcons();
}

static QIcon iconForScheme(const QString &scheme) {
    QString resource;
    if (scheme == QLatin1String("sftp"))
        resource = QStringLiteral(":/icons/dev-sftp.svg");
    else if (scheme == QLatin1String("smb"))
        resource = QStringLiteral(":/icons/dev-smb.svg");
    else if (scheme == QLatin1String("ftp"))
        resource = QStringLiteral(":/icons/dev-ftp.svg");
    else if (scheme == QLatin1String("webdav"))
        resource = QStringLiteral(":/icons/dev-webdav.svg");
    // An unknown scheme has no glyph; the caller draws the status dot alone.
    return resource.isEmpty() ? QIcon() : IconCache::instance().glyphIcon(resource);
}

// The protocol icon with a small status dot overlaid so the tab shows connection
// health at a glance: amber while connecting/reconnecting, red on failure. No dot
// once connected (or for a local tab), keeping the plain protocol icon.
static QIcon iconForConn(const QString &scheme, int state, const QColor &statusColor = QColor()) {
    const QIcon base = iconForScheme(scheme);
    if (base.isNull())
        return base;
    QColor dot;
    if (statusColor.isValid())
        dot = statusColor;
    else if (state == NetworkSession::Failed)
        dot = QColor(0xe0, 0x4a, 0x4a); // red
    else if (state == NetworkSession::Connecting || state == NetworkSession::Reconnecting)
        dot = QColor(0xd0, 0x8a, 0x2a); // amber
    else
        return base; // connected / idle / local: no badge
    const int sz = 16;
    QPixmap pm = base.pixmap(sz, sz);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 1));
    p.setBrush(dot);
    const int d = 8;
    p.drawEllipse(sz - d, sz - d, d - 1, d - 1); // bottom-right corner
    p.end();
    return QIcon(pm);
}

void FilePanel::refreshTabIcons() {
    // Each tab renders its OWN stored protocol (connScheme), so a tab keeps its
    // icon even when another tab switches to a local folder (which changes the
    // panel-wide provider but not this tab's identity). A status dot is overlaid
    // per tab: the active tab reads the model's live session state; a parked tab
    // reads its own stored session's state.
    const int activeIdx = m_tabManager->activeIndex();
    const int n = qMin(m_tabBar->count(), m_tabManager->count());
    for (int i = 0; i < n; ++i) {
        auto tab = m_tabManager->tabAt(i);
        if (!tab) {
            m_tabBar->setTabIcon(i, QIcon());
            continue;
        }
        int state = -1;
        if (i == activeIdx)
            state = m_model->sessionState();
        else if (tab->session)
            state = static_cast<int>(tab->session->state());
        const bool isLoading = state == NetworkSession::Connecting ||
                               state == NetworkSession::Reconnecting;
        if (i == activeIdx && isLoading && !m_networkStatusVisible)
            m_tabBar->setTabIcon(i, iconForConn(tab->connScheme, NetworkSession::Connected));
        else
            m_tabBar->setTabIcon(i, iconForConn(
                tab->connScheme, state, i == activeIdx && isLoading ? m_networkStatusDotColor : QColor()));
    }
}

void FilePanel::syncTabBarFromManager() {
    pruneTabHistory(); // bulk closes (close-others/to-right/on-mount) land here
    m_tabBar->blockSignals(true);
    while (m_tabBar->count() > 0)
        m_tabBar->removeTab(0);
    for (int i = 0; i < m_tabManager->count(); ++i)
        m_tabBar->addTab(tabLabelFor(m_tabManager->tabAt(i)));
    const int active = m_tabManager->activeIndex();
    if (active >= 0 && active < m_tabBar->count())
        m_tabBar->setCurrentIndex(active);
    m_tabBar->blockSignals(false);
    refreshTabIcons();
    loadTabState(m_tabManager->activeIndex());
    m_tabBar->animateCurrentTabActivation();
}

void FilePanel::saveCurrentTabState() {
    auto tab = m_tabManager->activeTab();
    if (!tab)
        return;
    if (m_model->isFlatMode()) {
        // Preserve the flat search-results listing (and its keyword title) so
        // switching away and back restores it instead of a real directory.
        tab->flatPaths = m_flatPaths;
    } else if (m_computerProvider) {
        // Record where the computer view was entered from, never "computer://".
        // This state is read by the session snapshot taken at shutdown, and that
        // path is handed to the LOCAL provider on the next launch -- restoring
        // the synthetic root would leave the tab pointing at something no
        // backend can list, with no way back except typing a path. The flag
        // beside it is what brings the view itself back.
        tab->path = m_computerExitDir;
        tab->computerView = true;
        tab->flatPaths.clear();
    } else {
        tab->path = m_model->rootPath();
        tab->flatPaths.clear();
        tab->title.clear();
    }
    tab->selectedFiles = selectedPaths();
    // Stash this tab's back/forward stacks so they survive a tab switch instead
    // of being wiped (which used to strand a remote tab's Back on the server).
    m_tabHistory.insert(tab.data(), TabHistory{m_backHistory, m_forwardHistory});
}

void FilePanel::loadTabState(int index) {
    auto tab = m_tabManager->tabAt(index);
    if (!tab)
        return;
    // Restore this tab's own history (empty for a tab that never navigated).
    const TabHistory h = m_tabHistory.value(tab.data());
    m_backHistory = h.back;
    m_forwardHistory = h.forward;
    m_pendingSelection = tab->selectedFiles;
    updateNavButtons();
    if (tab->computerView) {
        // Rebuilt rather than restored: the rows are what was plugged in and
        // reachable, and asking again is both simpler than preserving them and
        // more correct -- a stick pulled out while the tab was in the background
        // should not still be listed. The owner holds the device monitor and the
        // host browser, so it does the assembling.
        m_flatPaths.clear();
        m_restoringComputerView = true;
        emit computerViewRequested(this);
        m_restoringComputerView = false;
        if (m_computerProvider) {
            m_computerExitDir = tab->path; // where leaving this tab returns to
            return;
        }
        // Nobody rebuilt it -- no owner connected yet, which is the normal state
        // during session restore, since the panel is filled before the signal
        // that assembles the rows is wired up. Fall through to the directory the
        // tab records, but KEEP the flag: it is the tab's intent, and clearing
        // it here would lose it precisely in the case where the owner is about
        // to arrive. Only a deliberate departure (leaveComputerView) clears it.
        if (tab->title == tr("Computer"))
            tab->title.clear();
    }
    if (!tab->flatPaths.isEmpty()) {
        m_flatPaths = tab->flatPaths;
        m_model->setFlatEntries(tab->flatPaths);
    } else if (!tab->path.isEmpty()) {
        m_flatPaths.clear();
        m_model->setRootPath(tab->path);
    }
}

void FilePanel::pruneTabHistory() {
    QSet<const TabState *> live;
    for (int i = 0; i < m_tabManager->count(); ++i)
        live.insert(m_tabManager->tabAt(i).data());
    for (auto it = m_tabHistory.begin(); it != m_tabHistory.end();) {
        if (!live.contains(it.key()))
            it = m_tabHistory.erase(it); // frees any parked sessions its entries held
        else
            ++it;
    }
}

void FilePanel::parkConnectionInto(const QSharedPointer<TabState> &tab) {
    if (!tab)
        return;
    // The tab is going into the background: its thumbnails are no longer being
    // painted, so stop fetching them (the session itself stays warm).
    cancelRemoteThumbnails();
    FileSystemModel::NetworkConn c = m_model->detachConnection();
    tab->provider = c.provider;
    tab->session = c.session;
    tab->authLabel = c.label;
    tab->authFactory = c.authRetry;
    // connScheme/connLabel were already stamped and stay put.
}

void FilePanel::adoptConnectionFrom(const QSharedPointer<TabState> &tab) {
    FileSystemModel::NetworkConn c;
    if (tab) {
        c.provider = tab->provider;
        c.session = tab->session;
        c.label = tab->authLabel;
        c.authRetry = tab->authFactory;
    }
    m_model->attachConnection(std::move(c));
    // A tab with no session is local: clear any lingering connection status.
    if (!m_model->hasNetworkSession()) {
        resetNetworkStatusFeedback();
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
    }
    // A parked session still pending credentials is re-offered its prompt by the
    // subsequent loadTabState() re-list (which re-runs the connect and re-emits
    // authRequired now that the tab is active again), so nothing extra is needed
    // here; a Failed one restores its status + retry link via attachConnection's
    // networkStateChanged emit.
}

void FilePanel::onTabBarCurrentChanged(int index) {
    if (index < 0)
        return;
    cancelDirectorySizeTask();
    // Clicking a tab also activates its panel: otherwise the click switched the
    // tab but left the OTHER panel active, so the next keyboard/F5 action ran on
    // the wrong pane. (The +/view/favorites paths already emit this.)
    emit panelActivated(this);
    // Step out of any archive first. Archive browsing is panel state, not tab
    // state, so it cannot travel with the tab going into the background: leaving
    // it in place would park an empty connection (the real one is held aside for
    // the archive's "..") and strand the tab on a backend it no longer owns.
    // Backing out to the directory the archive was entered from is what the tab
    // then saves and comes back to.
    // The outgoing tab is only being parked, so the computer view has to survive
    // the back-out below (which drops the backend and, with it, the flag).
    const bool parkedComputerView = m_computerProvider != nullptr;
    backOutOfVirtualBackend();
    const int prev = m_tabManager->activeIndex();
    saveCurrentTabState();
    if (parkedComputerView) {
        if (auto tab = m_tabManager->activeTab()) { // still the outgoing tab
            tab->computerView = true;
            tab->title = tr("Computer");
        }
    }
    if (prev != index && prev >= 0)
        parkConnectionInto(m_tabManager->tabAt(prev)); // keep the old tab's server alive
    m_tabManager->setActiveIndex(index);
    if (prev != index)
        adoptConnectionFrom(m_tabManager->tabAt(index)); // swap in this tab's server
    loadTabState(index);
    updateActiveTabLabel();
}

void FilePanel::openLocalInTab(int tabIndex, const QString &path) {
    cancelDirectorySizeTask();
    // Switch to the target tab (which swaps in its own connection) ...
    if (tabIndex >= 0 && tabIndex < m_tabBar->count() &&
        tabIndex != m_tabBar->currentIndex())
        m_tabBar->setCurrentIndex(tabIndex); // drives onTabBarCurrentChanged()

    // An archive has to be stepped out of before the snapshot below, or the
    // connection it parked would never be handed to the history entry and the
    // session would be stranded with nothing able to reach it.
    backOutOfVirtualBackend();

    // Record where we're leaving -- INCLUDING the live server connection -- into
    // back history so a Back press returns to the server (and its "user@host"
    // label), then leave it for the local favorite. currentLocation() captures
    // the connection via peekConnection(), so we snapshot BEFORE detaching.
    const NavEntry from = currentLocation();
    if (from.isValid()) {
        pushHistory(from);
        m_forwardHistory.clear();
    }

    // Park the connection: detach it from the model (it stays alive, co-owned by
    // the history entry above) rather than setProvider(nullptr), which would stop
    // the session and make Back unable to restore it. This tab is now local.
    cancelRemoteThumbnails();
    m_model->detachConnection();
    if (auto tab = m_tabManager->activeTab()) {
        tab->provider.reset();
        tab->session.reset();
        tab->connScheme.clear();
        tab->connLabel.clear();
        tab->authLabel.clear();
        tab->authFactory = nullptr;
        tab->connInfo = {}; // now a local tab: don't persist/reconnect it as remote
    }
    resetNetworkStatusFeedback();
    m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);

    // Navigate locally without re-pushing history (we already captured `from`).
    if (m_filterBar->isVisible()) {
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    m_flatPaths.clear();
    m_model->setRootPath(path);
    emit pathChanged(path);
    if (auto tab = m_tabManager->activeTab()) {
        tab->path = path;
        tab->flatPaths.clear();
        tab->title.clear();
        updateActiveTabLabel();
    }
    updateNavButtons();
}

void FilePanel::closeTabAt(int index) {
    if (m_tabManager->count() <= 1)
        return; // always keep at least one tab open
    cancelDirectorySizeTask();
    // Closing the active tab: drop its live connection from the model first, so
    // the model doesn't keep listing through a server whose tab is gone. (A
    // background tab's session is owned solely by its TabState and is torn down
    // when closeTab() drops that TabState.)
    // Drop the closing tab's connection from the tree registry. Expiry cannot be
    // relied on here: this tab's Back history still holds shared_ptrs to the
    // session, so it outlives the tab and its root would linger in the tree.
    if (m_connRegistry)
        m_connRegistry->unregisterConnection(connectionIdOfTab(index));
    // An archive belongs to the active tab, so closing that tab has to leave it:
    // otherwise the panel would go on reporting itself read-only over the tab
    // that slides into its place, and the connection parked for the archive's
    // ".." would survive with nothing able to reach it -- the hasNetworkSession()
    // test below cannot see it, because it is not the model's any more.
    if (index == m_tabManager->activeIndex())
        backOutOfVirtualBackend();
    if (index == m_tabManager->activeIndex() && m_model->hasNetworkSession()) {
        cancelRemoteThumbnails();
        m_model->detachConnection(); // returned bundle drops here -> session stops
    }
    m_tabHistory.remove(m_tabManager->tabAt(index).data());
    m_tabManager->closeTab(index);
    // Whatever tab is active now may carry its own parked connection: make it live.
    adoptConnectionFrom(m_tabManager->activeTab());
    syncTabBarFromManager();
}

int FilePanel::closeTabsOnMount(const QString &mountRoot) {
    if (mountRoot.isEmpty())
        return 0;

    const QString prefix = mountRoot + QLatin1Char('/');
    auto onDevice = [&](const QSharedPointer<TabState> &t) {
        // Only real-directory tabs live on a mount; flat search-result tabs are
        // virtual and path-independent.
        return t && t->flatPaths.isEmpty() &&
               (t->path == mountRoot || t->path.startsWith(prefix));
    };

    // Flush the live view into the active tab so a surviving active tab keeps
    // whatever the user last navigated to (not a stale saved path).
    saveCurrentTabState();

    int affected = 0;
    // Close from the back so indices stay valid; never drop below one tab.
    for (int i = m_tabManager->count() - 1; i >= 0; --i) {
        if (m_tabManager->count() <= 1)
            break;
        if (onDevice(m_tabManager->tabAt(i))) {
            m_tabHistory.remove(m_tabManager->tabAt(i).data());
            m_tabManager->closeTab(i);
            ++affected;
        }
    }

    // If the only remaining tab is still on the dead device, send it home
    // rather than leave it stranded on an unmounted path.
    if (m_tabManager->count() == 1 && onDevice(m_tabManager->tabAt(0))) {
        const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        auto t0 = m_tabManager->tabAt(0);
        t0->path = home;
        t0->flatPaths.clear();
        t0->title.clear();
        t0->selectedFiles.clear();
        ++affected;
    }

    if (affected > 0)
        syncTabBarFromManager();
    return affected;
}

void FilePanel::newTab() {
    cancelDirectorySizeTask();
    // This opens a tab rather than switching to one, so it does its own
    // park/save instead of going through onTabBarCurrentChanged -- and has to
    // step out of an archive for the same reason that does (see there).
    // Same parking as a tab switch: this tab stays on the computer view, the new
    // one starts on a directory.
    const bool parkedComputerView = m_computerProvider != nullptr;
    backOutOfVirtualBackend();
    saveCurrentTabState();
    if (parkedComputerView) {
        if (auto tab = m_tabManager->activeTab()) {
            tab->computerView = true;
            tab->title = tr("Computer");
        }
    }
    const bool wasNetwork = m_model->hasNetworkSession();
    // Park the outgoing tab's connection so its server stays alive in the
    // background while this new tab is active (also drops the model to local).
    parkConnectionInto(m_tabManager->activeTab());
    // A brand-new tab is a local tab: keep the current local directory, or go
    // home when we were on a server (its remote path is meaningless locally).
    const QString path = wasNetwork
        ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        : m_model->rootPath();
    const int idx = m_tabManager->addTab(path);
    m_tabBar->blockSignals(true);
    m_tabBar->addTab(tabLabelFor(m_tabManager->tabAt(idx)));
    m_tabBar->setCurrentIndex(idx);
    m_tabBar->blockSignals(false);
    m_tabManager->setActiveIndex(idx);
    loadTabState(idx);
    updateActiveTabLabel(); // stamp the new (local) tab: no icon, plain label
    m_tabBar->animateCurrentTabActivation();
}

void FilePanel::closeCurrentTab() {
    closeTabAt(m_tabBar->currentIndex());
}

void FilePanel::nextTab() {
    const int count = m_tabBar->count();
    if (count > 1)
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % count);
}

void FilePanel::prevTab() {
    const int count = m_tabBar->count();
    if (count > 1)
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() - 1 + count) % count);
}

QVector<FilePanel::RestoredTab> FilePanel::tabSnapshot() {
    saveCurrentTabState(); // flush the live view's path/selection into the active tab
    QVector<RestoredTab> result;
    for (int i = 0; i < m_tabManager->count(); ++i) {
        auto tab = m_tabManager->tabAt(i);
        result.append({tab->path, tab->selectedFiles, tab->computerView});
    }
    return result;
}

SavedConnection FilePanel::tabConnInfo(int index) const {
    if (auto tab = m_tabManager->tabAt(index))
        return tab->connInfo;
    return {};
}

void FilePanel::setActiveTabConnInfo(const SavedConnection &conn) {
    if (auto tab = m_tabManager->activeTab())
        tab->connInfo = conn;
}

void FilePanel::connectTabTo(int index, std::shared_ptr<FileProvider> provider,
                             std::function<bool(QString *)> connectFn, const QString &initialPath,
                             const QString &label, const SavedConnection &connInfo,
                             FileSystemModel::AuthRetryFactory authFactory) {
    if (index < 0 || index >= m_tabBar->count() || !provider)
        return;
    if (index != m_tabBar->currentIndex())
        m_tabBar->setCurrentIndex(index); // park/adopt via onTabBarCurrentChanged
    // A connection replaces the tab's backend wholesale, so any synthetic one
    // has to go first -- otherwise the panel would still believe it is showing
    // the computer view while the model lists a server, and the flag would drag
    // the view back on the next tab switch.
    backOutOfVirtualBackend();
    m_model->connectNetwork(provider, std::move(connectFn), initialPath);
    if (authFactory)
        m_model->setAuthContext(label, std::move(authFactory));
    setConnectingLabel(label, provider->scheme());
    if (auto tab = m_tabManager->activeTab())
        tab->connInfo = connInfo;
    navigateTo(initialPath); // sets tab->path and drives the (async) remote listing
}

void FilePanel::activateTab(int index) {
    if (index >= 0 && index < m_tabBar->count() && index != m_tabBar->currentIndex())
        m_tabBar->setCurrentIndex(index); // park/adopt via onTabBarCurrentChanged
}

bool FilePanel::tabHasConnection(int index) const {
    auto tab = m_tabManager->tabAt(index);
    return tab && (!tab->connScheme.isEmpty() || !tab->connInfo.host.isEmpty());
}

void FilePanel::disconnectTab(int index) {
    if (index < 0 || index >= m_tabManager->count())
        return;
    activateTab(index); // make it current so the model's active backend is this tab's
    // If that tab is inside an archive its connection is parked with the archive,
    // not in the model, and the detachConnection() below would find nothing to
    // drop -- leaving the session running after an explicit disconnect.
    backOutOfVirtualBackend();
    auto tab = m_tabManager->activeTab();
    // Take the connection out of the tree registry now. Its weak_ptr would expire
    // on its own once the session dies, but a deliberate disconnect should drop
    // the tree root immediately rather than at the next unrelated query.
    if (m_connRegistry)
        m_connRegistry->unregisterConnection(connectionIdOfTab(index));
    // Drop the live connection from the model (bundle dropped -> released) and then
    // release every remaining owner of the parked session so it actually stops.
    if (m_model->hasNetworkSession()) {
        cancelRemoteThumbnails();
        m_model->detachConnection();
    }
    if (tab) {
        tab->provider.reset();
        tab->session.reset(); // last owner gone -> ~NetworkSession stops its thread
        tab->connScheme.clear();
        tab->connLabel.clear();
        tab->authLabel.clear();
        tab->authFactory = nullptr;
        tab->connInfo = SavedConnection{};
        m_tabHistory.remove(tab.data()); // history entries also held the session
    }
    m_backHistory.clear();
    m_forwardHistory.clear();
    updateNavButtons();
    resetNetworkStatusFeedback();
    m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
    const int idx = m_tabManager->activeIndex();
    if (auto t = m_tabManager->activeTab())
        m_tabBar->setTabText(idx, tabLabelFor(t));
    refreshTabIcons();
    // Send the now-local tab home rather than leaving it on a dead remote path.
    navigateTo(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}

void FilePanel::restoreTabs(const QVector<RestoredTab> &tabs, int activeIndex) {
    if (tabs.isEmpty())
        return;

    // Reuse the single tab created in the constructor as tab 0 instead of
    // adding a duplicate empty one.
    auto tab0 = m_tabManager->tabAt(0);
    tab0->path = tabs.at(0).path;
    tab0->selectedFiles = tabs.at(0).selectedFiles;
    tab0->computerView = tabs.at(0).computerView;

    for (int i = 1; i < tabs.size(); ++i) {
        const int idx = m_tabManager->addTab(tabs.at(i).path);
        m_tabManager->tabAt(idx)->selectedFiles = tabs.at(i).selectedFiles;
        m_tabManager->tabAt(idx)->computerView = tabs.at(i).computerView;
    }

    const int clamped = qBound(0, activeIndex, m_tabManager->count() - 1);
    m_tabManager->setActiveIndex(clamped);
    syncTabBarFromManager(); // rebuilds the tab bar and loads the (now correct) active tab
}

bool FilePanel::activeTabWantsComputerView() const {
    // Read from the tab, not from isComputerView(): during a session restore
    // loadTabState has already run and fallen back to the directory, because
    // the owner had not yet connected the signal that assembles the rows. The
    // tab keeps the intent, so the owner can act on it once it can.
    auto tab = m_tabManager->activeTab();
    return tab && tab->computerView;
}
