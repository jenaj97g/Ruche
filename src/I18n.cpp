#include "I18n.h"

#include "Library.h"

#include <QApplication>
#include <QFile>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace i18n {

const QVector<QPair<QString, QString>> &languages()
{
    static const QVector<QPair<QString, QString>> list = {
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("be"), QStringLiteral("Беларуская")},
        {QStringLiteral("ca"), QStringLiteral("Català")},
        {QStringLiteral("cs"), QStringLiteral("Čeština")},
        {QStringLiteral("de"), QStringLiteral("Deutsch")},
        {QStringLiteral("es"), QStringLiteral("Español")},
        {QStringLiteral("fr"), QStringLiteral("Français")},
        {QStringLiteral("ga"), QStringLiteral("Gaeilge")},
        {QStringLiteral("hu"), QStringLiteral("Magyar")},
        {QStringLiteral("nl"), QStringLiteral("Nederlands")},
        {QStringLiteral("pt"), QStringLiteral("Português (Portugal)")},
        {QStringLiteral("pt_BR"), QStringLiteral("Português (Brasil)")},
        {QStringLiteral("ro"), QStringLiteral("Română")},
        {QStringLiteral("ru"), QStringLiteral("Русский")},
        {QStringLiteral("sr"), QStringLiteral("Српски")},
        {QStringLiteral("sr_Latn"), QStringLiteral("Srpski (latinica)")},
        {QStringLiteral("sv"), QStringLiteral("Svenska")},
        {QStringLiteral("ja"), QStringLiteral("日本語")},
        {QStringLiteral("zh_CN"), QStringLiteral("中文（简体）")},
    };
    return list;
}

static bool known(const QString &code)
{
    for (const auto &l : languages())
        if (l.first == code)
            return true;
    return false;
}

// "pt-BR" -> pt_BR, "sr-Latn-RS" -> sr_Latn, "zh-Hans-CN" -> zh_CN, "fr-BE" -> fr
static QString fromUiLanguage(const QString &tag)
{
    const QStringList p = QString(tag).replace(QLatin1Char('_'), QLatin1Char('-')).split(QLatin1Char('-'));
    const QString lang = p.value(0).toLower();
    if (lang == QLatin1String("pt"))
        return p.contains(QLatin1String("BR")) ? QStringLiteral("pt_BR") : QStringLiteral("pt");
    if (lang == QLatin1String("sr"))
        return p.contains(QLatin1String("Latn")) ? QStringLiteral("sr_Latn") : QStringLiteral("sr");
    if (lang == QLatin1String("zh"))
        return QStringLiteral("zh_CN");
    return lang;
}

QString current()
{
    const QString chosen = QSettings(paths::configFile(), QSettings::IniFormat).value(QStringLiteral("ui/language")).toString();
    if (!chosen.isEmpty() && known(chosen))
        return chosen;
    for (const QString &tag : QLocale::system().uiLanguages()) {
        const QString code = fromUiLanguage(tag);
        if (known(code))
            return code;
    }
    return QStringLiteral("en");
}

void install(QApplication &app)
{
    const QString code = current();
    if (code == QLatin1String("en"))
        return;
    QLocale::setDefault(QLocale(code == QLatin1String("sr_Latn") ? QStringLiteral("sr_Latn_RS") : code));
    auto *ruche = new QTranslator(&app);
    if (ruche->load(QStringLiteral(":/i18n/ruche_%1.qm").arg(code)))
        QApplication::installTranslator(ruche);
    // Qt's own strings (OK, Cancel, file and colour dialogs), where Qt has them
    const QString qtCode = code == QLatin1String("sr_Latn") ? QStringLiteral("sr") : code;
    auto *qt = new QTranslator(&app);
    if (qt->load(QStringLiteral(":/i18n/qtbase_%1.qm").arg(qtCode)) ||
        qt->load(QStringLiteral(":/i18n/qtbase_%1.qm").arg(qtCode.section(QLatin1Char('_'), 0, 0))))
        QApplication::installTranslator(qt);
}

} // namespace i18n
