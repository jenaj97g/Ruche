#pragma once

#include "Models.h"
#include "Theme.h"

#include <QImage>
#include <QMainWindow>
#include <QSet>
#include <QTimer>

class Library;
class PlaybackController;
class Stations;
class Mpris;
class ArtLoader;
class PlayerBar;
class NowPlayingPanel;
class QLineEdit;
class QLabel;
class QListView;
class QSplitter;
class QStackedWidget;
class QTableView;
class QTreeWidget;
class QTreeWidgetItem;
class QProgressBar;
class QSettings;
class QSystemTrayIcon;
class QAction;
class QDBusVariant;
class Notifier;
class PluginHost;
class QLabel;
#include "Updater.h"
class DiscordPresence;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(Library *lib, PlaybackController *pc, Stations *stations, QWidget *parent = nullptr);
    ~MainWindow() override;
    void setMpris(Mpris *m) { m_mpris = m; }
    void openArguments(const QStringList &args);
    void showRadio();
    void setQuitting() { m_quitting = true; } // real quit: don't just hide to the tray
    void showFromTray();

#ifdef RUCHE_DBUS
private Q_SLOTS:
    void onPortalSetting(const QString &ns, const QString &key, const QDBusVariant &value);
#endif

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    enum class View { Library, NowPlaying, Playlist, Radio };

    void buildUi();
    void setupShortcuts();
    bool navDropEvent(QEvent *ev);
    QString askPlaylistName(const QString &title, const QString &current = QString());
    void newPlaylist();
    void renamePlaylist(const QString &name);
    void deletePlaylist(const QString &name);
    void rebuildNavigator();
    void fitNarrowColumns();
    void switchView(View v, const QString &playlist = QString());
    void refreshLibrary(int fromLevel);
    QSet<QString> selectedKeys(QListView *view, BrowserModel *model) const;
    void restoreSelection(QListView *view, BrowserModel *model, const QSet<QString> &keys);
    void updateStatus();
    void onTrackChanged(qint64 id);
    void onStateChanged();
    void updateNowPlaying();
    void updateUpNextOrHistory();
    void onArt(int token, const QImage &img);
    void trackContextMenu(const QPoint &pos);
    void stationContextMenu(const QPoint &pos);
    void playFromView(int row);
    QVector<qint64> selectedTrackIds() const;
    QVector<int> selectedRows() const;
    void addToPlaylistMenu(QMenu *menu, const QVector<qint64> &ids);
    void removeSelected();
    void addStation();
    void editStation(int row);
    void importStationsFile();
    void importStationsUrl();
    void playStationRow(int row);
    void pasteLogo(int row);
    void chooseLogoFile(int row);
    void exportStations();
    void openSettings(const QString &page = QString());
    void configurePlugin(const QString &id, QWidget *parent);
    void shutdownExtensions();
    void quitApp();
    void startupUpdateCheck();
    void checkForUpdates(bool manual, QLabel *status = nullptr);
    void showUpdate(const Updater::Release &rel, bool manual);
    void showAbout();
    QString tabFromStored(const QString &stored) const;
    QString tabToStored(const QString &name) const;
    void loadLibraryTabs();
    void saveLibraryTabs();
    void rebuildLibraryTabs();
    QStringList currentTabFolders() const;
    void renameLibraryTab(int i);
    void newLibraryTab();
    void pinFolderTabs();
    void closeLibraryTab(int i);
    void updateTabHint();
    void applyTheme(const Theme::Choice &c);
    void watchThemeFile();
    void showCrashNotice();
    void addFolder();
    void openFiles(const QStringList &paths, bool play);
    void importPlaylistFile(const QString &file);
    void loadSettings();
    void saveSettings();
    QString techLine() const;
    void flash(const QString &text);
    void setCloseToTray(bool on, bool persist = true);
    void syncTray(); // the tray icon exists while close-to-tray (or, on Windows, pop-ups) need it
    void updateTray();
    void setNotifyWhen(const QString &when, bool persist = true);
    void scheduleTrackNotification();
    void notifyNow();
    bool wantNotification() const;

    Library *m_lib;
    PlaybackController *m_pc;
    Stations *m_stations;
    Mpris *m_mpris = nullptr;
    ArtLoader *m_art;

    QTreeWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_libraryPage = nullptr;
    QWidget *m_radioPage = nullptr;
    QSplitter *m_mainSplit = nullptr;
    QSplitter *m_libSplit = nullptr;
    QSplitter *m_browserSplit = nullptr;
    QWidget *m_browser = nullptr;
    QListView *m_genreView = nullptr, *m_artistView = nullptr, *m_albumView = nullptr;
    BrowserModel *m_genreModel, *m_artistModel, *m_albumModel;
    QTableView *m_trackView = nullptr;
    TrackTableModel *m_trackModel;
    QTableView *m_stationView = nullptr;
    StationModel *m_stationModel;
    PlayerBar *m_bar = nullptr;
    NowPlayingPanel *m_np = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_viewTitle = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_flash = nullptr;
    QProgressBar *m_scanBar = nullptr;
    QLabel *m_stationHint = nullptr;
    QString m_logoKey; // station whose logo is on screen
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_trayTitle = nullptr;
    QAction *m_trayShow = nullptr;
    QAction *m_trayPlay = nullptr;
    bool m_closeToTray = false;
    Notifier *m_notifier = nullptr;
    PluginHost *m_plugins = nullptr;
    DiscordPresence *m_discord = nullptr;
    Updater *m_updater = nullptr;
    class QTabBar *m_libTabs = nullptr;
    QWidget *m_tabRow = nullptr;
    QWidget *m_tabHint = nullptr;
    class QFileSystemWatcher *m_themeWatch = nullptr;
    QStringList m_tabNames;
    QHash<QString, QString> m_folderTab; // folder -> tab name
    bool m_tabsUpdating = false;
    Theme::Choice m_themeChoice;
    QString m_notifyWhen = QStringLiteral("background"); // off, background, always
    QTimer m_notifyTimer; // waits briefly for the cover before showing a track's pop-up
    bool m_notifyPending = false;
    bool m_artPending = false;
    qint64 m_lastNotifiedTrack = 0;
    QString m_lastNotifiedTitle;
    bool m_quitting = false;
    QTimer m_searchTimer;
    QTimer m_flashTimer;

    View m_view = View::Library;
    QString m_playlist;
    QVector<qint64> m_base, m_afterGenre, m_afterArtist;
    int m_artToken = 0;
    QImage m_coverImage;
    qint64 m_coverFor = 0;
    QByteArray m_headerStateLibrary;
    bool m_restoringSelection = false;
};
