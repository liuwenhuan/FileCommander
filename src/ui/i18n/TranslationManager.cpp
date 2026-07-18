#include "TranslationManager.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

void TranslationManager::install(QCoreApplication &app, const QString &language) {
    QString effective = language;
    if (effective.isEmpty() || effective == QStringLiteral("auto"))
        effective = QLocale::system().name(); // e.g. "zh_CN", "en_US"

    if (!effective.startsWith(QStringLiteral("zh")))
        return; // English is the tr() source language; nothing to load

    auto *translator = new QTranslator(&app);
    if (translator->load(QStringLiteral(":/translations/ttc_zh_CN.qm")))
        app.installTranslator(translator);
}
