#include "TranslationManager.h"

#include <QCoreApplication>
#include <QLocale>
#include <QStringList>
#include <QTranslator>

void TranslationManager::install(QCoreApplication &app, const QString &language) {
    QString effective = language;
    if (effective.isEmpty() || effective == QStringLiteral("auto"))
        effective = QLocale::system().name(); // e.g. "zh_CN", "fr_FR", "en_US"

    if (effective.startsWith(QStringLiteral("en")))
        return; // English is the tr() source language; nothing to load

    // Try the full locale first (e.g. "zh_CN", "pt_BR"), then fall back to the
    // bare language code (e.g. "fr_FR" -> "fr") so a system locale we don't ship
    // a region-specific catalog for still resolves to the base translation.
    QStringList candidates;
    candidates << effective;
    const int sep = effective.indexOf(QLatin1Char('_'));
    if (sep > 0)
        candidates << effective.left(sep);

    for (const QString &code : candidates) {
        auto *translator = new QTranslator(&app);
        if (translator->load(QStringLiteral(":/translations/ttc_%1.qm").arg(code))) {
            app.installTranslator(translator);
            return;
        }
        delete translator;
    }
}
