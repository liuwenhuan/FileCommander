#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QXmlStreamReader>

namespace {

// Every catalog shipped through resources.qrc. Kept as a literal list rather
// than a directory glob so that adding a .ts without wiring it into the .qrc
// (or the reverse) shows up as a failure instead of being silently skipped.
const QStringList &bundledLanguages() {
    static const QStringList languages = {
        QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("fr"),
        QStringLiteral("de"),    QStringLiteral("es"),    QStringLiteral("ru"),
        QStringLiteral("ja"),    QStringLiteral("ko"),    QStringLiteral("pt_BR"),
    };
    return languages;
}

QString catalogPath(const QString &language, const QString &suffix) {
    return QStringLiteral(TTC_SOURCE_DIR "/resources/translations/ttc_") + language + suffix;
}

QSet<QString> placeholders(const QString &text) {
    QSet<QString> found;
    static const QRegularExpression pattern(QStringLiteral("%(\\d|n)"));
    QRegularExpressionMatchIterator matches = pattern.globalMatch(text);
    while (matches.hasNext())
        found.insert(matches.next().captured(0));
    return found;
}

struct Entry {
    QString source;
    QString type;
    QStringList translations;
};

// A hand-rolled walk rather than QTranslator: the point is to inspect the
// *authoring* state that lrelease throws away. lrelease drops every message
// still marked type="unfinished", so a half-translated catalog compiles
// cleanly and then falls back to English at runtime with nothing to show for
// it -- which is precisely the regression this guards.
QVector<Entry> readEntries(const QString &path, QString *error) {
    QVector<Entry> entries;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot open %1").arg(path);
        return entries;
    }

    QXmlStreamReader reader(&file);
    Entry current;
    bool inMessage = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringRef name = reader.name();
            if (name == QLatin1String("message")) {
                inMessage = true;
                current = Entry();
            } else if (inMessage && name == QLatin1String("source")) {
                current.source = reader.readElementText();
            } else if (inMessage && name == QLatin1String("translation")) {
                current.type = reader.attributes().value(QLatin1String("type")).toString();
                // readElementText consumes nested <numerusform> children too, so
                // the forms are collected explicitly before falling back to the
                // plain-text form.
                while (!(reader.isEndElement() && reader.name() == QLatin1String("translation"))) {
                    reader.readNext();
                    if (reader.isStartElement() && reader.name() == QLatin1String("numerusform"))
                        current.translations.append(reader.readElementText());
                    else if (reader.isCharacters() && !reader.isWhitespace())
                        current.translations.append(reader.text().toString());
                    if (reader.atEnd())
                        break;
                }
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("message")) {
            inMessage = false;
            entries.append(current);
        }
    }
    if (reader.hasError())
        *error = QStringLiteral("%1: %2").arg(path, reader.errorString());
    return entries;
}

bool isRetired(const QString &type) {
    return type == QLatin1String("obsolete") || type == QLatin1String("vanished");
}

} // namespace

TEST(TranslationCatalogsTest, EveryBundledCatalogIsFullyTranslated) {
    for (const QString &language : bundledLanguages()) {
        QString error;
        const QVector<Entry> entries = readEntries(catalogPath(language, QStringLiteral(".ts")),
                                                   &error);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
        ASSERT_FALSE(entries.isEmpty()) << language.toStdString() << " has no messages";

        QStringList incomplete;
        for (const Entry &entry : entries) {
            if (isRetired(entry.type))
                continue;
            bool translated = false;
            for (const QString &text : entry.translations)
                translated = translated || !text.trimmed().isEmpty();
            if (entry.type == QLatin1String("unfinished") || !translated)
                incomplete.append(entry.source.left(60));
        }
        EXPECT_TRUE(incomplete.isEmpty())
            << language.toStdString() << " has " << incomplete.size()
            << " untranslated message(s), first: " << incomplete.value(0).toStdString();
    }
}

TEST(TranslationCatalogsTest, TranslationsKeepEveryPlaceholderOfTheirSource) {
    for (const QString &language : bundledLanguages()) {
        QString error;
        const QVector<Entry> entries = readEntries(catalogPath(language, QStringLiteral(".ts")),
                                                   &error);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();

        for (const Entry &entry : entries) {
            if (isRetired(entry.type))
                continue;
            const QSet<QString> expected = placeholders(entry.source);
            for (const QString &text : entry.translations) {
                if (text.trimmed().isEmpty())
                    continue;
                // A dropped %1 leaves a message that reads as if the filename or
                // count were simply missing; an invented one renders literally.
                EXPECT_EQ(placeholders(text), expected)
                    << language.toStdString() << ": " << entry.source.left(60).toStdString()
                    << " -> " << text.left(60).toStdString();
            }
        }
    }
}

TEST(TranslationCatalogsTest, EveryBundledCatalogHasACompiledCounterpart) {
    for (const QString &language : bundledLanguages()) {
        // resources.qrc embeds the .qm, not the .ts, so a stale or missing .qm
        // is what actually reaches users.
        EXPECT_TRUE(QFile::exists(catalogPath(language, QStringLiteral(".qm"))))
            << language.toStdString() << " has no compiled catalog";
    }
}

TEST(TranslationCatalogsTest, ChineseRemovableDeviceMenuIsCompiled) {
    QTranslator translator;
    ASSERT_TRUE(translator.load(QStringLiteral(":/translations/ttc_zh_CN.qm")));

    const QPair<const char *, const char *> expected[] = {
        {"Open", "打开"},
        {"Open With", "打开方式"},
        {"View", "查看"},
        {"Edit", "编辑"},
        {"Copy", "复制"},
        {"Cut", "剪切"},
        {"Move", "移动"},
        {"Rename", "重命名"},
        {"Delete", "删除"},
        {"Compress", "压缩"},
        {"Extract To", "解压到"},
        {"Extract Here", "解压到当前目录"},
        {"Extract to Folder...", "解压到指定目录"},
        {"Send To", "发送到"},
        {"Send to Device", "发送到设备"},
        {"No other device", "没有其他设备"},
        {"Calculate Folder Size", "计算文件夹大小"},
        {"Copy Path", "复制路径"},
        {"Properties", "属性"},
    };
    for (const auto &entry : expected) {
        EXPECT_EQ(translator.translate("MainWindow", entry.first),
                  QString::fromUtf8(entry.second))
            << entry.first;
    }
}

TEST(TranslationCatalogsTest, ThemesExposeAVisibleDefaultButtonState) {
    for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << theme.toStdString();
        const QString stylesheet = QString::fromUtf8(file.readAll());
        EXPECT_NE(stylesheet.indexOf(QStringLiteral("QPushButton:default")), -1)
            << theme.toStdString();
    }
}
