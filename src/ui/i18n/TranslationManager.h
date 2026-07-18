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
    // language: "auto" (resolves via QLocale::system()), "en", or "zh_CN".
    static void install(QCoreApplication &app, const QString &language);
};
