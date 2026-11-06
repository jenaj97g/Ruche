#include "DesktopEntry.h"
#include "Icons.h"
#include "Library.h"
#include "MainWindow.h"
#include "Mpris.h"
#include "PlaybackController.h"
#include "Stations.h"
#include "I18n.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QImage>
#include <QSessionManager>
#include <QPainter>
#include <QTimer>

#ifdef RUCHE_DBUS
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#else
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <windows.h>
#endif

static QStringList absoluteArgs(const QStringList &args)
{
    QStringList absolute;
    for (const QString &a : args) {
        const QUrl u(a);
        absolute << ((u.scheme().size() > 1 && !QFileInfo::exists(a)) ? a : QFileInfo(a).absoluteFilePath());
    }
    return absolute;
}

#ifdef RUCHE_DBUS
// If Ruche is already running, hand it this launch (files to open, or a transport action)
// so a taskbar click raises the existing window instead of starting a second player.
static bool forwardToRunningInstance(const QStringList &args, const QString &action)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString service = QStringLiteral("org.mpris.MediaPlayer2.ruche");
    if (!bus.isConnected() || !bus.interface()->isServiceRegistered(service))
        return false;
    const QString path = QStringLiteral("/org/mpris/MediaPlayer2");
    if (!action.isEmpty()) {
        bus.call(QDBusMessage::createMethodCall(service, path, QStringLiteral("org.mpris.MediaPlayer2.Player"), action), QDBus::Block, 3000);
        return true;
    }
    QDBusMessage m = QDBusMessage::createMethodCall(service, path, QStringLiteral("fr.ruche.App"), QStringLiteral("Open"));
    m << absoluteArgs(args);
    const QDBusMessage reply = bus.call(m, QDBus::Block, 3000);
    return reply.type() == QDBusMessage::ReplyMessage;
}
#else
// Without D-Bus a second launch talks to the first one over a local pipe, one JSON line:
// {"action":"PlayPause"} or {"open":[files...]}.
static QString pipeName()
{
    return QStringLiteral("ruche-") + qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER", QStringLiteral("user")));
}

static bool forwardToRunningInstance(const QStringList &args, const QString &action)
{
    QLocalSocket sock;
    sock.connectToServer(pipeName());
    if (!sock.waitForConnected(1500))
        return false;
#ifdef Q_OS_WIN
    // Let the running Ruche bring its window to the front (focus-stealing prevention
    // otherwise only flashes its taskbar button).
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    QJsonObject o;
    if (!action.isEmpty())
        o.insert(QStringLiteral("action"), action);
    else
        o.insert(QStringLiteral("open"), QJsonArray::fromStringList(absoluteArgs(args)));
    sock.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    sock.waitForBytesWritten(2000);
    sock.waitForReadyRead(2000); // the reply only says it arrived
    return true;
}

static void listenForOtherInstances(QObject *owner, PlaybackController *pc, Mpris *mpris)
{
    auto *server = new QLocalServer(owner);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(pipeName())) {
        QLocalServer::removeServer(pipeName()); // stale socket left by a crash (not an issue on Windows)
        server->listen(pipeName());
    }
    QObject::connect(server, &QLocalServer::newConnection, server, [server, pc, mpris] {
        while (QLocalSocket *c = server->nextPendingConnection()) {
            QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
            QObject::connect(c, &QLocalSocket::readyRead, c, [c, pc, mpris] {
                if (!c->canReadLine())
                    return;
                const QJsonObject o = QJsonDocument::fromJson(c->readLine()).object();
                c->write("ok\n");
                c->flush();
                const QString action = o.value(QStringLiteral("action")).toString();
                if (action == QLatin1String("PlayPause"))
                    pc->togglePause();
                else if (action == QLatin1String("Next"))
                    pc->next();
                else if (action == QLatin1String("Previous"))
                    pc->previous();
                else if (o.contains(QStringLiteral("open"))) {
                    QStringList args;
                    for (const QJsonValue &v : o.value(QStringLiteral("open")).toArray())
                        args << v.toString();
                    Q_EMIT mpris->openArgsRequested(args);
                    Q_EMIT mpris->raiseRequested();
                }
            });
        }
    });
}
#endif

#ifdef Q_OS_WIN
// Play/pause, next, previous and stop keys work while another window has the focus.
class MediaKeys : public QAbstractNativeEventFilter {
public:
    explicit MediaKeys(PlaybackController *pc) : m_pc(pc)
    {
        const UINT keys[] = {VK_MEDIA_PLAY_PAUSE, VK_MEDIA_NEXT_TRACK, VK_MEDIA_PREV_TRACK, VK_MEDIA_STOP};
        for (int i = 0; i < 4; ++i)
            m_ok[i] = RegisterHotKey(nullptr, 0xB000 + i, MOD_NOREPEAT, keys[i]);
    }
    ~MediaKeys() override
    {
        for (int i = 0; i < 4; ++i)
            if (m_ok[i])
                UnregisterHotKey(nullptr, 0xB000 + i);
    }
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
    {
        const MSG *m = static_cast<const MSG *>(message);
        if (m->message != WM_HOTKEY || m->hwnd != nullptr || m->wParam < 0xB000 || m->wParam > 0xB003)
            return false;
        switch (m->wParam - 0xB000) {
        case 0: m_pc->togglePause(); break;
        case 1: m_pc->next(); break;
        case 2: m_pc->previous(); break;
        case 3: m_pc->stop(); break;
        }
        return true;
    }

private:
    PlaybackController *m_pc;
    bool m_ok[4] = {};
};
#endif

static QtMessageHandler g_defaultHandler = nullptr;
static void messageFilter(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    // Ruche draws with plain shared-memory buffers on Wayland and ships without the EGL
    // buffer plugin to save memory; Qt warns about it on every start.
    if (msg.startsWith(QLatin1String("Failed to load client buffer integration")) ||
        msg.startsWith(QLatin1String("Available client buffer integrations")))
        return;
    g_defaultHandler(type, ctx, msg);
    // A Windows GUI program has no terminal to print to: RUCHE_LOG_FILE=path collects
    // the same messages (with RUCHE_DEBUG=1 / RUCHE_MPV_LOG=v for more detail).
    static QFile *logFile = [] {
        const QString path = qEnvironmentVariable("RUCHE_LOG_FILE");
        auto *f = path.isEmpty() ? nullptr : new QFile(path);
        if (f && !f->open(QIODevice::Append | QIODevice::Text)) {
            delete f;
            f = nullptr;
        }
        return f;
    }();
    if (logFile) {
        logFile->write(qFormatLogMessage(type, ctx, msg).toUtf8() + '\n');
        logFile->flush();
    }
}

int main(int argc, char **argv)
{
    g_defaultHandler = qInstallMessageHandler(messageFilter);

#ifndef Q_OS_WIN
    // Commands that never open a window must also work without a display (TTY, SSH,
    // scripts, the launcher's right-click actions under any session).
    {
        static const char *const headless[] = {"--install-desktop-entry", "--remove-desktop-entry", "--play-pause", "--next",
                                               "--previous", "--render-icon", "--help", "-h", "--help-all", "--version", "-v"};
        for (int i = 1; i < argc; ++i)
            for (const char *flag : headless)
                if (qstrcmp(argv[i], flag) == 0)
                    qputenv("QT_QPA_PLATFORM", "minimal"); // overrides the launcher's xcb/wayland pick
    }
#endif
#ifdef __GLIBC__
    // Every thread that allocates would otherwise get its own malloc arena; mpv, the scanner
    // and the art loader would each keep megabytes of free-but-held memory around.
    mallopt(M_ARENA_MAX, 2);
#endif

    QApplication::setApplicationName(QStringLiteral("ruche"));
    QApplication::setApplicationVersion(QStringLiteral(RUCHE_VERSION));
    // Inside Flatpak the menu entry, icon and Wayland app id are the Flatpak app id.
    QApplication::setDesktopFileName(qEnvironmentVariable("FLATPAK_ID", QStringLiteral("ruche")));
    QApplication app(argc, argv);
    i18n::install(app);
    Theme::apply(app, Theme::loadChoice());

    // Packaging helper: ruche --render-icon file.png size
    if (argc == 4 && qstrcmp(argv[1], "--render-icon") == 0) {
        const int size = QByteArray(argv[3]).toInt();
        QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        Icons::paint(&p, QRectF(0, 0, size, size), Icons::Logo, Theme::Brand);
        p.end();
        return img.save(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
    }

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Ruche: a light music player with a MusicBee-style layout."));
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Audio files, folders, playlists or stream URLs."), QStringLiteral("[files...]"));
    const QCommandLineOption mpvOpt(QStringLiteral("mpv-opt"), QStringLiteral("Extra mpv option (key=value), repeatable."), QStringLiteral("opt"));
    const QCommandLineOption titleLog(QStringLiteral("title-log"), QStringLiteral("Append every stream title change to this file."), QStringLiteral("file"));
    const QCommandLineOption quitAfter(QStringLiteral("quit-after"), QStringLiteral("Quit after N seconds (testing)."), QStringLiteral("seconds"));
    const QCommandLineOption shot(QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot (testing)."), QStringLiteral("file"));
    const QCommandLineOption shotDelay(QStringLiteral("screenshot-delay"), QStringLiteral("Delay before the screenshot, ms."), QStringLiteral("ms"), QStringLiteral("2500"));
    const QCommandLineOption view(QStringLiteral("view"), QStringLiteral("Start on: music, radio, queue."), QStringLiteral("name"));
    const QCommandLineOption playPause(QStringLiteral("play-pause"), QStringLiteral("Toggle play/pause in the running Ruche."));
    const QCommandLineOption nextOpt(QStringLiteral("next"), QStringLiteral("Skip to the next track in the running Ruche."));
    const QCommandLineOption prevOpt(QStringLiteral("previous"), QStringLiteral("Go to the previous track in the running Ruche."));
    const QCommandLineOption newInstance(QStringLiteral("new-instance"), QStringLiteral("Start a separate player even if one is running."));
    const QCommandLineOption installEntry(QStringLiteral("install-desktop-entry"), QStringLiteral("Add Ruche (this AppImage) to the applications menu, then exit."));
    const QCommandLineOption removeEntry(QStringLiteral("remove-desktop-entry"), QStringLiteral("Remove Ruche from the applications menu, then exit."));
    cli.addOptions({mpvOpt, titleLog, quitAfter, shot, shotDelay, view, playPause, nextOpt, prevOpt, newInstance,
                    installEntry, removeEntry});
    cli.process(app);

    if (cli.isSet(installEntry) || cli.isSet(removeEntry)) {
        QString err;
        const bool ok = cli.isSet(installEntry) ? DesktopEntry::install(&err) : DesktopEntry::remove(&err);
        if (!ok)
            qCritical("%s", qPrintable(err));
        return ok ? 0 : 1;
    }
    QString action;
    if (cli.isSet(playPause))
        action = QStringLiteral("PlayPause");
    else if (cli.isSet(nextOpt))
        action = QStringLiteral("Next");
    else if (cli.isSet(prevOpt))
        action = QStringLiteral("Previous");
    if (!cli.isSet(newInstance) && forwardToRunningInstance(cli.positionalArguments(), action))
        return 0;
    if (!action.isEmpty())
        return 0; // nothing running to control
    DesktopEntry::refreshIfInstalled();

    Library lib;
    lib.open();
    Stations stations;
    stations.load();
    PlaybackController pc(&lib, cli.values(mpvOpt));
    if (cli.isSet(titleLog))
        pc.setTitleLog(cli.value(titleLog));

    MainWindow w(&lib, &pc, &stations);
    Mpris mpris(&pc, &lib);
    w.setMpris(&mpris);
    QObject::connect(&mpris, &Mpris::raiseRequested, &w, [&w] {
        w.showFromTray();
        w.showNormal();
        w.raise();
        w.activateWindow();
    });
    QObject::connect(&mpris, &Mpris::openRequested, &w, [&w](const QString &uri) { w.openArguments({uri}); });
    QObject::connect(&mpris, &Mpris::openArgsRequested, &w, [&w](const QStringList &a) {
        if (!a.isEmpty())
            w.openArguments(a);
    });
    QObject::connect(&app, &QApplication::aboutToQuit, &w, [&w] {
        w.setQuitting();
        w.close();
    });
    // Logging out must close Ruche even with close-to-tray on.
    QObject::connect(&app, &QGuiApplication::commitDataRequest, &w, [&w](QSessionManager &) { w.setQuitting(); });
#ifndef RUCHE_DBUS
    listenForOtherInstances(&w, &pc, &mpris);
#endif
#ifdef Q_OS_WIN
    MediaKeys mediaKeys(&pc);
    app.installNativeEventFilter(&mediaKeys);
#endif
    w.show();

    if (cli.value(view) == QLatin1String("radio"))
        w.showRadio();
    const QStringList args = cli.positionalArguments();
    if (!args.isEmpty())
        QTimer::singleShot(0, &w, [&w, args] { w.openArguments(args); });
    if (cli.isSet(shot)) {
        const QString file = cli.value(shot);
        QTimer::singleShot(cli.value(shotDelay).toInt(), &w, [&w, file] { w.grab().save(file); });
    }
#ifdef __GLIBC__
    // Hand freed heap back to the system now and then (after scans, cover loads, track changes).
    QTimer trim;
    QObject::connect(&trim, &QTimer::timeout, [] { malloc_trim(0); });
    trim.start(30000);
    QObject::connect(&lib, &Library::reloaded, [] { QTimer::singleShot(2000, [] { malloc_trim(0); }); });
    QTimer::singleShot(4000, [] { malloc_trim(0); });
#endif

    if (cli.isSet(quitAfter))
        QTimer::singleShot(int(cli.value(quitAfter).toDouble() * 1000), &app, &QApplication::quit);
    return app.exec();
}
