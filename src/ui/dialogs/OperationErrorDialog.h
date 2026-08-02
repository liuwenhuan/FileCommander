#pragma once

#include "FramelessDialog.h"
#include "FileOpTypes.h"

#include <QList>

class QVBoxLayout;
class QPushButton;
class QWidget;
class QEvent;
class QResizeEvent;
class QShowEvent;

class OperationErrorDialog final : public FramelessDialog {
public:
    static ErrorAction ask(QWidget *parent, const OperationError &error,
                           bool elevationAvailable);

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    OperationErrorDialog(const OperationError &error, bool elevationAvailable, QWidget *parent);

    void addAction(const QString &text, ErrorAction action, const QString &objectName);
    void updateActionLayout();

    QWidget *m_actionPanel = nullptr;
    QVBoxLayout *m_actionLayout = nullptr;
    QList<QPushButton *> m_actionButtons;
    ErrorAction m_result = ErrorAction::Abort;
};
