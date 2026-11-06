#include "Plugins.h"

#include "Library.h"
#include "NowPlaying.h"
#include "PlaybackController.h"
#include "Stations.h"

extern "C" {
#include "3rdparty/lua/lauxlib.h"
#include "3rdparty/lua/lua.h"
#include "3rdparty/lua/lualib.h"
}

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>

using Plugin = PluginHost::Plugin;

namespace {
PluginHost *g_host = nullptr;
QElapsedTimer g_clock;
constexpr int kTimeLimitMs = 3000;

Plugin *pluginOf(lua_State *L) { return *static_cast<Plugin **>(lua_getextraspace(L)); }

// A plugin stuck in a loop would freeze the whole player: stop any call that runs too long.
void watchdog(lua_State *L, lua_Debug *)
{
    if (g_clock.isValid() && g_clock.elapsed() > kTimeLimitMs)
        luaL_error(L, "took longer than %d seconds and was stopped", kTimeLimitMs / 1000);
}

QString str(lua_State *L, int idx)
{
    size_t n = 0;
    const char *s = luaL_tolstring(L, idx, &n);
    QString out = QString::fromUtf8(s, qsizetype(n));
    lua_pop(L, 1);
    return out;
}

void pushVariant(lua_State *L, const QVariant &v, int depth = 0);

void pushMap(lua_State *L, const QVariantMap &m, int depth)
{
    lua_createtable(L, 0, int(m.size()));
    for (auto it = m.cbegin(); it != m.cend(); ++it) {
        lua_pushstring(L, it.key().toUtf8().constData());
        pushVariant(L, it.value(), depth + 1);
        lua_settable(L, -3);
    }
}

void pushVariant(lua_State *L, const QVariant &v, int depth)
{
    if (depth > 32 || !v.isValid() || v.isNull()) {
        lua_pushnil(L);
        return;
    }
    switch (v.typeId()) {
    case QMetaType::Bool:
        lua_pushboolean(L, v.toBool());
        return;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
        lua_pushinteger(L, lua_Integer(v.toLongLong()));
        return;
    case QMetaType::Double:
    case QMetaType::Float:
        lua_pushnumber(L, v.toDouble());
        return;
    case QMetaType::QVariantMap:
        pushMap(L, v.toMap(), depth);
        return;
    case QMetaType::QVariantHash: {
        const QVariantHash h = v.toHash();
        QVariantMap m;
        for (auto it = h.cbegin(); it != h.cend(); ++it)
            m.insert(it.key(), it.value());
        pushMap(L, m, depth);
        return;
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        const QVariantList l = v.toList();
        lua_createtable(L, int(l.size()), 0);
        for (int i = 0; i < l.size(); ++i) {
            pushVariant(L, l.at(i), depth + 1);
            lua_rawseti(L, -2, i + 1);
        }
        return;
    }
    case QMetaType::QJsonObject:
        pushMap(L, v.toJsonObject().toVariantMap(), depth);
        return;
    case QMetaType::QJsonArray:
        pushVariant(L, v.toJsonArray().toVariantList(), depth);
        return;
    case QMetaType::QJsonValue:
        pushVariant(L, v.toJsonValue().toVariant(), depth);
        return;
    default:
        lua_pushstring(L, v.toString().toUtf8().constData());
    }
}

QVariant toVariant(lua_State *L, int idx, int depth = 0)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        return bool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            return qint64(lua_tointeger(L, idx));
        return lua_tonumber(L, idx);
    case LUA_TSTRING: {
        size_t n = 0;
        const char *s = lua_tolstring(L, idx, &n);
        return QString::fromUtf8(s, qsizetype(n));
    }
    case LUA_TTABLE: {
        if (depth > 32)
            return QVariant();
        // 1..n with nothing else is a list; anything else is a map.
        const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
        bool isList = true;
        lua_Integer count = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            ++count;
            if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n)
                isList = false;
            lua_pop(L, 1);
        }
        if (isList && count == n && n > 0) {
            QVariantList l;
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, idx, i);
                l << toVariant(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return l;
        }
        QVariantMap m;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            lua_pushvalue(L, -2);
            const QString key = QString::fromUtf8(lua_tostring(L, -1));
            lua_pop(L, 1);
            m.insert(key, toVariant(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return m;
    }
    default:
        return QVariant();
    }
}

int ref(lua_State *L, int idx)
{
    lua_pushvalue(L, idx);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

QString configFileFor(const QString &id) { return PluginHost::dataDirFor(id) + QStringLiteral("/config.json"); }
} // namespace

// The functions plugins call, as ruche.<name>.
struct PluginApi {
    static PluginHost *host() { return g_host; }

    static int on(lua_State *L)
    {
        const QString event = QString::fromUtf8(luaL_checkstring(L, 1));
        luaL_checktype(L, 2, LUA_TFUNCTION);
        pluginOf(L)->handlers[event].append(ref(L, 2));
        return 0;
    }
    static int now(lua_State *L)
    {
        pushVariant(L, nowplaying::snapshot(host()->m_pc, host()->m_lib, host()->m_stations));
        return 1;
    }
    static int state(lua_State *L)
    {
        lua_pushstring(L, nowplaying::stateName(host()->m_pc).toUtf8().constData());
        return 1;
    }
    static int position(lua_State *L)
    {
        lua_pushnumber(L, host()->m_pc->position());
        return 1;
    }
    static int history(lua_State *L)
    {
        const auto &h = host()->m_pc->history();
        lua_createtable(L, int(h.size()), 0);
        for (int i = 0; i < h.size(); ++i) {
            QVariantMap m;
            m.insert(QStringLiteral("title"), h.at(i).title);
            m.insert(QStringLiteral("time"), h.at(i).when.toSecsSinceEpoch());
            pushVariant(L, m);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    static int playPause(lua_State *) { host()->m_pc->togglePause(); return 0; }
    static int play(lua_State *) { host()->m_pc->play(); return 0; }
    static int pause(lua_State *) { host()->m_pc->pause(); return 0; }
    static int stopPlayback(lua_State *) { host()->m_pc->stop(); return 0; }
    static int next(lua_State *) { host()->m_pc->next(); return 0; }
    static int previous(lua_State *) { host()->m_pc->previous(); return 0; }
    static int seek(lua_State *L)
    {
        host()->m_pc->seek(luaL_checknumber(L, 1));
        return 0;
    }
    static int volume(lua_State *L)
    {
        if (!lua_isnoneornil(L, 1))
            host()->m_pc->setVolume(int(std::clamp<lua_Integer>(luaL_checkinteger(L, 1), 0, 100)));
        lua_pushinteger(L, host()->m_pc->volume());
        return 1;
    }
    static int playStation(lua_State *L)
    {
        const QUrl url(QString::fromUtf8(luaL_checkstring(L, 1)));
        if (!url.isValid() || url.scheme().isEmpty())
            return luaL_argerror(L, 1, "not a stream address");
        QString name = lua_isnoneornil(L, 2) ? QString() : QString::fromUtf8(luaL_checkstring(L, 2));
        if (name.isEmpty()) {
            const int i = host()->m_stations->indexOfUrl(url.toString());
            name = i >= 0 ? host()->m_stations->list().at(i).name : url.host();
        }
        host()->m_pc->playStation(name, url);
        return 0;
    }
    static int stations(lua_State *L)
    {
        const auto &list = host()->m_stations->list();
        lua_createtable(L, int(list.size()), 0);
        for (int i = 0; i < list.size(); ++i) {
            QVariantMap m;
            m.insert(QStringLiteral("name"), list.at(i).name);
            m.insert(QStringLiteral("url"), list.at(i).url);
            if (!list.at(i).lastTitle.isEmpty())
                m.insert(QStringLiteral("last_title"), list.at(i).lastTitle);
            pushVariant(L, m);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    static int notify(lua_State *L)
    {
        const QString summary = str(L, 1);
        const QString body = lua_isnoneornil(L, 2) ? QString() : str(L, 2);
        if (host()->notifyHook)
            host()->notifyHook(summary, body);
        return 0;
    }
    static int flash(lua_State *L)
    {
        if (host()->flashHook)
            host()->flashHook(str(L, 1));
        return 0;
    }
    static int raise(lua_State *)
    {
        if (host()->raiseHook)
            host()->raiseHook();
        return 0;
    }
    static int log(lua_State *L)
    {
        QStringList parts;
        for (int i = 1; i <= lua_gettop(L); ++i)
            parts << str(L, i);
        host()->appendLog(pluginOf(L), parts.join(QLatin1Char(' ')));
        return 0;
    }
    static int addAction(lua_State *L)
    {
        const QString label = QString::fromUtf8(luaL_checkstring(L, 1));
        luaL_checktype(L, 2, LUA_TFUNCTION);
        pluginOf(L)->actions.append({label, ref(L, 2)});
        Q_EMIT host()->changed();
        return 0;
    }
    static int timer(lua_State *L, bool repeat)
    {
        const lua_Integer ms = luaL_checkinteger(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        Plugin *p = pluginOf(L);
        const int fn = ref(L, 2);
        const int id = host()->m_nextTimer++;
        auto *t = new QTimer(host());
        t->setSingleShot(!repeat);
        t->setInterval(int(std::clamp<lua_Integer>(ms, repeat ? 50 : 0, 24 * 3600 * 1000)));
        QObject::connect(t, &QTimer::timeout, host(), [p, id, fn, repeat] {
            if (!p->L || !p->timers.contains(id))
                return;
            QTimer *self = p->timers.value(id);
            if (!repeat) {
                p->timers.remove(id);
                self->deleteLater();
            }
            lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
            if (!repeat)
                luaL_unref(p->L, LUA_REGISTRYINDEX, fn);
            g_host->call(p, 0, QStringLiteral("timer"));
        });
        t->setProperty("ruche_ref", fn);
        p->timers.insert(id, t);
        t->start();
        lua_pushinteger(L, id);
        return 1;
    }
    static int after(lua_State *L) { return timer(L, false); }
    static int every(lua_State *L) { return timer(L, true); }
    static int cancel(lua_State *L)
    {
        Plugin *p = pluginOf(L);
        const int id = int(luaL_checkinteger(L, 1));
        if (QTimer *t = p->timers.take(id)) {
            t->stop();
            luaL_unref(L, LUA_REGISTRYINDEX, t->property("ruche_ref").toInt());
            t->deleteLater();
        }
        return 0;
    }
    static int jsonEncode(lua_State *L)
    {
        const QVariant v = toVariant(L, 1);
        QByteArray out;
        if (v.typeId() == QMetaType::QVariantList)
            out = QJsonDocument(QJsonArray::fromVariantList(v.toList())).toJson(QJsonDocument::Compact);
        else if (v.typeId() == QMetaType::QVariantMap)
            out = QJsonDocument(QJsonObject::fromVariantMap(v.toMap())).toJson(QJsonDocument::Compact);
        else {
            QJsonArray wrap;
            wrap.append(QJsonValue::fromVariant(v));
            out = QJsonDocument(wrap).toJson(QJsonDocument::Compact);
            out = out.mid(1, out.size() - 2);
        }
        lua_pushlstring(L, out.constData(), size_t(out.size()));
        return 1;
    }
    static int jsonDecode(lua_State *L)
    {
        size_t n = 0;
        const char *s = luaL_checklstring(L, 1, &n);
        QJsonParseError err;
        const QByteArray text(s, qsizetype(n));
        const QJsonDocument d = QJsonDocument::fromJson(text, &err);
        if (err.error != QJsonParseError::NoError) {
            // a bare value ("12", "\"x\"", "true")
            const QJsonDocument w = QJsonDocument::fromJson("[" + text + "]", &err);
            if (err.error != QJsonParseError::NoError) {
                lua_pushnil(L);
                lua_pushstring(L, err.errorString().toUtf8().constData());
                return 2;
            }
            pushVariant(L, w.array().first().toVariant());
            return 1;
        }
        pushVariant(L, d.isArray() ? QVariant(d.array().toVariantList()) : QVariant(d.object().toVariantMap()));
        return 1;
    }
    static int config(lua_State *L)
    {
        Plugin *p = pluginOf(L);
        QJsonObject c = host()->config(p->id);
        if (lua_isnoneornil(L, 1)) {
            pushVariant(L, c.toVariantMap());
            return 1;
        }
        const QString key = QString::fromUtf8(luaL_checkstring(L, 1));
        if (c.contains(key))
            pushVariant(L, c.value(key).toVariant());
        else
            lua_pushvalue(L, 2); // the default given, or nil
        return 1;
    }
    static int setConfig(lua_State *L)
    {
        Plugin *p = pluginOf(L);
        const QString key = QString::fromUtf8(luaL_checkstring(L, 1));
        QJsonObject c = host()->config(p->id);
        if (lua_isnoneornil(L, 2))
            c.remove(key);
        else
            c.insert(key, QJsonValue::fromVariant(toVariant(L, 2)));
        // saved without firing "config": that event is for changes made in Settings
        QDir().mkpath(PluginHost::dataDirFor(p->id));
        QSaveFile f(configFileFor(p->id));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(c).toJson());
            f.commit();
        }
        return 0;
    }
    static int http(lua_State *L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        Plugin *p = pluginOf(L);
        const QVariantMap o = toVariant(L, 1).toMap();
        const QUrl url(o.value(QStringLiteral("url")).toString());
        if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
            return luaL_argerror(L, 1, "url must be an http(s) address");
        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " (plugin)");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(o.value(QStringLiteral("timeout"), 15000).toInt());
        const QVariantMap headers = o.value(QStringLiteral("headers")).toMap();
        for (auto it = headers.cbegin(); it != headers.cend(); ++it)
            req.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
        QByteArray body;
        if (o.contains(QStringLiteral("json"))) {
            const QVariant j = o.value(QStringLiteral("json"));
            body = j.typeId() == QMetaType::QVariantList
                       ? QJsonDocument(QJsonArray::fromVariantList(j.toList())).toJson(QJsonDocument::Compact)
                       : QJsonDocument(QJsonObject::fromVariantMap(j.toMap())).toJson(QJsonDocument::Compact);
            if (!headers.contains(QStringLiteral("Content-Type")))
                req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        } else if (o.contains(QStringLiteral("body"))) {
            body = o.value(QStringLiteral("body")).toString().toUtf8();
        }
        const QByteArray method = o.value(QStringLiteral("method"), body.isEmpty() ? QStringLiteral("GET") : QStringLiteral("POST"))
                                      .toString()
                                      .toUpper()
                                      .toUtf8();
        if (!host()->m_nam)
            host()->m_nam = new QNetworkAccessManager(host());
        QNetworkReply *reply = host()->m_nam->sendCustomRequest(req, method, body);
        const int fn = ref(L, 2);
        p->requests.append(reply);
        QObject::connect(reply, &QNetworkReply::finished, host(), [p, reply, fn] {
            reply->deleteLater();
            if (!p->L || !p->requests.removeOne(reply))
                return; // plugin stopped meanwhile
            QVariantMap r;
            r.insert(QStringLiteral("status"), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
            const QByteArray data = reply->readAll();
            r.insert(QStringLiteral("body"), QString::fromUtf8(data));
            QVariantMap hdrs;
            for (const auto &h : reply->rawHeaderPairs())
                hdrs.insert(QString::fromLatin1(h.first).toLower(), QString::fromUtf8(h.second));
            r.insert(QStringLiteral("headers"), hdrs);
            if (reply->error() != QNetworkReply::NoError)
                r.insert(QStringLiteral("error"), reply->errorString());
            if (hdrs.value(QStringLiteral("content-type")).toString().contains(QLatin1String("json"))) {
                const QJsonDocument d = QJsonDocument::fromJson(data);
                if (!d.isNull())
                    r.insert(QStringLiteral("json"), d.isArray() ? QVariant(d.array().toVariantList()) : QVariant(d.object().toVariantMap()));
            }
            lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
            luaL_unref(p->L, LUA_REGISTRYINDEX, fn);
            pushVariant(p->L, r);
            g_host->call(p, 1, QStringLiteral("http callback"));
        });
        return 0;
    }
};

PluginHost::PluginHost(PlaybackController *pc, Library *lib, Stations *stations, QObject *parent)
    : QObject(parent), m_pc(pc), m_lib(lib), m_stations(stations)
{
    g_host = this;
    connect(pc, &PlaybackController::trackChanged, this, [this](qint64 id) {
        if (id != 0)
            emitEvent(QStringLiteral("track"), nowplaying::snapshot(m_pc, m_lib, m_stations));
    });
    connect(pc, &PlaybackController::streamTitleChanged, this, [this](const QString &t) {
        if (!t.isEmpty())
            emitEvent(QStringLiteral("title"), nowplaying::snapshot(m_pc, m_lib, m_stations));
    });
    connect(pc, &PlaybackController::stationChanged, this, [this](const QString &name) {
        if (m_pc->mode() != PlaybackController::Mode::Radio)
            return;
        QVariantMap m;
        m.insert(QStringLiteral("station"), name);
        m.insert(QStringLiteral("station_url"), m_pc->stationUrl().toString());
        emitEvent(QStringLiteral("station"), m);
    });
    connect(pc, &PlaybackController::stateChanged, this, [this] {
        const QString s = nowplaying::stateName(m_pc);
        if (s != m_lastState) {
            m_lastState = s;
            emitEvent(QStringLiteral("state"), s);
        }
    });
}

PluginHost::~PluginHost()
{
    emitEvent(QStringLiteral("quit"));
    for (Plugin *p : std::as_const(m_plugins)) {
        stop(p);
        delete p;
    }
    g_host = nullptr;
}

QString PluginHost::userDir() { return paths::dataDir() + QStringLiteral("/plugins"); }

QString PluginHost::bundledDir()
{
    const QString app = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    return app + QStringLiteral("/plugins");
#else
    return QDir::cleanPath(app + QStringLiteral("/../share/ruche/plugins"));
#endif
}

QString PluginHost::dataDirFor(const QString &id) { return paths::dataDir() + QStringLiteral("/plugin-data/") + id; }

PluginHost::Plugin *PluginHost::find(const QString &id) const
{
    for (Plugin *p : m_plugins)
        if (p->id == id)
            return p;
    return nullptr;
}

static void readManifest(Plugin *p, const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    auto s = [&o](const char *k) { return o.value(QLatin1String(k)).toString(); };
    if (!s("name").isEmpty())
        p->name = s("name");
    p->version = s("version");
    p->description = s("description");
    p->author = s("author");
    for (const QJsonValue &v : o.value(QStringLiteral("settings")).toArray()) {
        const QJsonObject so = v.toObject();
        PluginHost::Setting st;
        st.key = so.value(QStringLiteral("key")).toString();
        if (st.key.isEmpty())
            continue;
        st.label = so.value(QStringLiteral("label")).toString(st.key);
        st.type = so.value(QStringLiteral("type")).toString(QStringLiteral("string"));
        st.help = so.value(QStringLiteral("help")).toString();
        st.def = so.value(QStringLiteral("default")).toVariant();
        for (const QJsonValue &c : so.value(QStringLiteral("choices")).toArray())
            st.choices << c.toString();
        st.min = so.value(QStringLiteral("min")).toInt(0);
        st.max = so.value(QStringLiteral("max")).toInt(100000);
        p->settings << st;
    }
}

// Single-file plugins describe themselves in their first comment lines:
//   -- @name Sleep timer
//   -- @description Stops playback after a while.
static void readHeader(Plugin *p, const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    static const QRegularExpression re(QStringLiteral("^--\\s*@(\\w+)\\s+(.*)$"));
    for (int i = 0; i < 30 && !f.atEnd(); ++i) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.startsWith(QLatin1String("--")))
            break;
        const auto m = re.match(line);
        if (!m.hasMatch())
            continue;
        const QString k = m.captured(1), v = m.captured(2).trimmed();
        if (k == QLatin1String("name"))
            p->name = v;
        else if (k == QLatin1String("description"))
            p->description = v;
        else if (k == QLatin1String("version"))
            p->version = v;
        else if (k == QLatin1String("author"))
            p->author = v;
    }
}

void PluginHost::scan()
{
    // Stop and forget everything, then look again (keeps it simple and makes Reload honest).
    for (Plugin *p : std::as_const(m_plugins)) {
        stop(p);
        delete p;
    }
    m_plugins.clear();
    QDir().mkpath(userDir());
    QStringList dirs = {userDir(), bundledDir()};
    for (const QString &extra : qEnvironmentVariable("RUCHE_PLUGIN_PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts))
        dirs << extra;
    QSettings s(paths::configFile(), QSettings::IniFormat);
    for (const QString &dir : dirs) {
        const bool bundled = dir == bundledDir();
        const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
        for (const QFileInfo &fi : entries) {
            auto *p = new Plugin;
            if (fi.isDir()) {
                p->script = fi.absoluteFilePath() + QStringLiteral("/main.lua");
                if (!QFileInfo::exists(p->script)) {
                    delete p;
                    continue;
                }
                p->id = fi.fileName();
                p->dir = fi.absoluteFilePath();
                p->name = p->id;
                readManifest(p, p->dir + QStringLiteral("/plugin.json"));
            } else if (fi.suffix() == QLatin1String("lua")) {
                p->script = fi.absoluteFilePath();
                p->id = fi.completeBaseName();
                p->dir = fi.absolutePath();
                p->name = p->id;
                readHeader(p, p->script);
            } else {
                delete p;
                continue;
            }
            static const QRegularExpression okId(QStringLiteral("^[A-Za-z0-9._-]+$"));
            if (!okId.match(p->id).hasMatch() || find(p->id)) { // a user copy wins over the bundled one
                delete p;
                continue;
            }
            p->bundled = bundled;
            p->enabled = s.value(QStringLiteral("plugins/") + p->id, false).toBool();
            m_plugins << p;
        }
    }
    for (Plugin *p : std::as_const(m_plugins))
        if (p->enabled)
            start(p);
    Q_EMIT changed();
}

void PluginHost::reloadAll() { scan(); }

void PluginHost::setEnabled(const QString &id, bool on)
{
    Plugin *p = find(id);
    if (!p || p->enabled == on)
        return;
    p->enabled = on;
    QSettings(paths::configFile(), QSettings::IniFormat).setValue(QStringLiteral("plugins/") + id, on);
    if (on)
        start(p);
    else
        stop(p);
    Q_EMIT changed();
}

void PluginHost::appendLog(Plugin *p, const QString &line)
{
    const QString stamped = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss  ")) + line;
    p->log << stamped;
    while (p->log.size() > 200)
        p->log.removeFirst();
    qInfo("[plugin %s] %s", qPrintable(p->id), qPrintable(line));
}

void PluginHost::fail(Plugin *p, const QString &where, const QString &message)
{
    p->error = where.isEmpty() ? message : where + QStringLiteral(": ") + message;
    appendLog(p, QStringLiteral("error: ") + p->error);
    Q_EMIT changed();
}

bool PluginHost::call(Plugin *p, int nargs, const QString &where)
{
    lua_State *L = p->L;
    const bool outer = !g_clock.isValid();
    if (outer)
        g_clock.start();
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 20000);
    const int rc = lua_pcall(L, nargs, 0, 0);
    lua_sethook(L, nullptr, 0, 0);
    if (outer)
        g_clock.invalidate();
    if (rc != LUA_OK) {
        const QString msg = QString::fromUtf8(lua_tostring(L, -1));
        lua_pop(L, 1);
        fail(p, where, msg);
        return false;
    }
    return true;
}

void PluginHost::installApi(Plugin *p)
{
    lua_State *L = p->L;
    *static_cast<Plugin **>(lua_getextraspace(L)) = p;
    static const luaL_Reg api[] = {
        {"on", PluginApi::on},
        {"now", PluginApi::now},
        {"state", PluginApi::state},
        {"position", PluginApi::position},
        {"history", PluginApi::history},
        {"play_pause", PluginApi::playPause},
        {"play", PluginApi::play},
        {"pause", PluginApi::pause},
        {"stop", PluginApi::stopPlayback},
        {"next", PluginApi::next},
        {"previous", PluginApi::previous},
        {"seek", PluginApi::seek},
        {"volume", PluginApi::volume},
        {"play_station", PluginApi::playStation},
        {"stations", PluginApi::stations},
        {"notify", PluginApi::notify},
        {"flash", PluginApi::flash},
        {"raise", PluginApi::raise},
        {"log", PluginApi::log},
        {"add_action", PluginApi::addAction},
        {"after", PluginApi::after},
        {"every", PluginApi::every},
        {"cancel", PluginApi::cancel},
        {"http", PluginApi::http},
        {"json_encode", PluginApi::jsonEncode},
        {"json_decode", PluginApi::jsonDecode},
        {"config", PluginApi::config},
        {"set_config", PluginApi::setConfig},
        {nullptr, nullptr},
    };
    luaL_newlib(L, api);
    auto setStr = [L](const char *k, const QString &v) {
        lua_pushstring(L, v.toUtf8().constData());
        lua_setfield(L, -2, k);
    };
    setStr("version", QStringLiteral(RUCHE_VERSION));
    setStr("api_version", QStringLiteral("1"));
#ifdef Q_OS_WIN
    setStr("platform", QStringLiteral("windows"));
#else
    setStr("platform", QStringLiteral("linux"));
#endif
    setStr("id", p->id);
    setStr("plugin_dir", QDir::toNativeSeparators(p->dir));
    setStr("data_dir", QDir::toNativeSeparators(dataDirFor(p->id)));
    lua_setglobal(L, "ruche");
    // print() goes to the plugin's log (Settings → Plugins → Log)
    lua_pushcfunction(L, PluginApi::log);
    lua_setglobal(L, "print");
    // require() finds modules next to the plugin
    lua_getglobal(L, "package");
    const QString path = p->dir + QStringLiteral("/?.lua;") + p->dir + QStringLiteral("/?/init.lua");
    lua_pushstring(L, QDir::toNativeSeparators(path).toUtf8().constData());
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath"); // no native modules
    lua_pop(L, 1);
}

void PluginHost::start(Plugin *p)
{
    stop(p);
    p->error.clear();
    p->L = luaL_newstate();
    if (!p->L) {
        fail(p, QString(), tr("out of memory"));
        return;
    }
    luaL_openlibs(p->L);
    installApi(p);
    QDir().mkpath(dataDirFor(p->id));
    QFile f(p->script);
    if (!f.open(QIODevice::ReadOnly)) {
        fail(p, QString(), tr("can't read %1").arg(p->script));
        stop(p);
        return;
    }
    const QByteArray code = f.readAll();
    const QByteArray chunk = "@" + QFileInfo(p->script).fileName().toUtf8();
    if (luaL_loadbufferx(p->L, code.constData(), size_t(code.size()), chunk.constData(), "t") != LUA_OK) {
        fail(p, QString(), QString::fromUtf8(lua_tostring(p->L, -1)));
        stop(p);
        return;
    }
    appendLog(p, tr("started"));
    if (!call(p, 0, QString())) {
        stop(p);
        return;
    }
    // Something may already be playing.
    if (m_pc->isActive()) {
        const QVariantMap np = nowplaying::snapshot(m_pc, m_lib, m_stations);
        if (m_pc->mode() == PlaybackController::Mode::Library) {
            for (int fn : p->handlers.value(QStringLiteral("track"))) {
                lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
                pushVariant(p->L, np);
                if (!call(p, 1, QStringLiteral("track")))
                    break;
            }
        } else if (!m_pc->streamTitle().isEmpty()) {
            for (int fn : p->handlers.value(QStringLiteral("title"))) {
                lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
                pushVariant(p->L, np);
                if (!call(p, 1, QStringLiteral("title")))
                    break;
            }
        }
    }
}

void PluginHost::stop(Plugin *p)
{
    if (!p->L)
        return;
    for (int fn : p->handlers.value(QStringLiteral("unload"))) {
        lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
        call(p, 0, QStringLiteral("unload"));
    }
    for (QTimer *t : std::as_const(p->timers)) {
        t->stop();
        t->deleteLater();
    }
    p->timers.clear();
    for (const QPointer<QNetworkReply> &r : std::as_const(p->requests))
        if (r)
            r->abort();
    p->requests.clear();
    p->handlers.clear();
    const bool hadActions = !p->actions.isEmpty();
    p->actions.clear();
    lua_State *L = p->L;
    p->L = nullptr;
    lua_close(L);
    if (hadActions)
        Q_EMIT changed();
}

void PluginHost::emitEvent(const QString &event, const QVariant &payload)
{
    for (Plugin *p : std::as_const(m_plugins)) {
        if (!p->L)
            continue;
        const QVector<int> fns = p->handlers.value(event);
        for (int fn : fns) {
            if (!p->L)
                break;
            lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
            int n = 0;
            if (payload.isValid()) {
                pushVariant(p->L, payload);
                n = 1;
            }
            call(p, n, event);
        }
    }
}

QJsonObject PluginHost::config(const QString &id) const
{
    QJsonObject c;
    if (const Plugin *p = find(id))
        for (const Setting &s : p->settings)
            if (s.def.isValid())
                c.insert(s.key, QJsonValue::fromVariant(s.def));
    QFile f(configFileFor(id));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject saved = QJsonDocument::fromJson(f.readAll()).object();
        for (auto it = saved.begin(); it != saved.end(); ++it)
            c.insert(it.key(), it.value());
    }
    return c;
}

void PluginHost::setConfig(const QString &id, const QJsonObject &values)
{
    QJsonObject c;
    QFile in(configFileFor(id));
    if (in.open(QIODevice::ReadOnly))
        c = QJsonDocument::fromJson(in.readAll()).object();
    in.close();
    for (auto it = values.begin(); it != values.end(); ++it)
        c.insert(it.key(), it.value());
    QDir().mkpath(dataDirFor(id));
    QSaveFile f(configFileFor(id));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(c).toJson());
        f.commit();
    }
    Plugin *p = find(id);
    if (p && p->L)
        for (int fn : p->handlers.value(QStringLiteral("config"))) {
            lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn);
            pushVariant(p->L, config(id).toVariantMap());
            call(p, 1, QStringLiteral("config"));
        }
}

QVector<PluginHost::Action> PluginHost::actions() const
{
    QVector<Action> out;
    for (const Plugin *p : m_plugins)
        if (p->L)
            for (const auto &a : p->actions)
                out.append({p->id, a.first, a.second});
    return out;
}

void PluginHost::runAction(const QString &plugin, int fnRef)
{
    Plugin *p = find(plugin);
    if (!p || !p->L)
        return;
    lua_rawgeti(p->L, LUA_REGISTRYINDEX, fnRef);
    call(p, 0, QStringLiteral("menu action"));
}
