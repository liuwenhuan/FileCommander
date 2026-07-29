#pragma once

#include "FramelessDialog.h"

class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;

// Prompts for an archive name + format when compressing selected files.
// Passphrase, header encryption, and compression level are offered for
// supported formats only.
class CompressDialog : public FramelessDialog {
    Q_OBJECT

public:
    CompressDialog(const QString &destDir, const QString &defaultBaseName,
                    QWidget *parent = nullptr);

    QString archivePath() const;
    QString format() const;

    // Emptied string for unsupported formats.
    QString passphrase() const;
    bool encryptHeaders() const;

    // The requested compression level, 0–9, where 0 means "store only" and 9
    // means "maximum compression". Meaningless when the selected format does
    // not support compression tuning.
    int compressionLevel() const;

private slots:
    void onFormatChanged(int index);

private:
    QString m_destDir;
    QLineEdit *m_nameEdit;
    QComboBox *m_formatCombo;

    QWidget *m_passwordOptions = nullptr;
    QWidget *m_levelOptions = nullptr;
    QLineEdit *m_passphraseEdit = nullptr;
    QCheckBox *m_encryptHeadersCheck = nullptr;
    QLineEdit *m_levelEdit = nullptr;
    QLabel *m_levelLabel = nullptr;
};
