#include "TranslationManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QTranslator>

namespace {

// Translators are QObject children of the application that installed them. QPointer
// clears itself when that application is destroyed, so a subsequent application in
// the same process can never remove or delete a dangling translator.
QPointer<QCoreApplication> g_owner;
QPointer<QTranslator> g_appTranslator;
QPointer<QTranslator> g_qtTranslator;

void removeTrackedTranslators() {
    if (g_owner) {
        if (g_appTranslator)
            g_owner->removeTranslator(g_appTranslator);
        if (g_qtTranslator)
            g_owner->removeTranslator(g_qtTranslator);
    }
    if (g_appTranslator)
        delete g_appTranslator.data();
    if (g_qtTranslator)
        delete g_qtTranslator.data();
    g_appTranslator.clear();
    g_qtTranslator.clear();
    g_owner.clear();
}

// External catalogs live alongside the config file so translators can drop in a
// .qm without rebuilding.
QString externalDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/FileCommander/translations");
}

// Native language names shown in the menu (kept out of tr() so each reads the
// same regardless of the active UI language). Codes without an entry here fall
// back to the bare code.
QString nativeName(const QString &code) {
    static const QMap<QString, QString> kNames = {
        {"en", QStringLiteral("English")},        {"zh_CN", QStringLiteral("简体中文")},
        {"zh_TW", QStringLiteral("繁體中文")},    {"fr", QStringLiteral("Français")},
        {"de", QStringLiteral("Deutsch")},        {"es", QStringLiteral("Español")},
        {"ru", QStringLiteral("Русский")},        {"ja", QStringLiteral("日本語")},
        {"ko", QStringLiteral("한국어")},         {"pt_BR", QStringLiteral("Português (Brasil)")},
    };
    return kNames.value(code, code);
}

// Resolves a language setting to the catalog codes to try, in order. "auto"
// follows the system locale; a region locale falls back to its base language.
QStringList candidatesFor(const QString &language) {
    QString effective = language;
    if (effective.isEmpty() || effective == QStringLiteral("auto"))
        effective = QLocale::system().name(); // e.g. "zh_CN", "fr_FR"
    QStringList candidates;
    candidates << effective;
    const int sep = effective.indexOf(QLatin1Char('_'));
    if (sep > 0)
        candidates << effective.left(sep);
    return candidates;
}

} // namespace

bool TranslationManager::loadCatalog(QTranslator *t, const QString &code) {
    // External dir wins, so a hand-supplied catalog overrides the bundled one.
    const QString external = externalDir() + QStringLiteral("/ttc_%1.qm").arg(code);
    if (QFileInfo::exists(external) && t->load(external))
        return true;
    return t->load(QStringLiteral(":/translations/ttc_%1.qm").arg(code));
}

bool TranslationManager::loadQtCatalog(QTranslator *t, const QString &code) {
    const QString translations = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
    return t->load(QStringLiteral("qtbase_%1").arg(code), translations);
}

void TranslationManager::install(QCoreApplication &app, const QString &language) {
    switchTo(app, language);
}

void TranslationManager::switchTo(QCoreApplication &app, const QString &language) {
    // Only the application that installed this pair may remove it. If callers
    // switch to another live QCoreApplication, leave the old pair with its owner
    // and forget our handles; the old application's QObject destruction cleans it
    // up. This avoids mutating either application's translator list incorrectly.
    if (g_owner == &app) {
        removeTrackedTranslators();
    } else {
        g_appTranslator.clear();
        g_qtTranslator.clear();
        g_owner.clear();
    }

    QString effective = language;
    if (effective.isEmpty() || effective == QStringLiteral("auto"))
        effective = QLocale::system().name();
    app.setProperty("ttc.uiLanguage", effective);
    app.setProperty("ttc.qtBaseCatalogLoaded", false);

    // English is the source language. Keeping both translators absent also
    // makes the standard-button fallback use the canonical English labels.
    if (effective.startsWith(QStringLiteral("en")))
        return;

    // Qt owns the standard widgets' source strings. Install it first, then the
    // application catalog, so application translations deliberately win if they
    // provide an overlapping context.
    auto *qtTranslator = new QTranslator(&app);
    for (const QString &code : candidatesFor(language)) {
        if (loadQtCatalog(qtTranslator, code)) {
            app.installTranslator(qtTranslator);
            g_owner = &app;
            g_qtTranslator = qtTranslator;
            app.setProperty("ttc.qtBaseCatalogLoaded", true);
            qtTranslator = nullptr;
            break;
        }
    }
    delete qtTranslator;

    auto *appTranslator = new QTranslator(&app);
    for (const QString &code : candidatesFor(language)) {
        if (loadCatalog(appTranslator, code)) {
            app.installTranslator(appTranslator);
            g_owner = &app;
            g_appTranslator = appTranslator;
            return;
        }
    }
    delete appTranslator;
}

QVector<std::pair<QString, QString>> TranslationManager::available() {
    QVector<std::pair<QString, QString>> result;
    result.append({QStringLiteral("auto"), QObject::tr("Auto")});
    result.append({QStringLiteral("en"), nativeName(QStringLiteral("en"))});

    QStringList seen{QStringLiteral("en")};
    // Bundled catalogs, then any extra external ones.
    for (const QString &base : {QStringLiteral(":/translations"), externalDir()}) {
        QDir dir(base);
        const QStringList files = dir.entryList({QStringLiteral("ttc_*.qm")}, QDir::Files);
        for (const QString &file : files) {
            // "ttc_zh_CN.qm" -> "zh_CN"
            QString code = QFileInfo(file).completeBaseName();
            code.remove(0, QStringLiteral("ttc_").size());
            if (code.isEmpty() || seen.contains(code))
                continue;
            seen << code;
            result.append({code, nativeName(code)});
        }
    }
    return result;
}
