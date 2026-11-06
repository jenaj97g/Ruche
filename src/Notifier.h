// "Now playing" pop-ups: freedesktop notifications on Linux, tray toasts on Windows.
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

class QSystemTrayIcon;

class Notifier : public QObject {
    Q_OBJECT
public:
    explicit Notifier(QObject *parent = nullptr);

    // True when something can show the pop-up (a notification server, or a tray icon).
    bool available() const;
    // Windows shows the pop-ups from the tray icon, so it needs one to exist.
    static bool needsTray();
    void setTray(QSystemTrayIcon *tray);

    // Replaces the previous pop-up instead of stacking a new one per song.
    void show(const QString &summary, const QString &body, const QImage &image, bool canSkip);

Q_SIGNALS:
    void activated();     // the pop-up was clicked
    void skipRequested(); // its "Next" button was pressed

#ifdef RUCHE_DBUS
private Q_SLOTS:
    void onActionInvoked(uint id, const QString &key);
    void onClosed(uint id, uint reason);

private:
    void queryServer();
    bool m_server = false;
    bool m_actions = false;
    bool m_markup = false;
    uint m_id = 0;
#endif
private:
    QSystemTrayIcon *m_tray = nullptr;
};
