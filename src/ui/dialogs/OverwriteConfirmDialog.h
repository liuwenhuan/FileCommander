#pragma once

#include "FramelessDialog.h"

#include "FileOpTypes.h"

class QShowEvent;

// Modal "file already exists" prompt with the standard TC-style choices.
// Use the static ask() helper -- it's what OperationQueue's conflict
// handler is wired to.
//
// Takes a FileConflict rather than two paths because the sizes it shows cannot
// be looked up here: on a network transfer both files are on a server, and the
// QFileInfo this used to build described a same-named LOCAL file or nothing at
// all -- so the prompt read "(0 bytes)" on both sides and the user decided
// whether to overwrite on the strength of two invented numbers.
class OverwriteConfirmDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit OverwriteConfirmDialog(const FileConflict &conflict, QWidget *parent = nullptr,
                                    bool allowRename = true);

    static ErrorAction ask(QWidget *parent, const FileConflict &conflict,
                           bool allowRename = true);

    // The prompt's message, as a pure function of the conflict, so what the user
    // is actually told can be checked without opening a modal dialog.
    static QString describe(const FileConflict &conflict);

protected:
    void showEvent(QShowEvent *event) override;

private:
    ErrorAction m_result = ErrorAction::Cancel;
};
