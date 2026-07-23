#pragma once

#include "FramelessDialog.h"

#include <QIcon>

class QPushButton;

// A small "About ttc" window: app icon, name, version (from TTC_VERSION), a
// one-line description, and a copyright/licence line. The icon is supplied by
// the caller (main paints the app icon in code); if none is given the dialog
// falls back to the window icon.
class AboutDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit AboutDialog(const QIcon &icon = QIcon(), QWidget *parent = nullptr);
};
