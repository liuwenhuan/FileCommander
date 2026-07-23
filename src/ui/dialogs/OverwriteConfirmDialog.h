#pragma once

#include "FramelessDialog.h"

#include "FileOpTypes.h"

// Modal "file already exists" prompt with the standard TC-style choices.
// Use the static ask() helper -- it's what OperationQueue's conflict
// handler is wired to.
class OverwriteConfirmDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit OverwriteConfirmDialog(const QString &source, const QString &destination,
                                     QWidget *parent = nullptr);

    static ErrorAction ask(QWidget *parent, const QString &source, const QString &destination);

private:
    ErrorAction m_result = ErrorAction::Cancel;
};
