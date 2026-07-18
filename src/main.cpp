#include <QApplication>

#include "MainWindow.h"
#include "Settings.h"
#include "TranslationManager.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("ttc");
    app.setOrganizationName("ttc");

    Settings settings;
    TranslationManager::install(app, settings.language());

    MainWindow window;
    window.show();

    return app.exec();
}
