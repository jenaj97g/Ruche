// Radio station list, stored as JSON, with logos saved under ~/.local/share/ruche/logos.
// Imports M3U/M3U8/PLS station lists (keeping tvg-logo) and HLS URLs; exports M3U8.
#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;

struct Station {
    QString name;
    QString url;
    QString lastTitle;
    QString logo;    // file name inside logoDir(), empty if none
    QString logoUrl; // where the logo came from, if it was downloaded
};

class Stations : public QObject {
    Q_OBJECT
public:
    explicit Stations(QObject *parent = nullptr);
    void load();
    void save() const;

    const QVector<Station> &list() const { return m_list; }
    int indexOfUrl(const QString &url) const;
    int add(const QString &name, const QString &url, const QString &logoUrl = QString());
    void update(int i, const QString &name, const QString &url);
    void remove(int i);
    void setLastTitle(const QString &url, const QString &title);

    static QString logoDir();
    QString logoPath(int i) const;
    QString logoPathForUrl(const QString &stationUrl) const;
    bool setLogoFromFile(int i, const QString &file, QString *error = nullptr);
    bool setLogoFromImage(int i, const QImage &img, const QString &sourceUrl = QString());
    void setLogoFromUrl(int i, const QUrl &url); // async; logoChanged() or logoFailed()
    void clearLogo(int i);

    // Returns how many stations were added. HLS playlists become one station each.
    int importText(const QString &text, const QUrl &source);
    int importFile(const QString &file);
    void importUrl(const QUrl &url); // async, reports through imported()
    bool exportM3u(const QString &file, QString *error = nullptr) const;

Q_SIGNALS:
    void changed();
    void lastTitleChanged(int row);
    void logoChanged(int row);
    void logoFailed(int row, const QString &error);
    void imported(int count, const QString &error);

private:
    struct PendingLogo {
        QString stationUrl;
        QUrl logoUrl;
        bool userRequested;
    };
    QNetworkAccessManager *nam();
    void queueLogo(const QString &stationUrl, const QUrl &logoUrl, bool userRequested);
    void pumpLogos();
    QString storeLogo(const QImage &img);
    void dropLogoFileIfUnused(const QString &file);

    QVector<Station> m_list;
    QNetworkAccessManager *m_nam = nullptr;
    QList<PendingLogo> m_logoQueue;
    int m_logoActive = 0;
    QSet<QString> m_logoTried;
};
