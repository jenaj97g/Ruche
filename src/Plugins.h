// Lua plugins. Each plugin is a folder (main.lua + optional plugin.json) or a single .lua
// file in the plugins folder. It runs in its own Lua state on the main thread, reacts to
// events (track, title, state, ...), and can control playback, show notifications, add menu
// actions, run timers and make HTTP requests. The API is documented in PLUGINS.md.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <functional>

struct lua_State;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class PlaybackController;
class Library;
class Stations;

class PluginHost : public QObject {
    Q_OBJECT
public:
    // One entry of plugin.json's "settings": shown as a form in Settings → Plugins.
    struct Setting {
        QString key, label, type, help; // type: string, password, bool, int, choice
        QVariant def;
        QStringList choices;
        int min = 0, max = 100000;
    };
    struct Plugin {
        QString id;          // folder or file name
        QString name, version, description, author;
        QString dir;         // the plugin's folder (or the folder holding the .lua file)
        QString script;      // the Lua file to run
        bool bundled = false; // shipped with Ruche (read-only)
        QVector<Setting> settings;
        bool enabled = false;
        QString error;       // last load or runtime error
        QStringList log;     // recent print()/ruche.log() lines
        lua_State *L = nullptr;
        QHash<QString, QVector<int>> handlers; // event -> Lua function refs
        QHash<int, QTimer *> timers;           // id -> timer (its callback ref is the id)
        QVector<QPointer<QNetworkReply>> requests;
        QVector<QPair<QString, int>> actions;  // menu label, function ref
    };

    PluginHost(PlaybackController *pc, Library *lib, Stations *stations, QObject *parent = nullptr);
    ~PluginHost() override;

    static QString userDir();     // where users put plugins
    static QString bundledDir();  // examples shipped with Ruche
    static QString dataDirFor(const QString &id); // plugin-private storage

    void scan();        // finds plugins, starts the enabled ones
    void reloadAll();
    void setEnabled(const QString &id, bool on);
    const QVector<Plugin *> &plugins() const { return m_plugins; }
    Plugin *find(const QString &id) const;

    QJsonObject config(const QString &id) const;
    void setConfig(const QString &id, const QJsonObject &values); // saves; fires "config"

    struct Action {
        QString plugin, label;
        int ref;
    };
    QVector<Action> actions() const;
    void runAction(const QString &plugin, int ref);

    // Sent to every running plugin: "track", "title", "state", "station", "quit", ...
    void emitEvent(const QString &event, const QVariant &payload = QVariant());

    // Hooks MainWindow fills in, so plugins can reach the interface.
    std::function<void(const QString &summary, const QString &body)> notifyHook;
    std::function<void(const QString &text)> flashHook;
    std::function<void()> raiseHook;

Q_SIGNALS:
    void changed(); // list, enabled state, errors or actions changed

private:
    void start(Plugin *p);
    void stop(Plugin *p);
    void fail(Plugin *p, const QString &where, const QString &message);
    void installApi(Plugin *p);
    bool call(Plugin *p, int nargs, const QString &where); // pops the function and args
    void appendLog(Plugin *p, const QString &line);

    friend struct PluginApi;
    PlaybackController *m_pc;
    Library *m_lib;
    Stations *m_stations;
    QNetworkAccessManager *m_nam = nullptr;
    QVector<Plugin *> m_plugins;
    int m_nextTimer = 1;
    QString m_lastState;
};
