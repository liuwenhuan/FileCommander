#include "NotepadPanel.h"

#include "config/Settings.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLayout>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kAutoSaveDelayMs = 600; // debounce between the last keystroke and a save
constexpr int kPanelWidth = 420;
constexpr int kEditorPref = 200;      // preferred editing-area height
constexpr int kEditorMin = 120;       // editor never shrinks below this
} // namespace

NotepadPanel::NotepadPanel(QWidget *parent)
    : QWidget(parent, Qt::Popup),
      m_ownedSettings(std::make_unique<Settings>()),
      m_settings(*m_ownedSettings),
      m_store() {
    initialize();
}

NotepadPanel::NotepadPanel(Settings &settings, QWidget *parent)
    : QWidget(parent, Qt::Popup), m_settings(settings), m_store() {
    initialize();
}

NotepadPanel::NotepadPanel(Settings &settings, const QString &notepadDirectory, QWidget *parent)
    : QWidget(parent, Qt::Popup), m_settings(settings), m_store(notepadDirectory) {
    initialize();
}

void NotepadPanel::initialize() {
    setObjectName(QStringLiteral("NotepadPanel"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground, true); // render the #objectName border

    // Toolbar: search + New / Delete.
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search notes..."));
    m_search->setClearButtonEnabled(true);
    auto *newButton = new QPushButton(tr("New"), this);
    newButton->setObjectName(QStringLiteral("NotepadNewButton"));
    m_deleteButton = new QPushButton(tr("Delete"), this);
    m_deleteButton->setObjectName(QStringLiteral("NotepadDeleteButton"));
    newButton->setFocusPolicy(Qt::NoFocus);
    m_deleteButton->setFocusPolicy(Qt::NoFocus);

    auto *toolRow = new QHBoxLayout;
    toolRow->setContentsMargins(0, 0, 0, 0);
    toolRow->setSpacing(4);
    toolRow->addWidget(m_search, 1);
    toolRow->addWidget(newButton);
    toolRow->addWidget(m_deleteButton);

    // Split top (list) / bottom (editor) with a draggable divider.
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setObjectName(QStringLiteral("NotepadSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(4);

    m_list = new QListWidget(m_splitter);
    m_list->setObjectName(QStringLiteral("NotepadList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_editor = new QPlainTextEdit(m_splitter);
    m_editor->setObjectName(QStringLiteral("NotepadEditor"));
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setPlaceholderText(tr("Write your note here..."));

    m_splitter->addWidget(m_list);
    m_splitter->addWidget(m_editor);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);

    // Restore the user's last list/editor divider (persisted as the editor
    // height). A genuine drag emits splitterMoved -- our own setSizes() does
    // not -- so we can persist the new editor pane height straight from it.
    m_editorHeight = m_settings.notepadEditorHeight();
    connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int) {
        const QList<int> sizes = m_splitter->sizes();
        if (sizes.size() == 2 && sizes.at(1) > 0) {
            m_editorHeight = sizes.at(1);
            m_settings.setNotepadEditorHeight(m_editorHeight);
            // Preserve the user-selected editor height by growing or shrinking
            // the popup from its anchored bottom instead of stealing list space.
            applyDynamicSize();
        }
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    // The anchor and screen bounds are hard limits for a popup. Let its dynamic
    // geometry shrink below the layout's usual preferred minimum when required.
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->addLayout(toolRow);
    layout->addWidget(m_splitter, 1);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kAutoSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, &NotepadPanel::flushPendingSaves);

    // Two-step delete confirm reverts itself after a few seconds of no second
    // click (an inline confirm avoids a modal dialog, which would dismiss this
    // popup).
    m_deleteArmTimer = new QTimer(this);
    m_deleteArmTimer->setSingleShot(true);
    m_deleteArmTimer->setInterval(3000);
    connect(m_deleteArmTimer, &QTimer::timeout, this, &NotepadPanel::disarmDelete);

    connect(newButton, &QPushButton::clicked, this, &NotepadPanel::onNewNote);
    connect(m_deleteButton, &QPushButton::clicked, this, &NotepadPanel::onDeleteCurrent);
    connect(m_list, &QListWidget::currentRowChanged, this, &NotepadPanel::onCurrentRowChanged);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &NotepadPanel::onEditorChanged);
    connect(m_search, &QLineEdit::textChanged, this, &NotepadPanel::onSearchTextChanged);

    // Seed a first note so the editor is never bound to nothing.
    if (m_store.notes().isEmpty())
        m_store.create(tr("Note 1"));
    reloadList();
}

QString NotepadPanel::previewOf(const QString &body) {
    const QVector<QStringRef> lines = body.splitRef(QLatin1Char('\n'));
    for (const QStringRef &line : lines) {
        const QString t = line.trimmed().toString();
        if (!t.isEmpty())
            return t.left(60);
    }
    return QString();
}

QListWidgetItem *NotepadPanel::addRow(const NotepadNote &note) {
    // Prefer a live preview of the body; fall back to the stored title.
    QString label = previewOf(m_store.load(note.id));
    if (label.isEmpty())
        label = note.title.isEmpty() ? tr("New note") : note.title;
    auto *item = new QListWidgetItem(label, m_list);
    item->setData(Qt::UserRole, note.id);
    return item;
}

void NotepadPanel::reloadList(const QString &selectId) {
    const QString want = selectId.isEmpty() ? m_currentId : selectId;

    m_list->blockSignals(true);
    m_list->clear();
    int selectRow = -1;
    const QVector<NotepadNote> notes = m_store.notes();
    for (int i = 0; i < notes.size(); ++i) {
        addRow(notes.at(i));
        if (notes.at(i).id == want)
            selectRow = i;
    }
    if (selectRow < 0 && m_list->count() > 0)
        selectRow = 0;
    m_list->setCurrentRow(selectRow);
    m_list->blockSignals(false);

    // Load whatever ended up selected into the editor.
    onCurrentRowChanged();

    // Re-fit the popup to the new note count (grows/shrinks; no-op before the
    // popup has an anchor, i.e. during construction).
    applyDynamicSize();
}

QString NotepadPanel::currentId() const {
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void NotepadPanel::onCurrentRowChanged() {
    disarmDelete(); // a different note is now selected
    // Persist the note we're leaving before swapping in the new one.
    commitCurrentEditor();

    const QString id = currentId();
    m_currentId = id;

    m_loadingEditor = true;
    m_editor->setPlainText(id.isEmpty() ? QString() : m_store.load(id));
    m_editor->setEnabled(!id.isEmpty());
    m_loadingEditor = false;
    m_dirty = false;
}

void NotepadPanel::onNewNote() {
    commitCurrentEditor();
    const NotepadNote note = m_store.create(tr("Note %1").arg(m_store.notes().size() + 1));
    m_search->clear(); // ensure the new row isn't filtered out
    reloadList(note.id);
    m_editor->setFocus();
}

void NotepadPanel::onDeleteCurrent() {
    const QString id = m_currentId;
    if (id.isEmpty())
        return;

    // First click arms an inline confirm (button reads "Confirm?"); a second
    // click within a few seconds actually deletes. This keeps confirmation
    // inside the popup -- a modal dialog would steal focus and close it.
    if (!m_deleteArmed) {
        m_deleteArmed = true;
        m_deleteButton->setText(tr("Confirm?"));
        m_deleteArmTimer->start();
        return;
    }

    disarmDelete();
    m_dirty = false;
    m_currentId.clear();
    m_store.remove(id);

    // Never leave the notepad empty.
    if (m_store.notes().isEmpty())
        m_store.create(tr("Note 1"));
    reloadList();
}

void NotepadPanel::disarmDelete() {
    if (!m_deleteArmed)
        return;
    m_deleteArmed = false;
    m_deleteArmTimer->stop();
    if (m_deleteButton)
        m_deleteButton->setText(tr("Delete"));
}

void NotepadPanel::onEditorChanged() {
    if (m_loadingEditor)
        return;
    m_dirty = true;
    m_saveTimer->start(); // restart the debounce window
}

void NotepadPanel::commitCurrentEditor() {
    if (!m_dirty || m_currentId.isEmpty())
        return;
    const QString body = m_editor->toPlainText();
    m_store.save(m_currentId, body);
    // Keep the stored title in sync with the preview so the list stays useful
    // even before the body is reloaded.
    m_store.rename(m_currentId, previewOf(body));
    m_dirty = false;

    // Refresh the current row's preview text in place.
    if (QListWidgetItem *item = m_list->currentItem()) {
        QString label = previewOf(body);
        if (label.isEmpty())
            label = tr("New note");
        item->setText(label);
    }
}

void NotepadPanel::flushPendingSaves() {
    commitCurrentEditor();
}

void NotepadPanel::onSearchTextChanged(const QString &query) {
    // Filter the list to notes whose title or body matches; empty query shows
    // everything. Rows are hidden rather than removed so selection is preserved.
    if (query.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i)
            m_list->item(i)->setHidden(false);
        return;
    }

    const QVector<QString> hits = m_store.search(query);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        item->setHidden(!hits.contains(item->data(Qt::UserRole).toString()));
    }
}

void NotepadPanel::saveAll() {
    m_saveTimer->stop();
    commitCurrentEditor();
}

void NotepadPanel::closeEvent(QCloseEvent *event) {
    saveAll();
    QWidget::closeEvent(event);
}

void NotepadPanel::popUpAbove(const QRect &anchorGlobalRect, const QRect &appContentGlobalRect) {
    m_anchorRect = anchorGlobalRect;
    m_appContentRect = appContentGlobalRect;
    applyDynamicSize();
    show();
    raise();
    m_editor->setFocus();
}

void NotepadPanel::applyDynamicSize() {
    if (m_anchorRect.isNull())
        return; // no anchor yet (e.g. reloadList during construction)

    // Height is dynamic: grow to show every list row plus a comfortable editor.
    // The only cap is the app window's top -- the panel's top may reach it but
    // not pass it. When that cap forces the list to be shorter than its full
    // content, the list (and only the list) scrolls.
    int listFull = 2 * m_list->frameWidth();
    for (int i = 0; i < m_list->count(); ++i)
        listFull += m_list->sizeHintForRow(i);
    listFull = qMax(listFull, 24);

    const int toolH = m_search->sizeHint().height();
    const QMargins frameMargins = contentsMargins();
    // Layout: frameless-dialog vertical margins + panel's top/bottom margins
    // (6+6) + tool/splitter spacing (6) + splitter handle.
    const int chrome = frameMargins.top() + frameMargins.bottom() + 6 + 6 + 6 +
                       m_splitter->handleWidth();

    // Editor target: the user's persisted divider height if any, else the
    // preferred default. This is what the list is sized *around*, so the
    // divider the user dragged stays put as the popup's total height changes.
    const int editorTarget = qMax(kEditorMin, m_editorHeight > 0 ? m_editorHeight : kEditorPref);

    // Ideal height shows all rows and the target editor.
    const int desired = toolH + chrome + listFull + editorTarget;
    // Ceiling: the popup cannot cross either the app content's top or the
    // available screen's top. It always expands upward from the anchor.
    int topLimit = m_appContentRect.top();
    if (QScreen *scr = QGuiApplication::screenAt(m_anchorRect.center()))
        topLimit = qMax(topLimit, scr->availableGeometry().top());
    const int maxAvail = qMax(0, m_anchorRect.top() - topLimit);
    const int h = qMin(desired, maxAvail);

    resize(kPanelWidth, h);

    // Split the space: give the editor its target height and show the whole
    // list when both fit; otherwise cap the list (keeping the editor at least
    // its minimum) and let the list scroll for the overflow.
    const int content = qMax(0, h - toolH - chrome); // shared by list + editor
    int listShown = 0;
    int editorShown = content;
    if (content >= 24) {
        if (listFull + editorTarget <= content) {
            listShown = listFull;
            editorShown = content - listFull;
        } else {
            listShown = qBound(24, content - editorTarget, listFull);
            editorShown = content - listShown;
        }
    }
    m_splitter->setSizes({listShown, editorShown});

    // Right edge aligned to the app window's right edge; bottom just above the
    // button; top capped at the app window's visible top (enforced via maxAvail).
    int x = m_appContentRect.right() - width() + 1;
    int y = m_anchorRect.top() - height() + 1; // 1px overlap merges the edges

    if (QScreen *scr = QGuiApplication::screenAt(m_anchorRect.center())) {
        const QRect avail = scr->availableGeometry();
        if (x + width() > avail.right())
            x = avail.right() - width();
        if (x < avail.left())
            x = avail.left();
        if (y < avail.top())
            y = avail.top();
    }

    move(x, y);
}
