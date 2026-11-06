#pragma once

#include <QAbstractListModel>
#include <QAbstractTableModel>
#include <QCollator>
#include <QColor>
#include <QHash>
#include <QVector>

class Library;
struct Track;

QString formatDuration(qint64 ms);
// "1 track" / "12 tracks"
QString countOf(qint64 n, const QString &one, const QString &many);
// Locale-aware, numeric, case-insensitive; falls back to English rules under the C locale
// so "Électrique" still sorts with the E's.
QCollator makeCollator();

class TrackTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColNum, ColTitle, ColArtist, ColAlbum, ColYear, ColGenre, ColTime, ColPlays, ColCount };
    explicit TrackTableModel(Library *lib, QObject *parent = nullptr);

    void setIds(const QVector<qint64> &ids, bool keepSort = true);
    const QVector<qint64> &ids() const { return m_ids; }
    qint64 idAt(int row) const { return m_ids.value(row, 0); }
    void setPlaying(qint64 id, int queueRow = -1);
    void setQueueMode(bool on) { m_queueMode = on; }
    void setAccent(const QColor &c) { m_accent = c; }
    int sortColumn() const { return m_sortColumn; }
    Qt::SortOrder sortOrder() const { return m_sortOrder; }
    void setSortEnabled(bool on) { m_sortEnabled = on; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    void sort(int column, Qt::SortOrder order) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    void applySort();
    Library *m_lib;
    QVector<qint64> m_ids;
    qint64 m_playingId = 0;
    int m_playingRow = -1;
    bool m_queueMode = false;
    bool m_sortEnabled = true;
    int m_sortColumn = -1; // -1 = album artist / album / disc / track
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    QColor m_accent;
};

// One pane of the Genre | Artist | Album column browser.
class BrowserModel : public QAbstractListModel {
    Q_OBJECT
public:
    struct Entry {
        QString key;
        QString display;
        int count = 0;
    };
    explicit BrowserModel(const QString &allLabel, QObject *parent = nullptr);
    void setEntries(const QVector<Entry> &entries);
    QString keyAt(int row) const; // row 0 is "All"
    int rowForKey(const QString &key) const;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    QString m_allLabel;
    QVector<Entry> m_entries;
    QHash<QString, int> m_rowByKey;
};

struct Station;
class Stations;

class StationModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit StationModel(Stations *stations, QObject *parent = nullptr);
    void setLive(const QString &url, const QString &title);
    void setAccent(const QColor &c) { m_accent = c; }
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    Stations *m_stations;
    QString m_liveUrl, m_liveTitle;
    QColor m_accent;
};
