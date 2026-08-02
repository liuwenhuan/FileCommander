#pragma once

#include "FramelessDialog.h"

#include <QStringList>

class QEvent;
class QPlainTextEdit;

class SecureWipeConfirmationDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit SecureWipeConfirmationDialog(const QStringList &paths, qint64 totalBytes,
                                          QWidget *parent = nullptr);

    static bool ask(QWidget *parent, const QStringList &paths, qint64 totalBytes);

protected:
    void changeEvent(QEvent *event) override;

private:
    void updatePathListHeight();

    QPlainTextEdit *m_pathList = nullptr;
};
