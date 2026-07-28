#include "ThemedDialogs.h"

#include "FramelessDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QInputDialog>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

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

    // A loaded Qt catalog (or an app catalog installed after it) owns a non-empty
    // label. The fallback only supplies text for the buttons it manages.
    if (hasQtBaseCatalog() && !button->text().isEmpty())
        return;

    const QString localized = fallbackText(definition);
    button->setText(localized);
    button->setProperty(kManagedTextProperty, localized);
}

void applyLocalization(QMessageBox *box, bool detectApplicationOverride = true) {
    if (!box)
        return;
    for (const ButtonDefinition &definition : kButtonDefinitions)
        applyFallback(box->button(static_cast<QMessageBox::StandardButton>(definition.button)),
                      definition, detectApplicationOverride);
}

void applyLocalization(QDialogButtonBox *box, bool detectApplicationOverride = true) {
    if (!box)
        return;
    for (const ButtonDefinition &definition : kButtonDefinitions)
        applyFallback(box->button(definition.button), definition, detectApplicationOverride);
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
            QTimer::singleShot(0, this, [this] {
                if (m_messageBox)
                    applyLocalization(m_messageBox, false);
                if (m_buttonBox)
                    applyLocalization(m_buttonBox, false);
            });
        }
        return QObject::eventFilter(watched, event);
    }

private:
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

} // namespace

namespace ttc {

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

QMessageBox::StandardButton message(QWidget *parent, QMessageBox::Icon icon, const QString &title,
                                    const QString &text, QMessageBox::StandardButtons buttons,
                                    QMessageBox::StandardButton defaultButton) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    // A real QMessageBox, but embedded as a plain child widget (Qt::Widget)
    // rather than a top-level window — so it renders its icon / text / standard
    // buttons inside our themed frame instead of its own native decorations.
    auto *box = new QMessageBox(&dlg);
    box->setIcon(icon);
    box->setWindowTitle(title);
    box->setText(text);
    box->setStandardButtons(buttons);
    localizeStandardButtons(box);
    if (defaultButton != QMessageBox::NoButton)
        box->setDefaultButton(defaultButton);
    box->setWindowFlags(Qt::Widget);

    QMessageBox::StandardButton result = QMessageBox::NoButton;
    QObject::connect(box, &QMessageBox::buttonClicked, &dlg,
                     [&](QAbstractButton *b) { result = box->standardButton(b); });
    QObject::connect(box, &QMessageBox::finished, &dlg, [&](int) { dlg.accept(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(box);

    dlg.exec();
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

    bool accepted = false;
    QObject::connect(input, &QInputDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QInputDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);
    // Give short prompts a comfortable minimum width (QInputDialog alone is very
    // narrow).
    dlg.setMinimumWidth(dlg.sizeHint().width() < 360 ? 360 : dlg.sizeHint().width());

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

    bool accepted = false;
    QObject::connect(input, &QInputDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QInputDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);
    dlg.setMinimumWidth(dlg.sizeHint().width() < 360 ? 360 : dlg.sizeHint().width());

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
    input->setWindowFlags(Qt::Widget);
    input->setOptions(options | QFontDialog::DontUseNativeDialog);
    localizeStandardButtons(input->findChild<QDialogButtonBox *>());

    bool accepted = false;
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
    return accepted ? input->currentFont() : initial;
}

} // namespace ttc
