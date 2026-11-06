#include "Models.h"
#include "Theme.h"
#include "Library.h"
#include "Icons.h"
#include "Logo.h"
#include "Stations.h"

#include <QCollator>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QLocale>
#include <algorithm>

QString countOf(qint64 n, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(QLocale().toString(n), n == 1 ? one : many);
}

QCollator makeCollator()
{
    QLocale loc;
    if (loc.language() == QLocale::C)
        loc = QLocale(QLocale::English, QLocale::UnitedStates);
    QCollator c(loc);
    c.setNumericMode(true);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}

QString formatDuration(qint64 ms)
{
    if (ms <= 0)
        return QString();
    qint64 s = (ms + 500) / 1000;
    const qint64 h = s / 3600;
    s %= 3600;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// ---------------------------------------------------------------------------

TrackTableModel::TrackTableModel(Library *lib, QObject *parent) : QAbstractTableModel(parent), m_lib(lib) {}

void TrackTableModel::setIds(const QVector<qint64> &ids, bool keepSort)
{
    beginResetModel();
    m_ids = ids;
    if (keepSort && m_sortEnabled)
        applySort();
    m_playingRow = -1;
    endResetModel();
}

void TrackTableModel::setPlaying(qint64 id, int queueRow)
{
    const qint64 old = m_playingId;
    const int oldRow = m_playingRow;
    m_playingId = id;
    m_playingRow = queueRow;
    if (old != id || oldRow != queueRow) {
        // Repaint only what changed would need a lookup; rows are cheap to repaint.
        if (!m_ids.isEmpty())
            Q_EMIT dataChanged(index(0, 0), index(m_ids.size() - 1, ColCount - 1), {Qt::FontRole, Qt::ForegroundRole});
    }
}

int TrackTableModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_ids.size(); }
int TrackTableModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : ColCount; }

Qt::ItemFlags TrackTableModel::flags(const QModelIndex &index) const
{
    return index.isValid() ? Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled : Qt::NoItemFlags;
}

QVariant TrackTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_ids.size())
        return {};
    const Track *t = m_lib->byId(m_ids[index.row()]);
    if (!t)
        return {};
    const bool playing = m_queueMode ? index.row() == m_playingRow : (t->id == m_playingId && m_playingId != 0);
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColNum:
            if (m_queueMode)
                return index.row() + 1;
            return t->trackNo > 0 ? QVariant(t->trackNo) : QVariant();
        case ColTitle:
            return t->displayTitle();
        case ColArtist:
            return t->artist;
        case ColAlbum:
            return t->album;
        case ColYear:
            return t->year > 0 ? QVariant(t->year) : QVariant();
        case ColGenre:
            return t->genre;
        case ColTime:
            return formatDuration(t->durationMs);
        case ColPlays:
            return t->plays > 0 ? QVariant(t->plays) : QVariant();
        }
        break;
    case Qt::ToolTipRole:
        if (index.column() == ColTitle)
            return t->filePath();
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == ColNum || index.column() == ColTime || index.column() == ColPlays || index.column() == ColYear)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::FontRole:
        if (playing) {
            QFont f;
            f.setBold(true);
            return f;
        }
        break;
    case Qt::ForegroundRole:
        if (playing && m_accent.isValid())
            return m_accent;
        break;
    }
    return {};
}

QVariant TrackTableModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::TextAlignmentRole)
        return int((section == ColNum || section == ColTime || section == ColPlays || section == ColYear ? Qt::AlignRight
                                                                                                          : Qt::AlignLeft) |
                   Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColNum:
        return QStringLiteral("#");
    case ColTitle:
        return tr("Title");
    case ColArtist:
        return tr("Artist");
    case ColAlbum:
        return tr("Album");
    case ColYear:
        return tr("Year");
    case ColGenre:
        return tr("Genre");
    case ColTime:
        return tr("Time");
    case ColPlays:
        return tr("Plays");
    }
    return {};
}

void TrackTableModel::sort(int column, Qt::SortOrder order)
{
    if (!m_sortEnabled)
        return;
    beginResetModel();
    m_sortColumn = column;
    m_sortOrder = order;
    applySort();
    endResetModel();
}

void TrackTableModel::applySort()
{
    const QCollator coll = makeCollator();
    const auto &tracks = m_lib->tracks();
    // Resolve ids to indexes once rather than hashing on every comparison.
    QVector<QPair<int, qint64>> rows;
    rows.reserve(m_ids.size());
    for (qint64 id : std::as_const(m_ids))
        rows.push_back({m_lib->indexOf(id), id});

    auto albumOrder = [&](const Track &a, const Track &b) {
        if (int c = coll.compare(a.album, b.album))
            return c < 0;
        if (a.discNo != b.discNo)
            return a.discNo < b.discNo;
        if (a.trackNo != b.trackNo)
            return a.trackNo < b.trackNo;
        return coll.compare(a.displayTitle(), b.displayTitle()) < 0;
    };
    auto less = [&](const QPair<int, qint64> &x, const QPair<int, qint64> &y) {
        if (x.first < 0 || y.first < 0)
            return x.first < y.first;
        const Track &a = tracks[x.first];
        const Track &b = tracks[y.first];
        int c = 0;
        switch (m_sortColumn) {
        case ColTitle:
            c = coll.compare(a.displayTitle(), b.displayTitle());
            break;
        case ColArtist:
            c = coll.compare(a.artist, b.artist);
            break;
        case ColAlbum:
            c = coll.compare(a.album, b.album);
            break;
        case ColYear:
            c = a.year - b.year;
            break;
        case ColGenre:
            c = coll.compare(a.genre, b.genre);
            break;
        case ColTime:
            c = a.durationMs < b.durationMs ? -1 : (a.durationMs > b.durationMs ? 1 : 0);
            break;
        case ColPlays:
            c = a.plays - b.plays;
            break;
        case ColNum:
            c = a.trackNo - b.trackNo;
            break;
        default:
            c = coll.compare(a.displayAlbumArtist(), b.displayAlbumArtist());
            break;
        }
        if (c != 0)
            return m_sortOrder == Qt::AscendingOrder ? c < 0 : c > 0;
        if (m_sortColumn != -1 && m_sortColumn != ColAlbum) {
            if (int c2 = coll.compare(a.displayAlbumArtist(), b.displayAlbumArtist()))
                return c2 < 0;
        }
        return albumOrder(a, b);
    };
    std::stable_sort(rows.begin(), rows.end(), less);
    for (int i = 0; i < rows.size(); ++i)
        m_ids[i] = rows[i].second;
}

// ---------------------------------------------------------------------------

BrowserModel::BrowserModel(const QString &allLabel, QObject *parent) : QAbstractListModel(parent), m_allLabel(allLabel) {}

void BrowserModel::setEntries(const QVector<Entry> &entries)
{
    beginResetModel();
    m_entries = entries;
    m_rowByKey.clear();
    for (int i = 0; i < m_entries.size(); ++i)
        m_rowByKey.insert(m_entries[i].key, i + 1);
    endResetModel();
}

QString BrowserModel::keyAt(int row) const { return row <= 0 ? QString() : m_entries.value(row - 1).key; }

int BrowserModel::rowForKey(const QString &key) const { return m_rowByKey.value(key, -1); }

int BrowserModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_entries.size() + 1; }

QVariant BrowserModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    if (index.row() == 0) {
        if (role == Qt::DisplayRole)
            return QStringLiteral("%1 (%2)").arg(m_allLabel).arg(m_entries.size());
        if (role == Qt::FontRole) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    }
    const Entry &e = m_entries.at(index.row() - 1);
    if (role == Qt::DisplayRole)
        return e.display.isEmpty() ? tr("(Unknown)") : e.display;
    if (role == Qt::ToolTipRole)
        return countOf(e.count, tr("track"), tr("tracks"));
    return {};
}

// ---------------------------------------------------------------------------

StationModel::StationModel(Stations *stations, QObject *parent) : QAbstractTableModel(parent), m_stations(stations)
{
    connect(stations, &Stations::changed, this, [this] {
        beginResetModel();
        endResetModel();
    });
    connect(stations, &Stations::lastTitleChanged, this,
            [this](int row) { Q_EMIT dataChanged(index(row, 0), index(row, 2)); });
    connect(stations, &Stations::logoChanged, this,
            [this](int row) { Q_EMIT dataChanged(index(row, 0), index(row, 0), {Qt::DecorationRole}); });
}

static QPixmap placeholderTile(int size, qreal dpr)
{
    static QPixmap cached;
    static int generation = -1;
    if (cached.isNull() || generation != Theme::generation() || !qFuzzyCompare(cached.devicePixelRatio(), dpr) ||
        cached.width() != int(size * dpr)) {
        generation = Theme::generation();
        cached = QPixmap(QSize(size, size) * dpr);
        cached.setDevicePixelRatio(dpr);
        cached.fill(Qt::transparent);
        QPainter p(&cached);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, size, size), size * 0.16, size * 0.16);
        p.fillPath(clip, Theme::surface());
        Icons::paint(&p, QRectF(size * 0.2, size * 0.2, size * 0.6, size * 0.6), Icons::Radio, Theme::faint());
    }
    return cached;
}

void StationModel::setLive(const QString &url, const QString &title)
{
    m_liveUrl = url;
    m_liveTitle = title;
    if (rowCount() > 0)
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, 2));
}

int StationModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_stations->list().size(); }
int StationModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : 3; }

QVariant StationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const Station &s = m_stations->list().at(index.row());
    const bool live = !m_liveUrl.isEmpty() && s.url == m_liveUrl;
    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == 0)
            return s.name;
        if (index.column() == 1)
            return live ? m_liveTitle : s.lastTitle;
        return s.url;
    case Qt::ToolTipRole:
        return s.url;
    case Qt::DecorationRole:
        if (index.column() == 0) {
            const qreal dpr = qApp ? qobject_cast<QGuiApplication *>(qApp)->devicePixelRatio() : 1.0;
            const QString logo = m_stations->logoPath(index.row());
            return logo.isEmpty() ? placeholderTile(28, dpr) : Logo::tile(logo, 28, dpr);
        }
        break;
    case Qt::FontRole:
        if (live && index.column() < 2) {
            QFont f;
            f.setBold(true);
            return f;
        }
        break;
    case Qt::ForegroundRole:
        if (live && m_accent.isValid() && index.column() < 2)
            return m_accent;
        break;
    }
    return {};
}

QVariant StationModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0:
        return tr("Station");
    case 1:
        return tr("Now playing / last heard");
    case 2:
        return tr("Address");
    }
    return {};
}
