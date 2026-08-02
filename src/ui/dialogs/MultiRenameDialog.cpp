#include "MultiRenameDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>

#include "ThemedDialogs.h"
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "FileOperations.h"

MultiRenameDialog::MultiRenameDialog(const QStringList &paths, QWidget *parent)
    : FramelessDialog(parent), m_sourcePaths(paths) {
    setWindowTitle(tr("Multi-Rename Tool"));
    resize(700, 550);

    m_table = new QTableWidget(paths.size(), 2, this);
    m_table->setHorizontalHeaderLabels({tr("Original Name"), tr("New Name")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int i = 0; i < paths.size(); ++i) {
        auto *item = new QTableWidgetItem(QFileInfo(paths.at(i)).fileName());
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 0, item);
        m_table->setItem(i, 1, new QTableWidgetItem());
    }

    m_searchEdit = new QLineEdit(this);
    m_replaceEdit = new QLineEdit(this);
    m_regexCheck = new QCheckBox(tr("Regular expression"), this);

    auto *searchGroup = new QGroupBox(tr("Search && Replace (applied to the name, not extension)"),
                                       this);
    auto *searchLayout = new QFormLayout(searchGroup);
    searchLayout->addRow(tr("Search for:"), m_searchEdit);
    searchLayout->addRow(tr("Replace with:"), m_replaceEdit);
    searchLayout->addRow(QString(), m_regexCheck);

    m_nameMaskEdit = new QLineEdit(QStringLiteral("[N]"), this);
    m_extMaskEdit = new QLineEdit(QStringLiteral("[E]"), this);
    m_counterStartSpin = new QSpinBox(this);
    m_counterStartSpin->setRange(0, 999999);
    m_counterStartSpin->setValue(1);
    m_counterStepSpin = new QSpinBox(this);
    m_counterStepSpin->setRange(-1000, 1000);
    m_counterStepSpin->setValue(1);
    m_counterDigitsSpin = new QSpinBox(this);
    m_counterDigitsSpin->setRange(1, 8);
    m_counterDigitsSpin->setValue(1);
    m_caseCombo = new QComboBox(this);
    m_caseCombo->addItem(tr("Unchanged"));
    m_caseCombo->addItem(tr("UPPERCASE"));
    m_caseCombo->addItem(tr("lowercase"));
    m_caseCombo->addItem(tr("Title Case"));

    auto *maskGroup = new QGroupBox(tr("Name Mask (%1 = search/replace result, %2 = counter, "
                                        "%3 = original extension)")
                                         .arg("[N]", "[C]", "[E]"),
                                     this);
    auto *maskLayout = new QFormLayout(maskGroup);
    maskLayout->addRow(tr("Name:"), m_nameMaskEdit);
    maskLayout->addRow(tr("Extension:"), m_extMaskEdit);
    maskLayout->addRow(tr("Counter start:"), m_counterStartSpin);
    maskLayout->addRow(tr("Counter step:"), m_counterStepSpin);
    maskLayout->addRow(tr("Counter digits:"), m_counterDigitsSpin);
    maskLayout->addRow(tr("Case:"), m_caseCombo);

    for (auto *edit : {m_searchEdit, m_replaceEdit, m_nameMaskEdit, m_extMaskEdit})
        connect(edit, &QLineEdit::textChanged, this, &MultiRenameDialog::updatePreview);
    connect(m_regexCheck, &QCheckBox::toggled, this, &MultiRenameDialog::updatePreview);
    connect(m_caseCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MultiRenameDialog::updatePreview);
    for (auto *spin : {m_counterStartSpin, m_counterStepSpin, m_counterDigitsSpin})
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                &MultiRenameDialog::updatePreview);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setProperty("semanticState", QStringLiteral("error"));

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ttc::localizeStandardButtons(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &MultiRenameDialog::apply);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *optionsRow = new QHBoxLayout;
    optionsRow->addWidget(searchGroup, 1);
    optionsRow->addWidget(maskGroup, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(optionsRow);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_buttons);

    updatePreview();
}

QString MultiRenameDialog::applySearchReplace(const QString &name) const {
    if (m_searchEdit->text().isEmpty())
        return name;
    QString result = name;
    if (m_regexCheck->isChecked()) {
        QRegularExpression re(m_searchEdit->text());
        result.replace(re, m_replaceEdit->text());
    } else {
        result.replace(m_searchEdit->text(), m_replaceEdit->text());
    }
    return result;
}

QString MultiRenameDialog::applyCase(const QString &name) const {
    switch (m_caseCombo->currentIndex()) {
    case 1:
        return name.toUpper();
    case 2:
        return name.toLower();
    case 3: {
        QString result = name;
        bool startOfWord = true;
        for (int i = 0; i < result.size(); ++i) {
            if (result.at(i).isSpace()) {
                startOfWord = true;
            } else if (startOfWord) {
                result[i] = result.at(i).toUpper();
                startOfWord = false;
            } else {
                result[i] = result.at(i).toLower();
            }
        }
        return result;
    }
    default:
        return name;
    }
}

QString MultiRenameDialog::buildNewName(const QString &originalBase, const QString &originalExt,
                                         int counterValue) const {
    const QString processedName = applyCase(applySearchReplace(originalBase));
    const QString counterStr =
        QStringLiteral("%1").arg(counterValue, m_counterDigitsSpin->value(), 10, QChar('0'));

    auto substitute = [&](QString mask) {
        mask.replace(QStringLiteral("[N]"), processedName);
        mask.replace(QStringLiteral("[C]"), counterStr);
        mask.replace(QStringLiteral("[E]"), originalExt);
        return mask;
    };

    const QString newName = substitute(m_nameMaskEdit->text());
    const QString newExt = substitute(m_extMaskEdit->text());
    return newExt.isEmpty() ? newName : newName + QLatin1Char('.') + newExt;
}

void MultiRenameDialog::updatePreview() {
    int counter = m_counterStartSpin->value();
    for (int i = 0; i < m_sourcePaths.size(); ++i) {
        QFileInfo info(m_sourcePaths.at(i));
        const QString base = info.isDir() ? info.fileName() : info.completeBaseName();
        const QString ext = info.isDir() ? QString() : info.suffix();
        const QString newName = buildNewName(base, ext, counter);
        m_table->item(i, 1)->setText(newName);
        counter += m_counterStepSpin->value();
    }
    validatePreview();
}

bool MultiRenameDialog::validatePreview() {
    QSet<QString> seenNames;
    QStringList problems;

    for (int i = 0; i < m_table->rowCount(); ++i) {
        const QString newName = m_table->item(i, 1)->text();
        QFileInfo srcInfo(m_sourcePaths.at(i));
        const QString destDir = srcInfo.absolutePath();

        if (newName.isEmpty()) {
            problems.append(tr("Row %1: name is empty").arg(i + 1));
            continue;
        }
        if (seenNames.contains(newName)) {
            problems.append(tr("Duplicate result name: %1").arg(newName));
        }
        seenNames.insert(newName);

        if (newName != srcInfo.fileName() &&
            QFileInfo::exists(QDir(destDir).filePath(newName))) {
            problems.append(tr("%1 already exists on disk").arg(newName));
        }
    }

    if (problems.isEmpty()) {
        m_statusLabel->clear();
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        return true;
    }
    m_statusLabel->setText(problems.first());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    return false;
}

void MultiRenameDialog::apply() {
    if (!validatePreview())
        return;

    FileOperations ops;
    QStringList failures;
    for (int i = 0; i < m_sourcePaths.size(); ++i) {
        const QString &path = m_sourcePaths.at(i);
        const QString newName = m_table->item(i, 1)->text();
        if (newName == QFileInfo(path).fileName())
            continue; // unchanged, nothing to do
        QString err;
        if (!ops.renamePath(path, newName, &err))
            failures.append(err.isEmpty() ? path : err);
    }

    if (!failures.isEmpty())
        ttc::warning(this, tr("Multi-Rename"),
                              tr("Some files could not be renamed:\n%1").arg(failures.join('\n')));
    accept();
}
