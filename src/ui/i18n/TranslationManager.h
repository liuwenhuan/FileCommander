#pragma once

#include <QString>

class QCoreApplication;

// Installs the UI translation for the app's language setting. Per the
// reference design spec, language changes take effect on next launch
// (not live) -- this keeps every widget's already-constructed text
// (menus, dialogs, buttons) correct without needing a LanguageChange
// retranslate() pass wired through every widget.
class TranslationManager {
public:
    // language: "auto" (resolves via QLocale::system()), "en" (source
    // language, no catalog), or a locale such as "zh_CN", "zh_TW", "fr",
    // "de", "es", "ru", "ja", "ko", "pt_BR". Loads the matching
    // ":/translations/ttc_<locale>.qm", falling back to the bare language
    // code (e.g. "fr_FR" -> "fr") when the exact locale has no catalog.
    static void install(QCoreApplication &app, const QString &language);
};
