#include "CompareDialog.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScreen>
#include <QSizePolicy>

#include "ThemedDialogs.h"
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextStream>
#include <QVBoxLayout>

#include "TextDiff.h"

namespace {
class ElidingPathLabel final : public QLabel {
public:
    ElidingPathLabel(const QString &fullPath, QWidget *parent)
        : QLabel(parent), m_fullPath(fullPath) {
        setToolTip(fullPath);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        updateElision();
    }

    QSize sizeHint() const override {
        return {0, fontMetrics().height()};
    }

    QSize minimumSizeHint() const override {
        return {0, fontMetrics().height()};
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        updateElision();
    }

private:
    void updateElision() {
        QLabel::setText(fontMetrics().elidedText(m_fullPath, Qt::ElideMiddle, width()));
    }

    QString m_fullPath;
};

QPlainTextEdit *makeReadOnlyEditor(QWidget *parent) {
    auto *edit = new QPlainTextEdit(parent);
    edit->setReadOnly(true);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont font(QStringLiteral("monospace"));
    edit->setFont(font);
    return edit;
}

void colorLine(QPlainTextEdit *edit, int lineIndex, const QColor &color) {
    QTextBlock block = edit->document()->findBlockByNumber(lineIndex);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    cursor.select(QTextCursor::BlockUnderCursor);
    QTextBlockFormat fmt = block.blockFormat();
    fmt.setBackground(color);
    cursor.setBlockFormat(fmt);
}
} // namespace

CompareDialog::CompareDialog(const QString &leftPath, const QString &rightPath, QWidget *parent)
    : CompareDialog(leftPath, rightPath, leftPath, rightPath, parent) {}

CompareDialog::CompareDialog(const QString &leftPath, const QString &rightPath,
                             const QString &leftLabel, const QString &rightLabel, QWidget *parent)
    : FramelessDialog(parent) {
    setWindowTitle(tr("Compare: %1 vs %2")
                        .arg(QFileInfo(leftLabel).fileName(), QFileInfo(rightLabel).fileName()));
    const QSize availableSize = screen()->availableGeometry().size();
    setMaximumWidth(availableSize.width());
    resize(QSize(1100, 700).boundedTo(availableSize));

    m_leftEdit = makeReadOnlyEditor(this);
    m_rightEdit = makeReadOnlyEditor(this);
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setMinimumWidth(0);
    m_summaryLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *header = new QHBoxLayout;
    header->addWidget(new ElidingPathLabel(leftLabel, this), 1);
    header->addWidget(new ElidingPathLabel(rightLabel, this), 1);

    auto *editors = new QHBoxLayout;
    editors->addWidget(m_leftEdit);
    editors->addWidget(m_rightEdit);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addLayout(editors, 1);
    layout->addWidget(m_summaryLabel);

    connect(m_leftEdit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (m_syncing)
            return;
        m_syncing = true;
        m_rightEdit->verticalScrollBar()->setValue(value);
        m_syncing = false;
    });
    connect(m_rightEdit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (m_syncing)
            return;
        m_syncing = true;
        m_leftEdit->verticalScrollBar()->setValue(value);
        m_syncing = false;
    });

    // Reading both files and diffing them is bounded (2 MB, 20k lines) but not
    // cheap -- TextDiff is quadratic in the worst case -- and it ran here, on
    // the GUI thread, before the dialog had painted once. Off-thread, so the
    // window comes up and fills in.
    auto *watcher = new QFutureWatcher<CompareResult>(this);
    connect(watcher, &QFutureWatcher<CompareResult>::finished, this, [this, watcher]() {
        const CompareResult result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty()) {
            ttc::warning(this, tr("Compare"), result.error);
            return;
        }
        applyComparison(result);
    });
    watcher->setFuture(QtConcurrent::run(&CompareDialog::compareFiles, leftPath, rightPath));
}

// Worker half: everything that only needs the two paths. No widget is touched
// here, which is what makes it safe to run off the GUI thread.
CompareDialog::CompareResult CompareDialog::compareFiles(const QString &leftPath,
                                                          const QString &rightPath) {
    CompareResult result;
    for (const QString &path : {leftPath, rightPath}) {
        if (QFileInfo(path).size() > kMaxCompareBytes) {
            result.error = tr("%1 is too large to compare (over 2 MB).")
                               .arg(QFileInfo(path).fileName());
            return result;
        }
    }

    auto readLines = [](const QString &path) -> QStringList {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        QTextStream stream(&file);
        return stream.readAll().split(QLatin1Char('\n'));
    };

    const QStringList leftLines = readLines(leftPath);
    const QStringList rightLines = readLines(rightPath);
    if (leftLines.size() > kMaxCompareLines || rightLines.size() > kMaxCompareLines) {
        result.error = tr("Files are too long to compare (over %1 lines).")
                           .arg(kMaxCompareLines);
        return result;
    }

    result.diff = TextDiff::compare(leftLines, rightLines);
    return result;
}

// GUI half: takes the worker's answer and fills the two editors.
void CompareDialog::applyComparison(const CompareResult &result) {
    const QVector<DiffLine> &diff = result.diff;
    QStringList leftDisplay, rightDisplay;
    QVector<int> leftColored, rightColored;
    leftDisplay.reserve(diff.size());
    rightDisplay.reserve(diff.size());

    int addedCount = 0, removedCount = 0;
    for (int i = 0; i < diff.size(); ++i) {
        const DiffLine &line = diff.at(i);
        switch (line.kind) {
        case DiffLine::Kind::Same:
            leftDisplay << line.leftText;
            rightDisplay << line.rightText;
            break;
        case DiffLine::Kind::Removed:
            leftDisplay << line.leftText;
            rightDisplay << QString();
            leftColored << i;
            ++removedCount;
            break;
        case DiffLine::Kind::Added:
            leftDisplay << QString();
            rightDisplay << line.rightText;
            rightColored << i;
            ++addedCount;
            break;
        }
    }

    m_leftEdit->setPlainText(leftDisplay.join(QLatin1Char('\n')));
    m_rightEdit->setPlainText(rightDisplay.join(QLatin1Char('\n')));

    const QColor removedColor(255, 210, 210);
    const QColor addedColor(210, 255, 210);
    const QColor gapColor(235, 235, 235);

    for (int i : leftColored)
        colorLine(m_leftEdit, i, removedColor);
    for (int i : rightColored)
        colorLine(m_rightEdit, i, addedColor);
    // Gaps: the line is blank on one side because the other side added or
    // removed content there -- shade it so it reads as "nothing here",
    // distinct from an actual blank line in the original file.
    for (int i = 0; i < diff.size(); ++i) {
        if (diff.at(i).kind == DiffLine::Kind::Removed)
            colorLine(m_rightEdit, i, gapColor);
        else if (diff.at(i).kind == DiffLine::Kind::Added)
            colorLine(m_leftEdit, i, gapColor);
    }

    if (addedCount == 0 && removedCount == 0)
        m_summaryLabel->setText(tr("Files are identical"));
    else
        m_summaryLabel->setText(
            tr("%1 line(s) only in left, %2 line(s) only in right").arg(removedCount).arg(addedCount));

    m_summaryLabel->setToolTip(m_summaryLabel->text());
}
