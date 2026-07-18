#pragma once

#include <QWidget>

class QPlainTextEdit;

// Read-only text viewer, wired to F3. Truncates very large files rather
// than loading them entirely into memory.
class TextViewer : public QWidget {
    Q_OBJECT

public:
    explicit TextViewer(QWidget *parent = nullptr);

    bool loadFile(const QString &path);

private:
    QPlainTextEdit *m_editor;
    static constexpr qint64 kMaxBytes = 5 * 1024 * 1024; // 5 MB
};
