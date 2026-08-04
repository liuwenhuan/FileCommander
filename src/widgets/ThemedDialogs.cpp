#include "ThemedDialogs.h"

#include "FramelessDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLocale>
#include <QMargins>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace {

struct ButtonDefinition {
    QDialogButtonBox::StandardButton button;
    const char *english;
};

const ButtonDefinition kButtonDefinitions[] = {
    {QDialogButtonBox::Ok, "OK"},
    {QDialogButtonBox::Save, "Save"},
    {QDialogButtonBox::SaveAll, "Save All"},
    {QDialogButtonBox::Open, "Open"},
    {QDialogButtonBox::Yes, "&Yes"},
    {QDialogButtonBox::YesToAll, "Yes to &All"},
    {QDialogButtonBox::No, "&No"},
    {QDialogButtonBox::NoToAll, "N&o to All"},
    {QDialogButtonBox::Abort, "Abort"},
    {QDialogButtonBox::Retry, "Retry"},
    {QDialogButtonBox::Ignore, "Ignore"},
    {QDialogButtonBox::Close, "Close"},
    {QDialogButtonBox::Cancel, "Cancel"},
    {QDialogButtonBox::Discard, "Discard"},
    {QDialogButtonBox::Help, "Help"},
    {QDialogButtonBox::Apply, "Apply"},
    {QDialogButtonBox::Reset, "Reset"},
    {QDialogButtonBox::RestoreDefaults, "Restore Defaults"},
};

QString languageCode() {
    QString language = qApp->property("ttc.uiLanguage").toString();
    if (language.isEmpty() || language == QStringLiteral("auto"))
        language = QLocale::system().name();
    return language;
}

QString fallbackText(const ButtonDefinition &definition) {
    const QString code = languageCode();
    if (code.startsWith(QStringLiteral("en")))
        return QString::fromLatin1(definition.english);

    static const QHash<QString, QStringList> kTranslations = {
        {QStringLiteral("zh_CN"),
         {QStringLiteral("确定"), QStringLiteral("保存"), QStringLiteral("全部保存"),
          QStringLiteral("打开"), QStringLiteral("是"), QStringLiteral("全部选是"),
          QStringLiteral("否"), QStringLiteral("全部选否"), QStringLiteral("中止"),
          QStringLiteral("重试"), QStringLiteral("忽略"), QStringLiteral("关闭"),
          QStringLiteral("取消"), QStringLiteral("放弃"), QStringLiteral("帮助"),
          QStringLiteral("应用"), QStringLiteral("重置"), QStringLiteral("恢复默认值")}},
        {QStringLiteral("zh_TW"),
         {QStringLiteral("確定"), QStringLiteral("儲存"), QStringLiteral("全部儲存"),
          QStringLiteral("開啟"), QStringLiteral("是"), QStringLiteral("全部選是"),
          QStringLiteral("否"), QStringLiteral("全部選否"), QStringLiteral("中止"),
          QStringLiteral("重試"), QStringLiteral("忽略"), QStringLiteral("關閉"),
          QStringLiteral("取消"), QStringLiteral("捨棄"), QStringLiteral("說明"),
          QStringLiteral("套用"), QStringLiteral("重設"), QStringLiteral("還原預設值")}},
        {QStringLiteral("fr"),
         {QStringLiteral("OK"), QStringLiteral("Enregistrer"), QStringLiteral("Tout enregistrer"),
          QStringLiteral("Ouvrir"), QStringLiteral("Oui"), QStringLiteral("Oui pour tous"),
          QStringLiteral("Non"), QStringLiteral("Non pour tous"), QStringLiteral("Abandonner"),
          QStringLiteral("Réessayer"), QStringLiteral("Ignorer"), QStringLiteral("Fermer"),
          QStringLiteral("Annuler"), QStringLiteral("Ignorer"), QStringLiteral("Aide"),
          QStringLiteral("Appliquer"), QStringLiteral("Réinitialiser"),
          QStringLiteral("Restaurer les valeurs par défaut")}},
        {QStringLiteral("de"),
         {QStringLiteral("OK"), QStringLiteral("Speichern"), QStringLiteral("Alle speichern"),
          QStringLiteral("Öffnen"), QStringLiteral("Ja"), QStringLiteral("Ja für alle"),
          QStringLiteral("Nein"), QStringLiteral("Nein für alle"), QStringLiteral("Abbrechen"),
          QStringLiteral("Wiederholen"), QStringLiteral("Ignorieren"), QStringLiteral("Schließen"),
          QStringLiteral("Abbrechen"), QStringLiteral("Verwerfen"), QStringLiteral("Hilfe"),
          QStringLiteral("Anwenden"), QStringLiteral("Zurücksetzen"),
          QStringLiteral("Standardeinstellungen wiederherstellen")}},
        {QStringLiteral("es"),
         {QStringLiteral("Aceptar"), QStringLiteral("Guardar"), QStringLiteral("Guardar todo"),
          QStringLiteral("Abrir"), QStringLiteral("Sí"), QStringLiteral("Sí a todo"),
          QStringLiteral("No"), QStringLiteral("No a todo"), QStringLiteral("Abortar"),
          QStringLiteral("Reintentar"), QStringLiteral("Ignorar"), QStringLiteral("Cerrar"),
          QStringLiteral("Cancelar"), QStringLiteral("Descartar"), QStringLiteral("Ayuda"),
          QStringLiteral("Aplicar"), QStringLiteral("Restablecer"),
          QStringLiteral("Restaurar valores predeterminados")}},
        {QStringLiteral("ru"),
         {QStringLiteral("ОК"), QStringLiteral("Сохранить"), QStringLiteral("Сохранить все"),
          QStringLiteral("Открыть"), QStringLiteral("Да"), QStringLiteral("Да для всех"),
          QStringLiteral("Нет"), QStringLiteral("Нет для всех"), QStringLiteral("Прервать"),
          QStringLiteral("Повторить"), QStringLiteral("Игнорировать"), QStringLiteral("Закрыть"),
          QStringLiteral("Отмена"), QStringLiteral("Не сохранять"), QStringLiteral("Справка"),
          QStringLiteral("Применить"), QStringLiteral("Сбросить"),
          QStringLiteral("Восстановить по умолчанию")}},
        {QStringLiteral("ja"),
         {QStringLiteral("OK"), QStringLiteral("保存"), QStringLiteral("すべて保存"),
          QStringLiteral("開く"), QStringLiteral("はい"), QStringLiteral("すべてはい"),
          QStringLiteral("いいえ"), QStringLiteral("すべていいえ"), QStringLiteral("中止"),
          QStringLiteral("再試行"), QStringLiteral("無視"), QStringLiteral("閉じる"),
          QStringLiteral("キャンセル"), QStringLiteral("破棄"), QStringLiteral("ヘルプ"),
          QStringLiteral("適用"), QStringLiteral("リセット"), QStringLiteral("既定値に戻す")}},
        {QStringLiteral("ko"),
         {QStringLiteral("확인"), QStringLiteral("저장"), QStringLiteral("모두 저장"),
          QStringLiteral("열기"), QStringLiteral("예"), QStringLiteral("모두 예"),
          QStringLiteral("아니요"), QStringLiteral("모두 아니요"), QStringLiteral("중단"),
          QStringLiteral("재시도"), QStringLiteral("무시"), QStringLiteral("닫기"),
          QStringLiteral("취소"), QStringLiteral("버리기"), QStringLiteral("도움말"),
          QStringLiteral("적용"), QStringLiteral("재설정"), QStringLiteral("기본값 복원")}},
        {QStringLiteral("pt_BR"),
         {QStringLiteral("OK"), QStringLiteral("Salvar"), QStringLiteral("Salvar tudo"),
          QStringLiteral("Abrir"), QStringLiteral("Sim"), QStringLiteral("Sim para todos"),
          QStringLiteral("Não"), QStringLiteral("Não para todos"), QStringLiteral("Abortar"),
          QStringLiteral("Tentar novamente"), QStringLiteral("Ignorar"), QStringLiteral("Fechar"),
          QStringLiteral("Cancelar"), QStringLiteral("Descartar"), QStringLiteral("Ajuda"),
          QStringLiteral("Aplicar"), QStringLiteral("Redefinir"),
          QStringLiteral("Restaurar padrões")}},
    };

    const int separator = code.indexOf(QLatin1Char('_'));
    const QString baseCode = separator > 0 ? code.left(separator) : code;
    const QStringList translations = kTranslations.value(code, kTranslations.value(baseCode));
    const int index = &definition - kButtonDefinitions;
    return translations.value(index, QString::fromLatin1(definition.english));
}

bool hasQtBaseCatalog() {
    return qApp->property("ttc.qtBaseCatalogLoaded").toBool();
}

constexpr char kManagedProperty[] = "ttc.standardButtonManaged";
constexpr char kManagedTextProperty[] = "ttc.standardButtonManagedText";
constexpr char kOverrideProperty[] = "ttc.standardButtonOverrideText";
constexpr char kFilterInstalledProperty[] = "ttc.standardButtonFilterInstalled";

QIcon standardButtonIcon(QAbstractButton *button,
                         QDialogButtonBox::StandardButton standardButton) {
    if (!button)
        return {};

    const QColor color = button->palette().color(QPalette::ButtonText);
    QIcon icon;
    for (const int size : {16, 20, 24}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color, qMax(1.5, size / 9.0), Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        const qreal scale = size / 16.0;
        auto point = [scale](qreal x, qreal y) { return QPointF(x * scale, y * scale); };
        auto rect = [scale](qreal x, qreal y, qreal w, qreal h) {
            return QRectF(x * scale, y * scale, w * scale, h * scale);
        };

        switch (standardButton) {
        case QDialogButtonBox::Ok:
        case QDialogButtonBox::Yes:
        case QDialogButtonBox::YesToAll:
        case QDialogButtonBox::Apply: {
            QPolygonF checkMark;
            checkMark << point(3, 8) << point(6.5, 11.5) << point(13, 4.5);
            painter.drawPolyline(checkMark);
            break;
        }
        case QDialogButtonBox::No:
        case QDialogButtonBox::NoToAll:
        case QDialogButtonBox::Abort:
        case QDialogButtonBox::Close:
        case QDialogButtonBox::Cancel:
        case QDialogButtonBox::Discard:
            painter.drawLine(point(4, 4), point(12, 12));
            painter.drawLine(point(12, 4), point(4, 12));
            break;
        case QDialogButtonBox::Save:
        case QDialogButtonBox::SaveAll:
            painter.drawRect(rect(3, 2.5, 10, 11));
            painter.drawRect(rect(5, 3, 5, 3.5));
            painter.drawRect(rect(5, 9, 6, 4));
            break;
        case QDialogButtonBox::Open: {
            QPainterPath path;
            path.moveTo(point(2.5, 12.5));
            path.lineTo(point(4, 6));
            path.lineTo(point(7, 6));
            path.lineTo(point(8.5, 8));
            path.lineTo(point(13.5, 8));
            path.lineTo(point(12, 12.5));
            path.closeSubpath();
            painter.drawPath(path);
            break;
        }
        case QDialogButtonBox::Retry:
        case QDialogButtonBox::Reset:
        case QDialogButtonBox::RestoreDefaults: {
            painter.drawArc(rect(3, 3, 10, 10), 35 * 16, 285 * 16);
            QPolygonF arrowHead;
            arrowHead << point(3, 4) << point(3, 8) << point(7, 7);
            painter.drawPolyline(arrowHead);
            break;
        }
        case QDialogButtonBox::Ignore:
            painter.drawLine(point(4, 8), point(12, 8));
            break;
        case QDialogButtonBox::Help:
            painter.drawArc(rect(4.5, 2.5, 7, 6), 0, 180 * 16);
            painter.drawLine(point(11.5, 5.5), point(8, 9));
            painter.drawPoint(point(8, 12.5));
            break;
        default:
            painter.drawEllipse(rect(4, 4, 8, 8));
            break;
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

void applyButtonPresentation(QAbstractButton *button,
                             const ButtonDefinition &definition) {
    if (!button)
        return;
    button->setIcon(standardButtonIcon(button, definition.button));
    button->setIconSize(QSize(16, 16));
}

void applyFallback(QAbstractButton *button, const ButtonDefinition &definition,
                   bool detectApplicationOverride) {
    if (!button)
        return;

    const QVariant overrideText = button->property(kOverrideProperty);
    if (overrideText.isValid()) {
        button->setText(overrideText.toString());
        return;
    }

    const QString canonical = QString::fromLatin1(definition.english);
    const QVariant managed = button->property(kManagedProperty);
    if (managed.isValid() && managed.toBool() && detectApplicationOverride &&
        !hasQtBaseCatalog() &&
        button->text() != button->property(kManagedTextProperty).toString()) {
        // A caller changed a fallback-owned label since it was last rendered.
        // Stop managing it rather than treating an English override as Qt's
        // canonical source text.
        button->setProperty(kManagedProperty, false);
        return;
    }

    if (!managed.isValid()) {
        // First localization records who owns each button. A non-standard label
        // is application text; canonical source text is managed by the fallback.
        button->setProperty(kManagedProperty,
                            button->text().isEmpty() || button->text() == canonical);
    }
    if (!button->property(kManagedProperty).toBool())
        return;

    // A loaded Qt catalog owns the label only where it actually translated it.
    // Its mere presence is not proof of that, and treating it as proof is what
    // left "Cancel" in English on a Chinese UI: qtbase_zh_CN.qm loaded fine and
    // returned the source string unchanged for QPlatformTheme/Cancel, so this
    // stood down and nothing else filled it in. Text still identical to the
    // canonical English means Qt had its chance and produced nothing.
    if (hasQtBaseCatalog() && !button->text().isEmpty() && button->text() != canonical)
        return;

    const QString localized = fallbackText(definition);
    button->setText(localized);
    button->setProperty(kManagedTextProperty, localized);
}

void applyLocalization(QMessageBox *box, bool detectApplicationOverride = true) {
    if (!box)
        return;
    for (const ButtonDefinition &definition : kButtonDefinitions) {
        QAbstractButton *button =
            box->button(static_cast<QMessageBox::StandardButton>(definition.button));
        applyFallback(button, definition, detectApplicationOverride);
        applyButtonPresentation(button, definition);
    }
}

void applyLocalization(QDialogButtonBox *box, bool detectApplicationOverride = true) {
    if (!box)
        return;
    for (const ButtonDefinition &definition : kButtonDefinitions) {
        QAbstractButton *button = box->button(definition.button);
        applyFallback(button, definition, detectApplicationOverride);
        applyButtonPresentation(button, definition);
    }
}

void recordApplicationOverride(QAbstractButton *button) {
    if (!button || button->property(kOverrideProperty).isValid() || hasQtBaseCatalog())
        return;
    if (button->property(kManagedProperty).toBool() &&
        button->text() != button->property(kManagedTextProperty).toString())
        button->setProperty(kManagedProperty, false);
}

void recordApplicationOverrides(QMessageBox *box) {
    if (!box)
        return;
    for (const ButtonDefinition &definition : kButtonDefinitions)
        recordApplicationOverride(
            box->button(static_cast<QMessageBox::StandardButton>(definition.button)));
}

void recordApplicationOverrides(QDialogButtonBox *box) {
    if (!box)
        return;
    for (const ButtonDefinition &definition : kButtonDefinitions)
        recordApplicationOverride(box->button(definition.button));
}

class StandardButtonLocalizationFilter final : public QObject {
public:
    explicit StandardButtonLocalizationFilter(QMessageBox *box)
        : QObject(box), m_messageBox(box) {}
    explicit StandardButtonLocalizationFilter(QDialogButtonBox *box)
        : QObject(box), m_buttonBox(box) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::LanguageChange) {
            if (m_messageBox)
                recordApplicationOverrides(m_messageBox);
            if (m_buttonBox)
                recordApplicationOverrides(m_buttonBox);
            scheduleRefresh(false);
        } else if (event->type() == QEvent::PaletteChange ||
                   event->type() == QEvent::StyleChange ||
                   event->type() == QEvent::ApplicationPaletteChange) {
            scheduleRefresh(true);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void scheduleRefresh(bool detectApplicationOverride) {
        QTimer::singleShot(0, this, [this, detectApplicationOverride] {
            if (m_messageBox)
                applyLocalization(m_messageBox, detectApplicationOverride);
            if (m_buttonBox)
                applyLocalization(m_buttonBox, detectApplicationOverride);
        });
    }

    QPointer<QMessageBox> m_messageBox;
    QPointer<QDialogButtonBox> m_buttonBox;
};

template <typename T> void installLanguageChangeFilter(T *box) {
    if (!box || box->property(kFilterInstalledProperty).toBool())
        return;
    box->setProperty(kFilterInstalledProperty, true);
    auto *filter = new StandardButtonLocalizationFilter(box);
    box->installEventFilter(filter);
}

// How wide one of these prompts is allowed to be before the text it carries
// stops deciding: 640 px is what OverwriteConfirmDialog already picks for the
// same kind of path-bearing message, and two thirds of the screen is the
// ceiling OperationErrorDialog already applies to a modal prompt -- on a small
// screen that is the tighter of the two. Neither figure is derived from the
// message, which is the point: a copy failure naming two absolute paths asks
// for 5337 px (measured) if you let it.
constexpr int kComfortableDialogWidth = 640;

// A short prompt would look mean at its natural width, so it gets a floor. This
// is a floor only -- raising it to fit the content is what made these windows
// oversized *and* unshrinkable.
constexpr int kMinimumPromptWidth = 360;

QRect availableGeometryFor(const QWidget *widget) {
    const QScreen *widgetScreen = widget ? widget->screen() : nullptr;
    if (!widgetScreen)
        widgetScreen = QGuiApplication::primaryScreen();
    return widgetScreen ? widgetScreen->availableGeometry() : QRect();
}

int comfortableWidthFor(const QWidget *widget) {
    const QRect available = availableGeometryFor(widget);
    if (available.isEmpty())
        return kComfortableDialogWidth;
    return qMin(kComfortableDialogWidth, available.width() * 2 / 3);
}

// The translucent shadow band FramelessDialog draws outside its content, so a
// content width can be turned into a window width and back.
int chromeWidth(const QWidget *dialog) {
    const QMargins margins = dialog->contentsMargins();
    return margins.left() + margins.right();
}

// A frameless wrapper whose size is decided by the single Qt dialog embedded in
// it, rather than the other way round.
//
// Qt's own dialogs only work out their wrapped, screen-bounded width when they
// are first shown -- QMessageBoxPrivate::updateSize() turns word wrap on, clamps
// the result, and ends with setFixedSize(). Until that runs, the embedded box
// reports the width of its *unwrapped* text: 5337 px, measured, for a copy
// failure naming two absolute paths. The wrapper is laid out from that hint
// before its children are shown, and QLayout turns it into the wrapper's
// minimum width as well, so a later resize() cannot bring it back down. That is
// how a warning ended up spanning the screen with its text wrapping in a narrow
// column and a wide empty band beside every line.
//
// SetFixedSize keeps the window equal to its content whenever the layout runs,
// and re-running the layout from showEvent -- after the embedded dialog has
// settled, but before the window is mapped and QDialog::showEvent centres it --
// is what makes the settled size the one the user sees. No band, no flash, and
// no width that depends on the longest unbroken string in the message.
class ContentSizedDialog final : public FramelessDialog {
public:
    explicit ContentSizedDialog(QWidget *parent) : FramelessDialog(parent) {}

    void embed(QWidget *content) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        layout->addWidget(content);
    }

protected:
    void showEvent(QShowEvent *event) override {
        if (QLayout *contentLayout = layout()) {
            // invalidate() first: activate() is a no-op on a layout that still
            // believes it is current, and whether the embedded dialog's own
            // resize dirtied it is Qt's business, not ours to assume.
            contentLayout->invalidate();
            contentLayout->activate();
        }
        FramelessDialog::showEvent(event);
    }
};

// QInputDialog's prompt label does not wrap, so its minimumSizeHint is the whole
// prompt on one line -- and callers put file names and paths in there. That
// minimum reaches the wrapper window through the layout, which is how a rename
// prompt measured 5473 px wide.
void relaxPromptLabel(QInputDialog *input) {
    if (!input)
        return;
    // The prompt is the only QLabel QInputDialog builds; matching on the text as
    // well keeps this from silently re-styling some future sibling.
    for (QLabel *label : input->findChildren<QLabel *>()) {
        if (label->text() != input->labelText())
            continue;
        ttc::relaxLabelWidth(label);
        return;
    }
}

// A comfortable width for a prompt, decided by the screen rather than by
// whatever was interpolated into the label.
void sizePromptDialog(FramelessDialog *dlg) {
    const QRect available = availableGeometryFor(dlg);
    const int floorWidth =
        available.isEmpty() ? kMinimumPromptWidth : qMin(kMinimumPromptWidth, available.width());
    // A floor, not a fit: setMinimumWidth(sizeHint()) is what used to pin these
    // windows to the length of their own label, leaving the user unable to
    // shrink a window that was already too wide to look like it belonged to its
    // content.
    dlg->setMinimumWidth(floorWidth);
    // qMax keeps the floor authoritative on a screen too narrow for both.
    const int cap = qMax(floorWidth, comfortableWidthFor(dlg));
    dlg->resize(qBound(floorWidth, dlg->sizeHint().width(), cap), dlg->sizeHint().height());
}

} // namespace

namespace ttc {

void relaxLabelWidth(QLabel *label) {
    if (!label)
        return;
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    // Ignored, not Minimum/Preferred: QWidgetItem zeroes both the size hint and
    // the smart minimum for an Ignored policy, which is what keeps the text out
    // of the window's width entirely rather than merely reducing its say.
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void localizeStandardButtons(QMessageBox *box) {
    applyLocalization(box);
    installLanguageChangeFilter(box);
}

void localizeStandardButtons(QDialogButtonBox *box) {
    applyLocalization(box);
    installLanguageChangeFilter(box);
}

void setStandardButtonOverride(QAbstractButton *button, const QString &text) {
    if (!button)
        return;
    button->setProperty(kManagedProperty, false);
    button->setProperty(kOverrideProperty, text);
    button->setText(text);
}

QDialog *createMessageDialog(QWidget *parent, QMessageBox::Icon icon, const QString &title,
                             const QString &text, QMessageBox::StandardButtons buttons,
                             QMessageBox::StandardButton defaultButton) {
    auto *dlg = new ContentSizedDialog(parent);
    dlg->setWindowTitle(title);

    // A real QMessageBox, but embedded as a plain child widget (Qt::Widget)
    // rather than a top-level window — so it renders its icon / text / standard
    // buttons inside our themed frame instead of its own native decorations.
    auto *box = new QMessageBox(dlg);
    box->setIcon(icon);
    box->setWindowTitle(title);
    box->setText(text);
    box->setStandardButtons(buttons);
    localizeStandardButtons(box);
    if (defaultButton != QMessageBox::NoButton)
        box->setDefaultButton(defaultButton);
    box->setWindowFlags(Qt::Widget);
    // Everything the box says about its own width before it is shown is the
    // unwrapped text (see ContentSizedDialog), and the window is laid out from
    // it once before the box ever gets a show event. Cap the box for that one
    // pass so the window is never even briefly sized to the message; the box
    // replaces this with its own wrapped size, via setFixedSize, on show.
    box->setMaximumWidth(qMax(0, comfortableWidthFor(dlg) - chromeWidth(dlg)));

    dlg->embed(box);
    return dlg;
}

QMessageBox::StandardButton message(QWidget *parent, QMessageBox::Icon icon, const QString &title,
                                    const QString &text, QMessageBox::StandardButtons buttons,
                                    QMessageBox::StandardButton defaultButton) {
    const std::unique_ptr<QDialog> dlg(
        createMessageDialog(parent, icon, title, text, buttons, defaultButton));
    auto *box = dlg->findChild<QMessageBox *>();

    QMessageBox::StandardButton result = QMessageBox::NoButton;
    QObject::connect(box, &QMessageBox::buttonClicked, dlg.get(),
                     [&](QAbstractButton *b) { result = box->standardButton(b); });
    QObject::connect(box, &QMessageBox::finished, dlg.get(), [&](int) { dlg->accept(); });

    dlg->exec();
    return result;
}

QString getText(QWidget *parent, const QString &title, const QString &label,
                QLineEdit::EchoMode mode, const QString &text, bool *ok) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    auto *input = new QInputDialog(&dlg);
    input->setWindowFlags(Qt::Widget);
    input->setInputMode(QInputDialog::TextInput);
    input->setLabelText(label);
    input->setTextEchoMode(mode);
    input->setTextValue(text);
    localizeStandardButtons(input->findChild<QDialogButtonBox *>());
    relaxPromptLabel(input);

    bool accepted = false;
    QObject::connect(input, &QInputDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QInputDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);
    sizePromptDialog(&dlg);

    dlg.exec();
    if (ok)
        *ok = accepted;
    return accepted ? input->textValue() : QString();
}

int getInt(QWidget *parent, const QString &title, const QString &label, int value, int min,
           int max, int step, bool *ok) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    auto *input = new QInputDialog(&dlg);
    input->setWindowFlags(Qt::Widget);
    input->setInputMode(QInputDialog::IntInput);
    input->setLabelText(label);
    input->setIntRange(min, max);
    input->setIntStep(step);
    input->setIntValue(value);
    localizeStandardButtons(input->findChild<QDialogButtonBox *>());
    relaxPromptLabel(input);

    bool accepted = false;
    QObject::connect(input, &QInputDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QInputDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);
    sizePromptDialog(&dlg);

    dlg.exec();
    if (ok)
        *ok = accepted;
    return accepted ? input->intValue() : value;
}

QFont getFont(bool *ok, const QFont &initial, QWidget *parent, const QString &title,
              QFontDialog::FontDialogOptions options) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    auto *input = new QFontDialog(initial, &dlg);
    input->setObjectName(QStringLiteral("ThemedFontDialog"));
    input->setWindowFlags(Qt::Widget);
    input->setOptions(options | QFontDialog::DontUseNativeDialog);
    localizeStandardButtons(input->findChild<QDialogButtonBox *>());

    bool accepted = false;
    QFont selectedFont = initial;
    QObject::connect(input, &QFontDialog::currentFontChanged, &dlg,
                     [&selectedFont](const QFont &font) { selectedFont = font; });
    QObject::connect(input, &QFontDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QFontDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);

    dlg.exec();
    if (ok)
        *ok = accepted;
    return accepted ? selectedFont : initial;
}

} // namespace ttc
