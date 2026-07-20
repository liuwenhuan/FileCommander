#pragma once

#include <QByteArray>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;

// Read-only file viewer (F3 lister) with a text/hex toggle, selectable text
// encoding, word-wrap toggle, and in-viewer find. Truncates very large files
// rather than loading them entirely into memory.
class TextViewer : public QWidget {
    Q_OBJECT

public:
    explicit TextViewer(QWidget *parent = nullptr);

    bool loadFile(const QString &path);

    // Formats raw bytes as an offset/hex/ascii dump. Static for unit testing.
    static QString toHexDump(const QByteArray &data);

private:
    void render();
    void findNext();

    QPlainTextEdit *m_editor;
    QComboBox *m_encoding;
    QLineEdit *m_find;
    QByteArray m_raw;
    bool m_hex = false;
    bool m_truncated = false;
    static constexpr qint64 kMaxBytes = 5 * 1024 * 1024; // 5 MB
};
