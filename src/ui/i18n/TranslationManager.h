#pragma once

#include <QString>
#include <QVector>
#include <utility>

class QCoreApplication;

// Loads and (live-)switches the UI translation.
//
// Catalogs are looked up first in the user's external translations directory
// (~/.config/FileCommander/translations/ttc_<code>.qm) and then in the bundled
// resources (:/translations/...). The external dir lets translators drop in a
// new/updated .qm without recompiling — see resources/translations/README.
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
    // Loads ttc_<code>.qm into `t` (external dir first, then resources).
    static bool loadCatalog(class QTranslator *t, const QString &code);
};
