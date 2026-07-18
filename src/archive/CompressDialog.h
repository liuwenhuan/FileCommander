#pragma once

#include <QDialog>

class QLineEdit;
class QComboBox;

// Prompts for an archive name + format when compressing selected files.
class CompressDialog : public QDialog {
    Q_OBJECT

public:
    CompressDialog(const QString &destDir, const QString &defaultBaseName,
                    QWidget *parent = nullptr);

    QString archivePath() const;
    QString format() const;

private:
    QString m_destDir;
    QLineEdit *m_nameEdit;
    QComboBox *m_formatCombo;
};
