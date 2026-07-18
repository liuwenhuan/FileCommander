#include "FilePanel.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QShortcut>
#include <QVBoxLayout>

#include "FileListView.h"

FilePanel::FilePanel(QWidget *parent) : QWidget(parent) {
    m_model = new FileSystemModel(this);
    m_view = new FileListView(this);
    m_view->setModel(m_model);
    m_view->installEventFilter(this);

    m_addressBar = new QLineEdit(this);
    m_addressBar->setFocusPolicy(Qt::ClickFocus); // keep it out of the Tab chain

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_addressBar);
    layout->addWidget(m_view, 1);

    connect(m_view, &QAbstractItemView::activated, this, &FilePanel::onActivated);
    connect(m_addressBar, &QLineEdit::returnPressed, this, &FilePanel::onAddressBarEntered);
    connect(m_model, &FileSystemModel::loadFinished, this, [this](int) {
        m_addressBar->setText(m_model->rootPath());
        if (m_view->model()->rowCount() > 0)
            m_view->setCurrentIndex(m_view->model()->index(0, 0));
    });

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

    auto *selectPlusShortcut = new QShortcut(QKeySequence(Qt::Key_Plus), this);
    selectPlusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectPlusShortcut, &QShortcut::activated, this, &FilePanel::selectAll);

    auto *selectMinusShortcut = new QShortcut(QKeySequence(Qt::Key_Minus), this);
    selectMinusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectMinusShortcut, &QShortcut::activated, this, &FilePanel::deselectAll);
}

bool FilePanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_view && event->type() == QEvent::FocusIn)
        emit panelActivated(this);
    return QWidget::eventFilter(watched, event);
}

void FilePanel::navigateTo(const QString &path) {
    const QString cleaned = QDir(path).absolutePath();
    if (!QFileInfo(cleaned).isDir())
        return;
    if (!m_model->rootPath().isEmpty())
        pushHistory(m_model->rootPath());
    m_forwardHistory.clear();
    m_model->setRootPath(cleaned);
    emit pathChanged(cleaned);
}

void FilePanel::pushHistory(const QString &fromPath) {
    m_backHistory.append(fromPath);
}

void FilePanel::navigateUp() {
    QDir dir(m_model->rootPath());
    if (dir.cdUp())
        navigateTo(dir.absolutePath());
}

void FilePanel::goBack() {
    if (m_backHistory.isEmpty())
        return;
    m_forwardHistory.append(m_model->rootPath());
    const QString path = m_backHistory.takeLast();
    m_model->setRootPath(path);
    emit pathChanged(path);
}

void FilePanel::goForward() {
    if (m_forwardHistory.isEmpty())
        return;
    m_backHistory.append(m_model->rootPath());
    const QString path = m_forwardHistory.takeLast();
    m_model->setRootPath(path);
    emit pathChanged(path);
}

void FilePanel::refresh() {
    if (!m_model->rootPath().isEmpty())
        m_model->setRootPath(m_model->rootPath());
}

void FilePanel::onActivated(const QModelIndex &index) {
    const FileInfo info = m_model->fileInfoAt(index.row());
    if (!info.isValid())
        return;
    if (info.isDir() || info.isParentEntry())
        navigateTo(info.path());
    else
        emit openRequested(info.path());
}

void FilePanel::onAddressBarEntered() {
    navigateTo(m_addressBar->text());
}

QString FilePanel::currentEntryPath() const {
    const QModelIndex idx = m_view->currentIndex();
    if (!idx.isValid())
        return {};
    return m_model->fileInfoAt(idx.row()).path();
}

QStringList FilePanel::selectedPaths() const {
    QStringList paths;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &idx : rows) {
        if (m_model->isParentEntry(idx.row()))
            continue;
        paths.append(m_model->fileInfoAt(idx.row()).path());
    }
    if (paths.isEmpty()) {
        const QString cur = currentEntryPath();
        if (!cur.isEmpty())
            paths.append(cur);
    }
    return paths;
}

void FilePanel::selectAll() {
    m_view->selectAll();
}

void FilePanel::deselectAll() {
    m_view->clearSelection();
}

void FilePanel::invertSelection() {
    QItemSelection full(m_model->index(0, 0),
                         m_model->index(m_model->rowCount() - 1, m_model->columnCount() - 1));
    m_view->selectionModel()->select(full, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}
