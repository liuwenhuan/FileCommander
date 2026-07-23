#include "NotepadPanel.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
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
    // Qt::Popup: a top-level fly-out that closes as soon as focus leaves it.
    : QWidget(parent, Qt::Popup) {
    setObjectName(QStringLiteral("NotepadPanel"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground, true); // render the #objectName border

    // Toolbar: search + New / Delete.
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search notes..."));
    m_search->setClearButtonEnabled(true);
    auto *newButton = new QPushButton(tr("New"), this);
    m_deleteButton = new QPushButton(tr("Delete"), this);
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

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
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

void NotepadPanel::popUpAbove(const QRect &anchorGlobalRect, int topLimitGlobalY) {
    m_anchorRect = anchorGlobalRect;
    m_topLimitY = topLimitGlobalY;
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
    // Layout: top+bottom margin (6+6) + tool/splitter spacing (6) + splitter handle.
    const int chrome = 6 + 6 + 6 + m_splitter->handleWidth();

    // Ideal height shows all rows and the preferred editor.
    const int desired = toolH + chrome + listFull + kEditorPref;
    // Ceiling: from the button's top up to the app window's top.
    const int maxAvail = qMax(kEditorMin + 60, m_anchorRect.top() - m_topLimitY);
    const int h = qMin(desired, maxAvail);

    resize(kPanelWidth, h);

    // Split the space: show the whole list when it fits alongside a full-size
    // editor; otherwise cap the list so the editor keeps its minimum, and let
    // the list scroll for the overflow.
    const int content = h - toolH - chrome; // shared by list + editor
    int listShown, editorShown;
    if (listFull + kEditorPref <= content) {
        listShown = listFull;
        editorShown = content - listFull;
    } else {
        listShown = qBound(24, content - kEditorMin, listFull);
        editorShown = content - listShown;
    }
    m_splitter->setSizes({listShown, editorShown});

    int x = m_anchorRect.left();
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
