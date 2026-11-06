#include "Notifier.h"

#include <QGuiApplication>
#include <QIcon>
#include <QPixmap>
#include <QSystemTrayIcon>

#ifdef RUCHE_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusMessage>

namespace {
const char *const kService = "org.freedesktop.Notifications";
const char *const kPath = "/org/freedesktop/Notifications";
const char *const kIface = "org.freedesktop.Notifications";

// The spec's raw image hint: (iiibiiay) = width, height, rowstride, has alpha,
// bits per sample, channels, RGB(A) bytes. Sent inline so the server never has to
// read a temporary file (sandboxed servers can't, and the file may be gone by then).
struct NotifyImage {
    int width = 0, height = 0, stride = 0;
    bool alpha = true;
    int bits = 8, channels = 4;
    QByteArray data;
};
} // namespace
Q_DECLARE_METATYPE(NotifyImage)

static QDBusArgument &operator<<(QDBusArgument &a, const NotifyImage &i)
{
    a.beginStructure();
    a << i.width << i.height << i.stride << i.alpha << i.bits << i.channels << i.data;
    a.endStructure();
    return a;
}
static const QDBusArgument &operator>>(const QDBusArgument &a, NotifyImage &i)
{
    a.beginStructure();
    a >> i.width >> i.height >> i.stride >> i.alpha >> i.bits >> i.channels >> i.data;
    a.endStructure();
    return a;
}

static NotifyImage toNotifyImage(const QImage &src)
{
    NotifyImage n;
    QImage img = src.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
    n.width = img.width();
    n.height = img.height();
    n.stride = int(img.bytesPerLine());
    n.data = QByteArray(reinterpret_cast<const char *>(img.constBits()), int(img.sizeInBytes()));
    return n;
}

Notifier::Notifier(QObject *parent) : QObject(parent)
{
    qDBusRegisterMetaType<NotifyImage>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface), QStringLiteral("ActionInvoked"), this,
                SLOT(onActionInvoked(uint, QString)));
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface), QStringLiteral("NotificationClosed"), this,
                SLOT(onClosed(uint, uint)));
    queryServer();
}

void Notifier::queryServer()
{
    // Asynchronous, so a slow or D-Bus-activated server never holds up the window.
    const QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                          QLatin1String(kIface), QStringLiteral("GetCapabilities"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m, 5000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<QStringList> r = *w;
        w->deleteLater();
        if (r.isError())
            return;
        m_server = true;
        const QStringList caps = r.value();
        m_actions = caps.contains(QLatin1String("actions"));
        m_markup = caps.contains(QLatin1String("body-markup"));
    });
}

bool Notifier::available() const { return m_server || m_tray; }
bool Notifier::needsTray() { return false; }
void Notifier::setTray(QSystemTrayIcon *tray) { m_tray = tray; }

void Notifier::show(const QString &summary, const QString &body, const QImage &image, bool canSkip)
{
    if (!m_server) {
        // No notification server answered (yet): fall back to the tray, if there is one.
        if (m_tray)
            m_tray->showMessage(summary, body, image.isNull() ? QIcon() : QIcon(QPixmap::fromImage(image)), 5000);
        return;
    }
    QStringList actions;
    if (m_actions) {
        actions << QStringLiteral("default") << tr("Show Ruche");
        if (canSkip)
            actions << QStringLiteral("next") << tr("Next");
    }
    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"), QGuiApplication::desktopFileName());
    hints.insert(QStringLiteral("category"), QStringLiteral("x-gnome.music"));
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue(uchar(0)));
    // Song changes are passing news: keep them out of the notification history.
    hints.insert(QStringLiteral("transient"), true);
    hints.insert(QStringLiteral("x-kde-origin-name"), QStringLiteral("Ruche"));
    if (!image.isNull())
        hints.insert(QStringLiteral("image-data"), QVariant::fromValue(toNotifyImage(image)));

    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
                                                    QStringLiteral("Notify"));
    m << QStringLiteral("Ruche") << m_id << QGuiApplication::desktopFileName() << summary
      << (m_markup ? body.toHtmlEscaped() : body) << actions << hints << int(5000);
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m, 5000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<uint> r = *w;
        w->deleteLater();
        if (r.isError()) {
            m_server = false;
            queryServer(); // the server went away; look again before the next song
            return;
        }
        m_id = r.value();
    });
}

void Notifier::onActionInvoked(uint id, const QString &key)
{
    if (id == 0 || id != m_id)
        return;
    if (key == QLatin1String("next"))
        Q_EMIT skipRequested();
    else
        Q_EMIT activated();
}

void Notifier::onClosed(uint id, uint)
{
    // A closed pop-up can't be replaced any more; the next song gets a fresh one.
    if (id == m_id)
        m_id = 0;
}

#else // tray toasts (Windows: shown as a Windows notification)

Notifier::Notifier(QObject *parent) : QObject(parent) {}
bool Notifier::available() const { return m_tray != nullptr; }
bool Notifier::needsTray() { return true; }

void Notifier::setTray(QSystemTrayIcon *tray)
{
    if (m_tray == tray)
        return;
    m_tray = tray;
    if (m_tray)
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, &Notifier::activated);
}

void Notifier::show(const QString &summary, const QString &body, const QImage &image, bool)
{
    if (!m_tray)
        return;
    QIcon icon;
    if (!image.isNull())
        icon = QIcon(QPixmap::fromImage(image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    m_tray->showMessage(summary, body, icon, 5000);
}

#endif
