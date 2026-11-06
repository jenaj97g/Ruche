#include "Theme.h"
#include "ThemeFiles.h"
#include "Icons.h"
#include "Library.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>
#include <QtGlobal>
#include <cmath>
#include <utility>
#ifdef RUCHE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#endif

namespace {
struct Current {
    bool dark = true;
    QColor window{0x1c, 0x1d, 0x20}, text{0xe3, 0xe3, 0xe5}, accent = Theme::Brand;
    QColor accentText = Theme::Brand, onAccent{0x1c, 0x1d, 0x20};
    int generation = 0;
    QString file; // theme file in use
};
Current g;

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

double luminance(const QColor &c)
{
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

double contrast(const QColor &a, const QColor &b)
{
    double x = luminance(a), y = luminance(b);
    if (x < y)
        std::swap(x, y);
    return (x + 0.05) / (y + 0.05);
}

// Darker or lighter shades of the background for panels set back from the content.
QColor deep(double t)
{
    return g.dark ? mix(g.window, Qt::black, t) : mix(g.window, g.text, t * 0.35);
}

QString hex(const QColor &c) { return c.name(QColor::HexRgb); }

const char *const kCss = R"CSS(
QMainWindow, QWidget#central { background: @window; }
QToolTip { background: @raised; color: @text; border: 1px solid @line4; padding: 4px 6px; }
QWidget#topBar { background: @deep3; border-bottom: 1px solid @line; }
QLabel#brand { color: @accenttext; font-weight: 700; font-size: 15px; letter-spacing: 0.5px; }
QLabel#viewTitle { color: @text; font-weight: 600; font-size: 14px; }
QLineEdit#search { background: @field; border: 1px solid @line2; border-radius: 13px; padding: 4px 10px 4px 2px;
                   color: @text; selection-background-color: @accent; selection-color: @onaccent; min-height: 18px; }
QLineEdit#search:focus { border-color: @accent; }
QLineEdit { background: @field; border: 1px solid @line2; border-radius: 4px; padding: 4px 6px; }
QLineEdit:focus { border-color: @accent; }

QTreeWidget#navigator { background: @deep2; border: none; border-right: 1px solid @line; padding-top: 6px; outline: 0; }
QTreeWidget#navigator::item { padding: 5px 4px; border-radius: 4px; margin: 0 6px; }
QTreeWidget#navigator::item:hover { background: @field; }
QTreeWidget#navigator::item:selected { background: @navsel; color: @accenttext; }
QTreeWidget#navigator::branch { background: transparent; image: none; border: none; }

QTableView, QListView { background: @base; alternate-background-color: @altbase; border: none; outline: 0;
                        selection-background-color: @selection; selection-color: @seltext; gridline-color: transparent; }
QTableView::item, QListView::item { padding: 0 4px; border: none; }
QTableView::item:selected, QListView::item:selected { background: @selection; }
QListView#browser { background: @deep1; border-right: 1px solid @line; }
QListView#browser::item { padding: 3px 6px; }
QListView#browser::item:selected { background: @browsesel; color: @accenttext; }
QHeaderView { background: @deep1; border: none; }
QHeaderView::section { background: @deep1; color: @dim; border: none; border-bottom: 1px solid @line1;
                       border-right: 1px solid @field; padding: 4px 6px; font-weight: 600; }
QHeaderView::section:hover { color: @text; }
QHeaderView::down-arrow, QHeaderView::up-arrow { width: 8px; height: 8px; subcontrol-position: center right; }
QTableCornerButton::section { background: @deep1; border: none; }

QSplitter::handle { background: @line; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @scroll; border-radius: 4px; min-height: 30px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: @scrollhover; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: @scroll; border-radius: 4px; min-width: 30px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QWidget#playerBar { background: @deep4; border-top: 1px solid @line; }
QWidget#nowPlaying { background: @deep2; border-left: 1px solid @line; }
QLabel#npTitle { color: @strong; font-size: 15px; font-weight: 600; }
QLabel#npArtist { color: @accenttext; font-size: 13px; }
QLabel#npAlbum, QLabel#npTech { color: @dim; }
QLabel#sectionLabel { color: @dim; font-size: 11px; font-weight: 700; letter-spacing: 1px; }
QLabel#barTitle { color: @strong; font-weight: 600; }
QLabel#barArtist { color: @soft; }
QLabel#time { color: @dim; font-family: monospace; }
QLabel#status { color: @status; padding: 2px 8px; }
QWidget#statusBar { background: @deep3; border-top: 1px solid @field; }

QToolButton { background: transparent; border: none; border-radius: 4px; padding: 4px; color: @icon; }
QToolButton:hover { background: @raised; }
QToolButton:pressed { background: @line2; }
QToolButton#playButton { background: @accent; border-radius: 19px; }
QToolButton#playButton:hover { background: @accenthover; }
QToolButton#playButton:pressed { background: @accentpressed; }
QToolButton::menu-indicator { image: none; }
QPushButton { background: @raised; border: 1px solid @line3; border-radius: 4px; padding: 5px 14px; color: @text; }
QPushButton:hover { background: @line2; }
QPushButton:default { border-color: @accent; }

QSlider::groove:horizontal { height: 4px; background: @groove; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::add-page:horizontal { background: @groove; border-radius: 2px; }
QSlider::handle:horizontal { background: @strong; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: @seltext; }
QSlider::sub-page:horizontal:disabled { background: @groove; }
QSlider::handle:horizontal:disabled { background: transparent; }

QMenu { background: @field; border: 1px solid @line3; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 22px; border-radius: 3px; }
QMenu::item:selected { background: @selection; }
QMenu::separator { height: 1px; background: @line3; margin: 4px 6px; }
QMenu::icon { padding-left: 4px; }
QDialog { background: @dialog; }
QListWidget#history, QListWidget#upNext { background: transparent; border: none; }
QListWidget#history::item, QListWidget#upNext::item { padding: 3px 2px; }
QComboBox { background: @field; border: 1px solid @line2; border-radius: 4px; padding: 3px 8px; }
QProgressBar { background: @field; border: none; border-radius: 2px; height: 4px; text-align: center; }
QProgressBar::chunk { background: @accent; border-radius: 2px; }
QLabel#hint { color: @faint; }
QTabWidget::pane { border: 1px solid @line2; border-radius: 4px; top: -1px; background: @dialog; }
QTabBar::tab { background: transparent; color: @dim; padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QTabBar::tab:hover { color: @text; }
QComboBox QAbstractItemView { background: @field; border: 1px solid @line3; selection-background-color: @selection; outline: 0; }
QCheckBox:disabled, QLabel:disabled { color: @faint; }
QPushButton#swatch { padding: 3px; min-width: 52px; }
)CSS";
} // namespace

const QVector<Theme::Preset> &Theme::presets()
{
    static const QVector<Preset> list = {
        {QStringLiteral("dark"), QObject::tr("Dark"), QColor(0x1c, 0x1d, 0x20), QColor(0xe3, 0xe3, 0xe5), Brand},
        {QStringLiteral("light"), QObject::tr("Light"), QColor(0xf4, 0xf4, 0xf6), QColor(0x1f, 0x20, 0x24), QColor(0xe0, 0x8e, 0x0b)},
        {QStringLiteral("midnight"), QObject::tr("Midnight"), QColor(0x12, 0x15, 0x1c), QColor(0xdc, 0xe1, 0xea), QColor(0x5a, 0xa9, 0xff)},
        {QStringLiteral("nord"), QObject::tr("Frost"), QColor(0x2e, 0x34, 0x40), QColor(0xe5, 0xe9, 0xf0), QColor(0x88, 0xc0, 0xd0)},
        {QStringLiteral("forest"), QObject::tr("Forest"), QColor(0x17, 0x1d, 0x19), QColor(0xdd, 0xe6, 0xde), QColor(0x6c, 0xc3, 0x8b)},
        {QStringLiteral("plum"), QObject::tr("Plum"), QColor(0x1f, 0x1a, 0x24), QColor(0xe8, 0xe0, 0xeb), QColor(0xe8, 0x6f, 0xa3)},
        {QStringLiteral("ember"), QObject::tr("Ember"), QColor(0x1e, 0x19, 0x18), QColor(0xec, 0xe2, 0xdc), QColor(0xff, 0x6b, 0x3d)},
        {QStringLiteral("paper"), QObject::tr("Paper"), QColor(0xf6, 0xf1, 0xe7), QColor(0x2b, 0x27, 0x22), QColor(0xb8, 0x5c, 0x38)},
        {QStringLiteral("black"), QObject::tr("Black (OLED)"), QColor(0x00, 0x00, 0x00), QColor(0xe6, 0xe6, 0xe6), Brand},
    };
    return list;
}

Theme::Choice Theme::loadChoice()
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    Choice c;
    c.theme = s.value(QStringLiteral("appearance/theme"), QStringLiteral("dark")).toString();
    c.accent = QColor(s.value(QStringLiteral("appearance/accent")).toString());
    c.customWindow = QColor(s.value(QStringLiteral("appearance/customBackground"), QStringLiteral("#1c1d20")).toString());
    c.customText = QColor(s.value(QStringLiteral("appearance/customText"), QStringLiteral("#e3e3e5")).toString());
    c.file = s.value(QStringLiteral("appearance/themeFile")).toString();
    return c;
}

void Theme::saveChoice(const Choice &c)
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("appearance/theme"), c.theme);
    s.setValue(QStringLiteral("appearance/accent"), c.accent.isValid() ? hex(c.accent) : QString());
    s.setValue(QStringLiteral("appearance/themeFile"), c.file);
    if (c.customWindow.isValid())
        s.setValue(QStringLiteral("appearance/customBackground"), hex(c.customWindow));
    if (c.customText.isValid())
        s.setValue(QStringLiteral("appearance/customText"), hex(c.customText));
}

bool Theme::systemPrefersDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs != Qt::ColorScheme::Unknown)
        return cs == Qt::ColorScheme::Dark;
#endif
#ifdef RUCHE_DBUS
    // XDG desktop portal: 1 = prefer dark, 2 = prefer light, 0 = no preference.
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                    QStringLiteral("/org/freedesktop/portal/desktop"),
                                                    QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("Read"));
    m << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 1500);
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
        QVariant v = r.arguments().first();
        while (v.canConvert<QDBusVariant>() && v.userType() == qMetaTypeId<QDBusVariant>())
            v = v.value<QDBusVariant>().variant();
        const uint scheme = v.toUInt();
        if (scheme == 1 || scheme == 2)
            return scheme == 1;
    }
#endif
#ifdef Q_OS_WIN
    const QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                        QSettings::NativeFormat);
    if (reg.contains(QStringLiteral("AppsUseLightTheme")))
        return reg.value(QStringLiteral("AppsUseLightTheme")).toInt() == 0;
#endif
    return true;
}

void Theme::apply(QApplication &app, const Choice &c)
{
    const QVector<Preset> &list = presets();
    Preset p = list.first();
    if (c.theme == QLatin1String("custom") && c.customWindow.isValid() && c.customText.isValid()) {
        p.window = c.customWindow;
        p.text = c.customText;
        p.accent = Brand;
    } else if (c.theme == QLatin1String("system")) {
        p = systemPrefersDark() ? list[0] : list[1];
    } else {
        for (const Preset &x : list)
            if (x.id == c.theme)
                p = x;
    }
    // A theme file can bring its own colours; the accent picked in Settings still wins.
    const File tf = c.file.isEmpty() ? File() : fileById(c.file);
    g.file = tf.path;
    if (tf.window.isValid() && tf.text.isValid()) {
        p.window = tf.window;
        p.text = tf.text;
    }
    if (tf.accent.isValid())
        p.accent = tf.accent;
    g.window = p.window;
    g.text = p.text;
    g.accent = c.accent.isValid() ? c.accent : p.accent;
    g.dark = luminance(g.window) < luminance(g.text);
    // Accent as text: nudge it until it reads on the background (contrast 3:1 or better).
    g.accentText = g.accent;
    for (int i = 0; i < 20 && contrast(g.accentText, g.window) < 3.0; ++i)
        g.accentText = g.dark ? g.accentText.lighter(110) : g.accentText.darker(112);
    g.onAccent = contrast(QColor(0x1c, 0x1d, 0x20), g.accent) >= contrast(Qt::white, g.accent) ? QColor(0x1c, 0x1d, 0x20)
                                                                                               : QColor(Qt::white);
    ++g.generation;

    const QColor away = g.dark ? QColor(Qt::white) : QColor(Qt::black); // past the text colour
    auto shade = [](double t) { return mix(g.window, g.text, t); };
    auto ink = [](double t) { return mix(g.text, g.window, t); }; // text faded toward the background

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette pal;
    pal.setColor(QPalette::Window, g.window);
    pal.setColor(QPalette::WindowText, g.text);
    pal.setColor(QPalette::Base, shade(0.01));
    pal.setColor(QPalette::AlternateBase, shade(0.035));
    pal.setColor(QPalette::ToolTipBase, shade(0.07));
    pal.setColor(QPalette::ToolTipText, g.text);
    pal.setColor(QPalette::PlaceholderText, ink(0.4));
    pal.setColor(QPalette::Text, g.text);
    pal.setColor(QPalette::Button, shade(0.03));
    pal.setColor(QPalette::ButtonText, g.text);
    pal.setColor(QPalette::BrightText, g.accent);
    pal.setColor(QPalette::Highlight, shade(0.14));
    pal.setColor(QPalette::HighlightedText, away);
    pal.setColor(QPalette::Link, g.accentText);
    pal.setColor(QPalette::Light, shade(0.13));
    pal.setColor(QPalette::Midlight, shade(0.09));
    pal.setColor(QPalette::Mid, shade(0.08));
    pal.setColor(QPalette::Dark, deep(0.2));
    pal.setColor(QPalette::Shadow, g.dark ? QColor(Qt::black) : shade(0.3));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, ink(0.62));
    app.setPalette(pal);
    Icons::setColors(ink(0.13), away, ink(0.7), g.accent);

    const QHash<QString, QColor> tokens = {
        {QStringLiteral("window"), g.window},
        {QStringLiteral("text"), g.text},
        {QStringLiteral("strong"), mix(g.text, away, 0.4)},
        {QStringLiteral("seltext"), away},
        {QStringLiteral("icon"), ink(0.13)},
        {QStringLiteral("soft"), ink(0.3)},
        {QStringLiteral("dim"), ink(0.4)},
        {QStringLiteral("status"), ink(0.52)},
        {QStringLiteral("faint"), ink(0.6)},
        {QStringLiteral("accent"), g.accent},
        {QStringLiteral("accenttext"), g.accentText},
        {QStringLiteral("accenthover"), g.dark ? mix(g.accent, Qt::white, 0.18) : g.accent.darker(108)},
        {QStringLiteral("accentpressed"), g.accent.darker(112)},
        {QStringLiteral("onaccent"), g.onAccent},
        {QStringLiteral("deep1"), deep(0.035)},
        {QStringLiteral("deep2"), deep(0.07)},
        {QStringLiteral("deep3"), deep(0.14)},
        {QStringLiteral("deep4"), deep(0.18)},
        {QStringLiteral("base"), shade(0.01)},
        {QStringLiteral("dialog"), shade(0.015)},
        {QStringLiteral("altbase"), shade(0.035)},
        {QStringLiteral("field"), shade(0.05)},
        {QStringLiteral("raised"), shade(0.07)},
        {QStringLiteral("line"), shade(0.07)},
        {QStringLiteral("line1"), shade(0.08)},
        {QStringLiteral("navsel"), shade(0.09)},
        {QStringLiteral("line2"), shade(0.1)},
        {QStringLiteral("groove"), shade(0.105)},
        {QStringLiteral("line3"), shade(0.11)},
        {QStringLiteral("browsesel"), shade(0.11)},
        {QStringLiteral("line4"), shade(0.13)},
        {QStringLiteral("scroll"), shade(0.13)},
        {QStringLiteral("selection"), shade(0.14)},
        {QStringLiteral("scrollhover"), shade(0.19)},
    };
    QString css = QString::fromUtf8(kCss);
    if (!tf.path.isEmpty())
        css += QStringLiteral("\n/* ---- theme file: %1 ---- */\n").arg(tf.name) + fileStylesheet(tf);
    static const QRegularExpression token(QStringLiteral("@([a-z0-9]+)"));
    QString out;
    out.reserve(css.size());
    qsizetype last = 0;
    for (auto it = token.globalMatch(css); it.hasNext();) {
        const QRegularExpressionMatch mt = it.next();
        out += QStringView(css).mid(last, mt.capturedStart() - last);
        const auto f = tokens.constFind(mt.captured(1));
        out += f != tokens.constEnd() ? hex(*f) : mt.captured(0);
        last = mt.capturedEnd();
    }
    out += QStringView(css).mid(last);
#ifdef Q_OS_WIN
    // Windows has no "monospace" alias; Qt would fall back to a proportional font.
    out.replace(QLatin1String("font-family: monospace"), QLatin1String("font-family: Consolas"));
#endif
    app.setStyleSheet(out);
}

int Theme::generation() { return g.generation; }
QString Theme::activeFilePath() { return g.file; }
bool Theme::isDark() { return g.dark; }
QColor Theme::window() { return g.window; }
QColor Theme::text() { return g.text; }
QColor Theme::accent() { return g.accent; }
QColor Theme::accentText() { return g.accentText; }
QColor Theme::onAccent() { return g.onAccent; }
QColor Theme::dim() { return mix(g.text, g.window, 0.4); }
QColor Theme::faint() { return mix(g.text, g.window, 0.6); }
QColor Theme::surface() { return mix(g.window, g.text, 0.07); }
QColor Theme::mixed(double t) { return mix(g.window, g.text, t); }
