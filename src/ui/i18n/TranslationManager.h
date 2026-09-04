#pragma once

#include <QString>
#include <QVector>
#include <utility>

class QCoreApplication;

// Loads and (live-)switches the UI translation.
//
// The bundled catalog is always installed first. A catalog in the user's
// external translations directory (~/.config/FileCommander/translations/
// ttc_<code>.qm) is installed as an overlay, so it can override known strings
// without making newer bundled strings fall back to English.
class TranslationManager {
public:
    // Startup install for the saved language ("auto"/"en"/locale like "zh_CN").
    static void install(QCoreApplication &app, const QString &language);

    // Runtime switch: removes the current catalog and installs the new one. Qt
    // then posts QEvent::LanguageChange to every top-level widget, which drives
    // each widget's retranslate. No restart required.
    static void switchTo(QCoreApplication &app, const QString &language);

    // (code, native label) for every language that has a loadable catalog,
    // bundled or external. Always includes "auto" and "en". Used to populate the
    // View > Language menu so a dropped-in .qm appears without code changes.
    static QVector<std::pair<QString, QString>> available();

private:
    // Loads ttc_<code>.qm from the bundled resources.
    static bool loadBundledCatalog(class QTranslator *t, const QString &code);

    // Loads ttc_<code>.qm from the user's optional override directory.
    static bool loadExternalCatalog(class QTranslator *t, const QString &code);

    // Loads Qt's qtbase_<code>.qm from Qt's installed translations directory.
    static bool loadQtCatalog(class QTranslator *t, const QString &code);
};
