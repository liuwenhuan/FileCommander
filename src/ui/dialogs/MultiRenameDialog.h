#pragma once

#include <QDialog>
#include <QStringList>

class QTableWidget;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QLabel;
class QDialogButtonBox;

// Ctrl+M batch rename: search/replace + a [N]/[C]/[E] name mask + a
// counter, applied to the active panel's selection with a live preview
// before anything touches disk. Renames are applied synchronously (like
// F2's inline rename) rather than through OperationQueue -- they're
// near-instant and the dialog does its own duplicate-name/conflict
// checking up front, which OperationQueue's per-item conflict prompt
// isn't set up for.
class MultiRenameDialog : public QDialog {
    Q_OBJECT

public:
    explicit MultiRenameDialog(const QStringList &paths, QWidget *parent = nullptr);

private slots:
    void updatePreview();
    void apply();

private:
    QString buildNewName(const QString &originalBase, const QString &originalExt,
                          int counterValue) const;
    QString applySearchReplace(const QString &name) const;
    QString applyCase(const QString &name) const;
    bool validatePreview(); // returns true (and enables OK) if no conflicts

    QStringList m_sourcePaths;
    QTableWidget *m_table;
    QLineEdit *m_searchEdit;
    QLineEdit *m_replaceEdit;
    QCheckBox *m_regexCheck;
    QLineEdit *m_nameMaskEdit;
    QLineEdit *m_extMaskEdit;
    QSpinBox *m_counterStartSpin;
    QSpinBox *m_counterStepSpin;
    QSpinBox *m_counterDigitsSpin;
    QComboBox *m_caseCombo;
    QLabel *m_statusLabel;
    QDialogButtonBox *m_buttons;
};
