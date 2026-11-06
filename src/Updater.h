// Updates from the project's GitHub releases. Finds the newest release, picks the file that
// matches how this copy was installed (AppImage, .deb, .rpm, Flatpak, Windows installer or
// zip), downloads it, checks it against the release's SHA256SUMS, and installs it the way
// that kind of install is updated.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QWidget;

class Updater : public QObject {
    Q_OBJECT
public:
    enum class Kind { AppImage, Flatpak, Deb, Rpm, WindowsInstaller, WindowsPortable, Other };
    struct Release {
        QString version;   // "0.1.7"
        QString notes;     // Markdown, from the release description
        QString page;      // the release's web page
        QString assetName, assetUrl;
        qint64 assetSize = 0;
        QString sumsUrl;   // SHA256SUMS, when the release has one
    };

    explicit Updater(QObject *parent = nullptr);

    static Kind kind();
    static QString kindName(Kind k);
    static QString repo();            // "owner/name"
    static QString releasesPage();
    // -1, 0, 1 like strcmp, on dotted numbers ("0.1.10" > "0.1.9"); a leading "v" is ignored.
    static int compareVersions(const QString &a, const QString &b);
    // Starts a program once this process has exited (so it doesn't hand over to us).
    static bool startAfterExit(const QString &program, const QStringList &args);
    // How to start this copy of Ruche again (empty when it can't be done from inside, i.e. Flatpak).
    static QString selfProgram();

    void check();          // emits checked()
    void download(const Release &r); // emits progress(), then downloaded() or failed()
    void cancel();
    // Puts the downloaded file in place / hands it to the system. Returns false (with a message)
    // when it couldn't; may quit Ruche to restart it.
    bool install(const Release &r, const QString &file, QWidget *parent, QString *message);

Q_SIGNALS:
    void checked(bool newer, const Updater::Release &release, const QString &error);
    void progress(qint64 received, qint64 total);
    void downloaded(const QString &file);
    void failed(const QString &error);

private:
    void fetchSums(const Release &r, const QString &file);
    QString targetFor(const Release &r) const;
    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
};
