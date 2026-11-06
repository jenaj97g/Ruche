#include "MainWindow.h"
#include "Art.h"
#include "DesktopEntry.h"
#include "Icons.h"
#include "Library.h"
#include "Logo.h"
#include "MetaParse.h"
#include "Mpris.h"
#include "DiscordPresence.h"
#include "Notifier.h"
#include "Plugins.h"
#include "Updater.h"
#include "I18n.h"
#include "PlaybackController.h"
#include "Stations.h"
#include "CrashLog.h"
#include "Theme.h"
#include "ThemeFiles.h"
#include "Widgets.h"

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QCloseEvent>
#include <QCollator>
#include <QColorDialog>
#include <QComboBox>
#include <QStyleHints>
#include <QGuiApplication>
#ifdef RUCHE_DBUS
#include <QDBusConnection>
#include <QDBusVariant>
#endif
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QLocale>
#include <QSettings>
#include <QSignalBlocker>
#include <QFileSystemWatcher>
#include <QMouseEvent>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QPainter>
#include <QPainterPath>
#include <QCheckBox>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QInputDialog>
#include <QHeaderView>
#include <QTableView>
#include <QToolButton>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QTextBrowser>
#include <QSpinBox>
#include <QJsonObject>
#include <functional>
#include <memory>

namespace {
enum NavRole { NavKind = Qt::UserRole + 1, NavName };

QString fold(const QString &s)
{
    bool ascii = true;
    for (QChar c : s)
        if (c.unicode() >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii)
        return s.toLower();
    const QString d = s.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(d.size());
    for (QChar c : d)
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c);
    return out.toCaseFolded();
}

QString formatLong(qint64 ms)
{
    const qint64 mins = ms / 60000;
    const qint64 days = mins / 1440, hours = (mins % 1440) / 60, m = mins % 60;
    if (days > 0)
        return QObject::tr("%1 d %2 h").arg(days).arg(hours);
    if (hours > 0)
        return QObject::tr("%1 h %2 min").arg(hours).arg(m);
    if (m == 0)
        return QObject::tr("%1 s").arg(ms / 1000);
    return QObject::tr("%1 min").arg(m);
}

QString genreKey(const Track &t) { return t.genre; }
QString artistKey(const Track &t) { return t.displayAlbumArtist(); }
QString albumKey(const Track &t) { return t.displayAlbumArtist() + QChar(0x1f) + t.album; }

QToolButton *toolButton(Icons::Name icon, const QString &text, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setIcon(Icons::icon(icon));
    b->setIconSize(QSize(16, 16));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
} // namespace

static QImage logoImage();

#ifdef Q_OS_WIN
static constexpr int kPathCase = Qt::CaseInsensitive; // C:/Music and c:/music are the same folder
#else
static constexpr int kPathCase = Qt::CaseSensitive;
#endif

MainWindow::MainWindow(Library *lib, PlaybackController *pc, Stations *stations, QWidget *parent)
    : QMainWindow(parent), m_lib(lib), m_pc(pc), m_stations(stations), m_art(new ArtLoader(this)),
      m_genreModel(new BrowserModel(tr("All genres"), this)), m_artistModel(new BrowserModel(tr("All artists"), this)),
      m_albumModel(new BrowserModel(tr("All albums"), this)), m_trackModel(new TrackTableModel(lib, this)),
      m_stationModel(new StationModel(stations, this))
{
    setWindowTitle(QStringLiteral("Ruche"));
    setWindowIcon(Icons::accentIcon(Icons::Logo));
    setAcceptDrops(true);
    m_trackModel->setAccent(Theme::accentText());
    m_stationModel->setAccent(Theme::accentText());
    buildUi();
    setupShortcuts();
    rebuildNavigator();

    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(180);
    connect(&m_searchTimer, &QTimer::timeout, this, [this] { refreshLibrary(0); });
    m_flashTimer.setSingleShot(true);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] { m_flash->clear(); });

    connect(m_art, &ArtLoader::ready, this, &MainWindow::onArt);
    connect(m_lib, &Library::reloaded, this, [this] {
        if (m_view == View::Library)
            refreshLibrary(0);
        updateStatus();
    });
    connect(m_lib, &Library::scanProgress, this, [this](int done, int total) {
        m_scanBar->setVisible(true);
        m_scanBar->setRange(0, total);
        m_scanBar->setValue(done);
        m_status->setText(tr("Scanning library… %1 / %2").arg(done).arg(total));
    });
    connect(m_lib, &Library::scanFinished, this, [this](int a, int u, int r) {
        m_scanBar->hide();
        updateStatus();
        if (a || u || r)
            flash(tr("Library updated: %1 added, %2 changed, %3 removed").arg(a).arg(u).arg(r));
    });
    connect(m_lib, &Library::playlistsChanged, this, &MainWindow::rebuildNavigator);

    connect(m_pc, &PlaybackController::trackChanged, this, &MainWindow::onTrackChanged);
    connect(m_pc, &PlaybackController::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_pc, &PlaybackController::stateChanged, this, &MainWindow::updateTray);
    connect(m_pc, &PlaybackController::trackChanged, this, &MainWindow::updateTray);
    connect(m_pc, &PlaybackController::streamTitleChanged, this, &MainWindow::updateTray);
    connect(m_pc, &PlaybackController::stationChanged, this, &MainWindow::updateTray);
    connect(m_pc, &PlaybackController::queueChanged, this, [this] {
        if (m_view == View::NowPlaying) {
            m_trackModel->setIds(m_pc->queue(), false);
            m_trackModel->setPlaying(m_pc->currentTrackId(), m_pc->queueIndex());
        }
        updateUpNextOrHistory();
    });
    connect(m_pc, &PlaybackController::stationChanged, this, [this](const QString &name) {
        // A station added straight from a URL is named after its host until the stream says otherwise.
        const QString url = m_pc->stationUrl().toString();
        const int row = m_stations->indexOfUrl(url);
        if (row >= 0 && !name.isEmpty() && m_stations->list()[row].name == m_pc->stationUrl().host())
            m_stations->update(row, name, url);
        updateNowPlaying();
    });
    connect(m_pc, &PlaybackController::streamTitleChanged, this, [this](const QString &t) {
        m_stationModel->setLive(m_pc->stationUrl().toString(), t);
        if (!t.isEmpty())
            m_stations->setLastTitle(m_pc->stationUrl().toString(), t);
        updateNowPlaying();
    });
    connect(m_pc, &PlaybackController::historyChanged, this, &MainWindow::updateUpNextOrHistory);
    connect(m_stations, &Stations::logoChanged, this, [this](int row) {
        if (row >= 0 && row < m_stations->list().size() && m_stations->list()[row].url == m_pc->stationUrl().toString()) {
            m_logoKey.clear();
            updateNowPlaying();
        }
    });
    connect(m_stations, &Stations::logoFailed, this, [this](int row, const QString &err) {
        const QString name = row >= 0 && row < m_stations->list().size() ? m_stations->list()[row].name : QString();
        flash(tr("Logo for %1: %2").arg(name, err));
    });
    connect(m_pc, &PlaybackController::message, this, &MainWindow::flash);
    int ticks = 0;
    connect(m_pc, &PlaybackController::positionChanged, this, [this, ticks](double pos, double dur) mutable {
        const bool live = m_pc->mode() == PlaybackController::Mode::Radio;
        m_bar->setPosition(pos, dur, m_pc->isSeekable(), live);
        if (live && (++ticks % 8) == 1)
            updateNowPlaying(); // codec/bitrate become known a moment after the stream starts
    });

    connect(m_bar, &PlayerBar::playPause, m_pc, &PlaybackController::togglePause);
    connect(m_bar, &PlayerBar::next, m_pc, &PlaybackController::next);
    connect(m_bar, &PlayerBar::previous, m_pc, &PlaybackController::previous);
    connect(m_bar, &PlayerBar::seekRequested, m_pc, &PlaybackController::seek);
    connect(m_bar, &PlayerBar::volumeChanged, m_pc, &PlaybackController::setVolume);
    connect(m_bar, &PlayerBar::shuffleToggled, m_pc, &PlaybackController::setShuffle);
    connect(m_bar, &PlayerBar::repeatClicked, this, [this] {
        const auto r = m_pc->repeat();
        m_pc->setRepeat(r == PlaybackController::Repeat::Off   ? PlaybackController::Repeat::All
                        : r == PlaybackController::Repeat::All ? PlaybackController::Repeat::One
                                                               : PlaybackController::Repeat::Off);
    });
    connect(m_bar, &PlayerBar::coverClicked, this, [this] {
        if (m_pc->mode() == PlaybackController::Mode::Radio)
            showRadio();
        else
            switchView(View::NowPlaying);
    });
    connect(m_np, &NowPlayingPanel::itemActivated, this, [this](int row) {
        if (m_pc->mode() != PlaybackController::Mode::Library)
            return;
        const QVector<int> up = m_pc->upcomingIndexes(60);
        if (row >= 0 && row < up.size())
            m_pc->playQueueIndex(up[row]);
    });

    // "Match the system": follow the desktop when it switches between dark and light.
    m_themeChoice = Theme::loadChoice();
    watchThemeFile(); // main() applied the theme before this window existed
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_themeChoice.theme == QLatin1String("system"))
            applyTheme(m_themeChoice);
    });
#endif
#ifdef RUCHE_DBUS
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.portal.Desktop"),
                                          QStringLiteral("/org/freedesktop/portal/desktop"),
                                          QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("SettingChanged"),
                                          this, SLOT(onPortalSetting(QString, QString, QDBusVariant)));
#endif
    m_notifier = new Notifier(this);
    m_discord = new DiscordPresence(m_pc, m_lib, m_stations, this);
    m_plugins = new PluginHost(m_pc, m_lib, m_stations, this);
    m_plugins->notifyHook = [this](const QString &summary, const QString &body) {
        if (m_notifier)
            m_notifier->show(summary, body, logoImage(), false);
    };
    m_plugins->flashHook = [this](const QString &text) { flash(text); };
    m_plugins->raiseHook = [this] { showFromTray(); };
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::shutdownExtensions);
    QTimer::singleShot(0, this, [this] {
        if (m_plugins)
            m_plugins->scan();
    });
    m_updater = new Updater(this);
    QTimer::singleShot(1500, this, &MainWindow::startupUpdateCheck);
    QTimer::singleShot(900, this, &MainWindow::showCrashNotice);
    connect(m_notifier, &Notifier::activated, this, &MainWindow::showFromTray);
    connect(m_notifier, &Notifier::skipRequested, m_pc, &PlaybackController::next);
    m_notifyTimer.setSingleShot(true);
    connect(&m_notifyTimer, &QTimer::timeout, this, &MainWindow::notifyNow);
    connect(m_pc, &PlaybackController::trackChanged, this, [this](qint64 id) {
        if (id == 0) {
            m_notifyPending = false;
            m_notifyTimer.stop();
            m_lastNotifiedTrack = 0;
            return;
        }
        // Repeat One would otherwise announce the same song on every loop.
        if (id == m_lastNotifiedTrack && m_pc->repeat() == PlaybackController::Repeat::One)
            return;
        scheduleTrackNotification();
    });
    connect(m_pc, &PlaybackController::streamTitleChanged, this, [this](const QString &t) {
        if (t.isEmpty()) { // new station
            m_lastNotifiedTitle.clear();
            return;
        }
        if (t == m_lastNotifiedTitle || !wantNotification())
            return;
        m_lastNotifiedTitle = t;
        notifyNow();
    });

    loadSettings();
    switchView(View::Library);
    updateStatus();
    onStateChanged();
    updateNowPlaying();
    updateUpNextOrHistory();
}

MainWindow::~MainWindow()
{
    shutdownExtensions();
}

void MainWindow::shutdownExtensions()
{
    // Plugins get their "quit" while the player, library and stations still exist.
    delete m_plugins;
    m_plugins = nullptr;
    delete m_discord;
    m_discord = nullptr;
}

// ---------------------------------------------------------------------------
// UI construction

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- top bar
    auto *top = new QWidget(central);
    top->setObjectName(QStringLiteral("topBar"));
    top->setAttribute(Qt::WA_StyledBackground);
    top->setFixedHeight(46);
    auto *th = new QHBoxLayout(top);
    th->setContentsMargins(8, 6, 12, 6);
    th->setSpacing(8);
    auto *menuBtn = new QToolButton(top);
    menuBtn->setIcon(Icons::icon(Icons::Menu));
    menuBtn->setIconSize(QSize(18, 18));
    menuBtn->setPopupMode(QToolButton::InstantPopup);
    menuBtn->setToolTip(tr("Menu"));
    menuBtn->setMenu(new QMenu(menuBtn));
    menuBtn->setCursor(Qt::PointingHandCursor);
    auto *logo = new QLabel(top);
    logo->setPixmap(Icons::accentIcon(Icons::Logo).pixmap(QSize(22, 22), devicePixelRatioF()));
    auto *brand = new QLabel(QStringLiteral("Ruche"), top);
    brand->setObjectName(QStringLiteral("brand"));
    m_viewTitle = new QLabel(top);
    m_viewTitle->setObjectName(QStringLiteral("viewTitle"));
    m_search = new QLineEdit(top);
    m_search->setObjectName(QStringLiteral("search"));
    m_search->setPlaceholderText(tr("Search library  (Ctrl+F)"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(Icons::icon(Icons::Search), QLineEdit::LeadingPosition);
    m_search->setFixedWidth(300);
    connect(m_search, &QLineEdit::textEdited, this, [this] {
        if (m_view != View::Library)
            switchView(View::Library);
        m_searchTimer.start();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (t.isEmpty())
            m_searchTimer.start();
    });
    th->addWidget(menuBtn);
    th->addWidget(logo);
    th->addWidget(brand);
    th->addSpacing(28);
    th->addWidget(m_viewTitle);
    th->addStretch();
    th->addWidget(m_search);
    root->addWidget(top);

    // --- navigator
    m_nav = new QTreeWidget(central);
    m_nav->setObjectName(QStringLiteral("navigator"));
    m_nav->setHeaderHidden(true);
    m_nav->setRootIsDecorated(false);
    m_nav->setIndentation(0);
    m_nav->setIconSize(QSize(16, 16));
    m_nav->setMinimumWidth(150);
    m_nav->setContextMenuPolicy(Qt::CustomContextMenu);
    m_nav->setFocusPolicy(Qt::NoFocus);
    m_nav->viewport()->setAcceptDrops(true);
    m_nav->viewport()->installEventFilter(this); // tracks dropped on playlists
    connect(m_nav, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        const QString kind = it->data(0, NavKind).toString();
        if (kind == QLatin1String("library"))
            switchView(View::Library);
        else if (kind == QLatin1String("queue"))
            switchView(View::NowPlaying);
        else if (kind == QLatin1String("radio"))
            switchView(View::Radio);
        else if (kind == QLatin1String("playlist"))
            switchView(View::Playlist, it->data(0, NavName).toString());
        else if (kind == QLatin1String("newplaylist"))
            newPlaylist();
    });
    connect(m_nav, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *it = m_nav->itemAt(pos);
        QMenu menu(this);
        if (it && it->data(0, NavKind).toString() == QLatin1String("playlist")) {
            const QString name = it->data(0, NavName).toString();
            menu.addAction(Icons::icon(Icons::Play), tr("Play"), this, [this, name] { m_pc->playTracks(m_lib->playlistTracks(name), 0); });
            menu.addAction(Icons::icon(Icons::Queue), tr("Add to Now Playing"), this, [this, name] { m_pc->enqueue(m_lib->playlistTracks(name)); });
            menu.addSeparator();
            menu.addAction(Icons::icon(Icons::Edit), tr("Rename playlist…"), this, [this, name] { renamePlaylist(name); });
            menu.addAction(Icons::icon(Icons::Remove), tr("Delete playlist"), this, [this, name] { deletePlaylist(name); });
            menu.addSeparator();
        }
        // anywhere in the sidebar
        menu.addAction(Icons::icon(Icons::Add), tr("New playlist…"), this, [this] { newPlaylist(); });
        menu.addAction(Icons::icon(Icons::Import), tr("Import playlist…"), this, [this] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Import"), QString(), tr("Playlists (*.m3u *.m3u8 *.pls)"));
            if (!f.isEmpty())
                importPlaylistFile(f);
        });
        menu.exec(m_nav->viewport()->mapToGlobal(pos));
    });

    // --- library page
    m_libraryPage = new QWidget(central);
    auto *lv = new QVBoxLayout(m_libraryPage);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);
    // Library tabs, like a browser's: drag to reorder, + for a new one, × to close.
    // Each music folder belongs to one tab.
    m_tabRow = new QWidget(m_libraryPage);
    m_tabRow->setObjectName(QStringLiteral("libTabRow"));
    auto *tabRowLayout = new QHBoxLayout(m_tabRow);
    tabRowLayout->setContentsMargins(0, 0, 0, 0);
    tabRowLayout->setSpacing(2);
    m_libTabs = new QTabBar(m_tabRow);
    m_libTabs->setObjectName(QStringLiteral("libTabs"));
    m_libTabs->setDrawBase(false);
    m_libTabs->setExpanding(false);
    m_libTabs->setMovable(true);
    m_libTabs->setUsesScrollButtons(true);
    m_libTabs->setElideMode(Qt::ElideRight);
    m_libTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    m_libTabs->installEventFilter(this); // middle-click closes a tab
    auto *addTab = new QToolButton(m_tabRow);
    addTab->setObjectName(QStringLiteral("libTabAdd"));
    addTab->setIcon(Icons::icon(Icons::Add));
    addTab->setAutoRaise(true);
    addTab->setToolTip(tr("New tab"));
    connect(addTab, &QToolButton::clicked, this, &MainWindow::newLibraryTab);
    tabRowLayout->addWidget(m_libTabs);
    tabRowLayout->addWidget(addTab);
    tabRowLayout->addStretch(1);
    lv->addWidget(m_tabRow);
    connect(m_libTabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i < 0 || m_tabsUpdating)
            return;
        QSettings(paths::configFile(), QSettings::IniFormat).setValue(QStringLiteral("library/currentTab"), tabToStored(m_libTabs->tabText(i)));
        refreshLibrary(0);
    });
    connect(m_libTabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (m_tabsUpdating || from < 0 || to < 0 || from >= m_tabNames.size() || to >= m_tabNames.size())
            return;
        pinFolderTabs(); // folders follow their tab, not whichever tab ends up first
        m_tabNames.move(from, to);
        saveLibraryTabs();
    });
    connect(m_libTabs, &QTabBar::tabBarDoubleClicked, this, [this](int i) {
        if (i >= 0)
            renameLibraryTab(i);
        else
            newLibraryTab(); // double-click on the empty bar, as in browsers
    });
    connect(m_libTabs, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int i = m_libTabs->tabAt(pos);
        QMenu menu(this);
        menu.addAction(Icons::icon(Icons::Add), tr("New tab"), this, &MainWindow::newLibraryTab);
        if (i >= 0) {
            menu.addAction(Icons::icon(Icons::Folder), tr("Add music folder to this tab…"), this, [this, i] {
                m_libTabs->setCurrentIndex(i);
                addFolder();
            });
            menu.addAction(Icons::icon(Icons::Edit), tr("Rename tab…"), this, [this, i] { renameLibraryTab(i); });
            QAction *close = menu.addAction(Icons::icon(Icons::Close), tr("Close tab"), this, [this, i] { closeLibraryTab(i); });
            close->setEnabled(m_tabNames.size() > 1);
        }
        menu.addSeparator();
        menu.addAction(tr("Tabs and folders…"), this, [this] { openSettings(QStringLiteral("library")); });
        menu.exec(m_libTabs->mapToGlobal(pos));
    });
    m_libSplit = new QSplitter(Qt::Vertical, m_libraryPage);
    m_libSplit->setChildrenCollapsible(false);
    m_browser = new QWidget(m_libSplit);
    auto *bl = new QHBoxLayout(m_browser);
    bl->setContentsMargins(0, 0, 0, 0);
    m_browserSplit = new QSplitter(Qt::Horizontal, m_browser);
    bl->addWidget(m_browserSplit);
    auto makeBrowser = [this](BrowserModel *model, int level) {
        auto *v = new QListView(m_browserSplit);
        v->setObjectName(QStringLiteral("browser"));
        v->setModel(model);
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setUniformItemSizes(true);
        v->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        v->setTextElideMode(Qt::ElideRight);
        connect(v->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this, level] {
            if (!m_restoringSelection)
                refreshLibrary(level);
        });
        connect(v, &QListView::doubleClicked, this, [this] {
            m_pc->playTracks(m_trackModel->ids(), 0);
        });
        return v;
    };
    m_genreView = makeBrowser(m_genreModel, 1);
    m_artistView = makeBrowser(m_artistModel, 2);
    m_albumView = makeBrowser(m_albumModel, 3);
    m_browserSplit->setSizes({160, 220, 260});

    m_trackView = new QTableView(m_libSplit);
    m_trackView->setModel(m_trackModel);
    m_trackView->verticalHeader()->hide();
    m_trackView->verticalHeader()->setDefaultSectionSize(24);
    m_trackView->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_trackView->setShowGrid(false);
    m_trackView->setAlternatingRowColors(true);
    m_trackView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_trackView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_trackView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_trackView->setWordWrap(false);
    m_trackView->setTextElideMode(Qt::ElideRight);
    m_trackView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabHint = new QWidget(m_trackView->viewport());
    {
        auto *hl = new QVBoxLayout(m_tabHint);
        hl->addStretch(1);
        auto *text = new QLabel(tr("This tab has no music folders yet."), m_tabHint);
        text->setObjectName(QStringLiteral("hint"));
        text->setAlignment(Qt::AlignCenter);
        auto *btn = new QPushButton(Icons::icon(Icons::Folder), tr("Add music folder to this tab…"), m_tabHint);
        connect(btn, &QPushButton::clicked, this, &MainWindow::addFolder);
        auto *sub = new QLabel(tr("Double-click the tab to rename it; drag it to move it."), m_tabHint);
        sub->setObjectName(QStringLiteral("hint"));
        sub->setAlignment(Qt::AlignCenter);
        hl->addWidget(text);
        hl->addWidget(btn, 0, Qt::AlignHCenter);
        hl->addWidget(sub);
        hl->addStretch(2);
        m_tabHint->hide();
        m_trackView->viewport()->installEventFilter(this); // keep the hint the viewport's size
    }
    m_trackView->setDragEnabled(true); // onto a playlist in the sidebar, or out to other apps
    m_trackView->setDragDropMode(QAbstractItemView::DragOnly);
    m_trackView->setDefaultDropAction(Qt::CopyAction);
    m_trackView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    QHeaderView *hh = m_trackView->horizontalHeader();
    hh->setSectionsClickable(true);
    hh->setSectionsMovable(true);
    hh->setHighlightSections(false);
    hh->setSortIndicatorShown(true);
    hh->setSortIndicator(-1, Qt::AscendingOrder);
    hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hh->setMinimumSectionSize(36);
    hh->resizeSection(TrackTableModel::ColNum, 44);
    hh->resizeSection(TrackTableModel::ColArtist, 190);
    hh->resizeSection(TrackTableModel::ColAlbum, 210);
    hh->resizeSection(TrackTableModel::ColYear, 56);
    hh->resizeSection(TrackTableModel::ColGenre, 110);
    hh->resizeSection(TrackTableModel::ColTime, 62);
    hh->resizeSection(TrackTableModel::ColPlays, 52);
    hh->resizeSection(TrackTableModel::ColAdded, 96);
    for (int c : {int(TrackTableModel::ColTitle), int(TrackTableModel::ColArtist), int(TrackTableModel::ColAlbum)})
        hh->setSectionResizeMode(c, QHeaderView::Stretch);
    fitNarrowColumns();
    hh->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(hh, &QHeaderView::customContextMenuRequested, this, [this, hh](const QPoint &pos) {
        QMenu menu(this);
        for (int c = 0; c < TrackTableModel::ColCount; ++c) {
            if (c == TrackTableModel::ColTitle)
                continue;
            QAction *a = menu.addAction(m_trackModel->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
            a->setCheckable(true);
            a->setChecked(!hh->isSectionHidden(c));
            connect(a, &QAction::toggled, hh, [hh, c](bool on) { hh->setSectionHidden(c, !on); });
        }
        menu.exec(hh->mapToGlobal(pos));
    });
    connect(hh, &QHeaderView::sectionClicked, this, [this, hh](int col) {
        if (m_view != View::Library) {
            hh->setSortIndicator(-1, Qt::AscendingOrder);
            return;
        }
        Qt::SortOrder order = Qt::AscendingOrder;
        if (m_trackModel->sortColumn() == col)
            order = m_trackModel->sortOrder() == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
        // a third click on the same column returns to the album order
        if (m_trackModel->sortColumn() == col && m_trackModel->sortOrder() == Qt::DescendingOrder) {
            m_trackModel->sort(-1, Qt::AscendingOrder);
            hh->setSortIndicator(-1, Qt::AscendingOrder);
            return;
        }
        m_trackModel->sort(col, order);
        hh->setSortIndicator(col, order);
    });
    connect(m_trackView, &QTableView::doubleClicked, this, [this](const QModelIndex &i) { playFromView(i.row()); });
    connect(m_trackView, &QTableView::customContextMenuRequested, this, &MainWindow::trackContextMenu);
    m_libSplit->setStretchFactor(0, 0);
    m_libSplit->setStretchFactor(1, 1);
    m_libSplit->setSizes({200, 600});
    lv->addWidget(m_libSplit, 1);

    auto *statusRow = new QWidget(m_libraryPage);
    statusRow->setObjectName(QStringLiteral("statusBar"));
    statusRow->setAttribute(Qt::WA_StyledBackground);
    auto *sh = new QHBoxLayout(statusRow);
    sh->setContentsMargins(4, 2, 8, 2);
    m_status = new QLabel(statusRow);
    m_status->setObjectName(QStringLiteral("status"));
    m_scanBar = new QProgressBar(statusRow);
    m_scanBar->setTextVisible(false);
    m_scanBar->setFixedSize(140, 4);
    m_scanBar->hide();
    m_flash = new QLabel(statusRow);
    m_flash->setObjectName(QStringLiteral("status"));
    sh->addWidget(m_status);
    sh->addWidget(m_scanBar);
    sh->addStretch();
    sh->addWidget(m_flash);
    lv->addWidget(statusRow);

    // --- radio page
    m_radioPage = new QWidget(central);
    auto *rv = new QVBoxLayout(m_radioPage);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);
    auto *rbar = new QWidget(m_radioPage);
    rbar->setObjectName(QStringLiteral("statusBar"));
    rbar->setAttribute(Qt::WA_StyledBackground);
    auto *rh = new QHBoxLayout(rbar);
    rh->setContentsMargins(6, 4, 8, 4);
    auto *addBtn = toolButton(Icons::Add, tr("Add station"), rbar);
    auto *impFile = toolButton(Icons::Import, tr("Import M3U / M3U8 / PLS"), rbar);
    auto *impUrl = toolButton(Icons::Import, tr("Import from URL"), rbar);
    auto *editBtn = toolButton(Icons::Edit, tr("Edit"), rbar);
    auto *delBtn = toolButton(Icons::Remove, tr("Remove"), rbar);
    auto *expBtn = toolButton(Icons::Playlist, tr("Export"), rbar);
    expBtn->setToolTip(tr("Save the station list as M3U8, logos included"));
    connect(expBtn, &QToolButton::clicked, this, &MainWindow::exportStations);
    connect(addBtn, &QToolButton::clicked, this, &MainWindow::addStation);
    connect(impFile, &QToolButton::clicked, this, &MainWindow::importStationsFile);
    connect(impUrl, &QToolButton::clicked, this, &MainWindow::importStationsUrl);
    connect(editBtn, &QToolButton::clicked, this, [this] { editStation(m_stationView->currentIndex().row()); });
    connect(delBtn, &QToolButton::clicked, this, [this] {
        const int row = m_stationView->currentIndex().row();
        if (row >= 0)
            m_stations->remove(row);
    });
    rh->addWidget(addBtn);
    rh->addWidget(impFile);
    rh->addWidget(impUrl);
    rh->addWidget(expBtn);
    rh->addStretch();
    rh->addWidget(editBtn);
    rh->addWidget(delBtn);
    rv->addWidget(rbar);
    m_stationView = new QTableView(m_radioPage);
    m_stationView->setModel(m_stationModel);
    m_stationView->verticalHeader()->hide();
    m_stationView->verticalHeader()->setDefaultSectionSize(36);
    m_stationView->setIconSize(QSize(28, 28));
    m_stationView->setShowGrid(false);
    m_stationView->setAlternatingRowColors(true);
    m_stationView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_stationView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_stationView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stationView->setWordWrap(false);
    m_stationView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_stationView->horizontalHeader()->resizeSection(0, 220);
    m_stationView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_stationView->horizontalHeader()->resizeSection(2, 300);
    m_stationView->horizontalHeader()->setHighlightSections(false);
    m_stationView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *emptyHint = new QLabel(tr("No stations yet.\n\nAdd one with its stream address, or import an M3U / M3U8 / PLS list.\n"
                                    "HLS (.m3u8) streams, Icecast and Shoutcast all work."), m_stationView->viewport());
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setObjectName(QStringLiteral("hint"));
    auto placeHint = [this, emptyHint] {
        emptyHint->setVisible(m_stations->list().isEmpty());
        emptyHint->setGeometry(m_stationView->viewport()->rect());
    };
    connect(m_stations, &Stations::changed, emptyHint, placeHint);
    m_stationView->viewport()->installEventFilter(this);
    m_stationHint = emptyHint;
    placeHint();
    connect(m_stationView, &QTableView::doubleClicked, this, [this](const QModelIndex &i) { playStationRow(i.row()); });
    connect(m_stationView, &QTableView::customContextMenuRequested, this, &MainWindow::stationContextMenu);
    rv->addWidget(m_stationView, 1);

    m_stack = new QStackedWidget(central);
    m_stack->addWidget(m_libraryPage);
    m_stack->addWidget(m_radioPage);

    m_np = new NowPlayingPanel(central);
    m_np->setMinimumWidth(220);
    m_np->list()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_np->list(), &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidget *l = m_np->list();
        if (!l->itemAt(pos))
            return;
        QMenu menu(this);
        if (m_pc->mode() == PlaybackController::Mode::Radio) {
            menu.addAction(tr("Copy title"), this, [l, pos] {
                QApplication::clipboard()->setText(l->itemAt(pos)->data(Qt::UserRole).toString());
            });
            menu.addAction(tr("Copy whole history"), this, [this] {
                QStringList lines;
                for (const auto &h : m_pc->history())
                    lines << h.when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + QLatin1Char('\t') + h.title;
                QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
            });
        } else {
            const int row = l->row(l->itemAt(pos));
            menu.addAction(Icons::icon(Icons::Play), tr("Play"), this, [this, row] { Q_EMIT m_np->itemActivated(row); });
            menu.addAction(Icons::icon(Icons::Remove), tr("Remove from Now Playing"), this, [this, row] {
                const QVector<int> up = m_pc->upcomingIndexes(60);
                if (row < up.size())
                    m_pc->removeFromQueue({up[row]});
            });
        }
        menu.exec(l->viewport()->mapToGlobal(pos));
    });

    m_mainSplit = new QSplitter(Qt::Horizontal, central);
    m_mainSplit->addWidget(m_nav);
    m_mainSplit->addWidget(m_stack);
    m_mainSplit->addWidget(m_np);
    m_mainSplit->setStretchFactor(0, 0);
    m_mainSplit->setStretchFactor(1, 1);
    m_mainSplit->setStretchFactor(2, 0);
    m_mainSplit->setCollapsible(1, false);
    m_mainSplit->setSizes({190, 900, 300});
    root->addWidget(m_mainSplit, 1);

    m_bar = new PlayerBar(central);
    root->addWidget(m_bar);
    setCentralWidget(central);

    // menu
    QMenu *menu = menuBtn->menu();
    menu->addAction(Icons::icon(Icons::Folder), tr("Add music folder…"), this, &MainWindow::addFolder);
    menu->addAction(Icons::icon(Icons::Search), tr("Rescan library"), this, [this] {
        if (m_lib->folders().isEmpty())
            addFolder();
        else
            m_lib->rescan();
    });
    menu->addAction(Icons::icon(Icons::Music), tr("Open files…"), this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open files"), QString(),
                                                                tr("Audio and playlists (*.mp3 *.flac *.ogg *.oga *.opus *.m4a *.m4b *.mp4 *.aac *.wav *.wv *.ape *.aif *.aiff *.wma *.mpc *.m3u *.m3u8 *.pls);;All files (*)"));
        if (!files.isEmpty())
            openFiles(files, true);
    });
    menu->addAction(Icons::icon(Icons::Import), tr("Import playlist or station list…"), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Import"), QString(), tr("Playlists (*.m3u *.m3u8 *.pls)"));
        if (!f.isEmpty())
            importPlaylistFile(f);
    });
    menu->addAction(Icons::icon(Icons::Playlist), tr("Save Now Playing as playlist…"), this, [this] {
        if (m_pc->queue().isEmpty()) {
            flash(tr("Now Playing is empty."));
            return;
        }
        const QString name = askPlaylistName(tr("Save playlist"));
        if (!name.isEmpty())
            m_lib->savePlaylist(name, m_pc->queue());
    });
    menu->addSeparator();
    QMenu *pluginMenu = menu->addMenu(tr("Plugins"));
    connect(menu, &QMenu::aboutToShow, pluginMenu, [this, pluginMenu] {
        pluginMenu->clear();
        const auto actions = m_plugins ? m_plugins->actions() : QVector<PluginHost::Action>();
        for (const auto &a : actions) {
            const QString plugin = a.plugin;
            const int ref = a.ref;
            pluginMenu->addAction(a.label, this, [this, plugin, ref] {
                if (m_plugins)
                    m_plugins->runAction(plugin, ref);
            });
        }
        if (!actions.isEmpty())
            pluginMenu->addSeparator();
        pluginMenu->addAction(tr("Manage plugins…"), this, [this] { openSettings(QStringLiteral("plugins")); });
    });
    menu->addAction(Icons::icon(Icons::Settings), tr("Settings…"), this, [this] { openSettings(); });
    menu->addAction(Icons::accentIcon(Icons::Logo), tr("About Ruche"), this, &MainWindow::showAbout);
    menu->addSeparator();
    QAction *quit = menu->addAction(tr("Quit"), this, &MainWindow::quitApp);
    quit->setShortcut(QKeySequence::Quit);
    addAction(quit);
}

void MainWindow::setupShortcuts()
{
    auto add = [this](const QKeySequence &k, auto fn) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, fn);
    };
    add(QKeySequence(Qt::Key_Space), [this] { m_pc->togglePause(); });
    add(QKeySequence(Qt::Key_MediaPlay), [this] { m_pc->togglePause(); });
    add(QKeySequence(Qt::Key_MediaTogglePlayPause), [this] { m_pc->togglePause(); });
    add(QKeySequence(Qt::Key_MediaNext), [this] { m_pc->next(); });
    add(QKeySequence(Qt::Key_MediaPrevious), [this] { m_pc->previous(); });
    add(QKeySequence(Qt::Key_MediaStop), [this] { m_pc->stop(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { m_pc->next(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_Left), [this] { m_pc->previous(); });
    add(QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] { m_bar->seekBy(5000); });
    add(QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] { m_bar->seekBy(-5000); });
    add(QKeySequence(Qt::CTRL | Qt::Key_Up), [this] { m_bar->setVolume(qMin(100, m_pc->volume() + 5)); });
    add(QKeySequence(Qt::CTRL | Qt::Key_Down), [this] { m_bar->setVolume(qMax(0, m_pc->volume() - 5)); });
    add(QKeySequence::Find, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    auto *enter = new QShortcut(QKeySequence(Qt::Key_Return), m_trackView, nullptr, nullptr, Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this] {
        if (m_trackView->currentIndex().isValid())
            playFromView(m_trackView->currentIndex().row());
    });
    auto *del = new QShortcut(QKeySequence::Delete, m_trackView, nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &MainWindow::removeSelected);
    auto *spaste = new QShortcut(QKeySequence::Paste, m_stationView, nullptr, nullptr, Qt::WidgetShortcut);
    connect(spaste, &QShortcut::activated, this, [this] { pasteLogo(m_stationView->currentIndex().row()); });
    auto *senter = new QShortcut(QKeySequence(Qt::Key_Return), m_stationView, nullptr, nullptr, Qt::WidgetShortcut);
    connect(senter, &QShortcut::activated, this, [this] { playStationRow(m_stationView->currentIndex().row()); });
}

// Asks for a playlist name; empty if cancelled. Refuses names a file can't have and
// names already taken (other than `current`, when renaming).
QString MainWindow::askPlaylistName(const QString &title, const QString &current)
{
    QString name = current;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, title, tr("Playlist name:"), QLineEdit::Normal, name, &ok).trimmed();
        if (!ok || name.isEmpty())
            return {};
        static const QRegularExpression bad(QStringLiteral(R"([/\\:*?"<>|]|^\.)"));
        if (name.contains(bad)) {
            QMessageBox::warning(this, title, tr("A playlist name can't contain / \\ : * ? \" < > | or start with a dot."));
            continue;
        }
        if (name.compare(current, Qt::CaseSensitive) != 0 && m_lib->playlists().contains(name, Qt::CaseInsensitive) &&
            name.compare(current, Qt::CaseInsensitive) != 0) {
            QMessageBox::warning(this, title, tr("There's already a playlist called “%1”.").arg(name));
            continue;
        }
        return name;
    }
}

void MainWindow::newPlaylist()
{
    const QString name = askPlaylistName(tr("New playlist"));
    if (name.isEmpty())
        return;
    m_lib->savePlaylist(name, {});
    switchView(View::Playlist, name);
    rebuildNavigator();
    flash(tr("Playlist “%1” created. Drag tracks onto it in the sidebar, or right-click tracks → Add to playlist.").arg(name));
}

void MainWindow::renamePlaylist(const QString &name)
{
    const QString to = askPlaylistName(tr("Rename playlist"), name);
    if (to.isEmpty() || to == name)
        return;
    if (!m_lib->renamePlaylist(name, to)) {
        QMessageBox::warning(this, tr("Rename playlist"), tr("Couldn't rename the playlist."));
        return;
    }
    if (m_view == View::Playlist && m_playlist == name)
        switchView(View::Playlist, to);
    rebuildNavigator();
}

void MainWindow::deletePlaylist(const QString &name)
{
    if (QMessageBox::question(this, tr("Delete playlist"), tr("Delete the playlist “%1”?").arg(name)) != QMessageBox::Yes)
        return;
    if (m_view == View::Playlist && m_playlist == name)
        switchView(View::Library);
    m_lib->deletePlaylist(name);
}

void MainWindow::rebuildNavigator()
{
    m_nav->clear();
    auto section = [this](const QString &label) {
        auto *it = new QTreeWidgetItem(m_nav, {label});
        it->setFlags(Qt::NoItemFlags);
        QFont f = it->font(0);
        f.setPointSizeF(f.pointSizeF() * 0.82);
        f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        it->setFont(0, f);
        it->setForeground(0, Theme::dim());
        return it;
    };
    auto entry = [this](Icons::Name icon, const QString &label, const QString &kind, const QString &name = QString()) {
        auto *it = new QTreeWidgetItem(m_nav, {label});
        it->setIcon(0, Icons::icon(icon));
        it->setData(0, NavKind, kind);
        it->setData(0, NavName, name);
        return it;
    };
    section(tr("LIBRARY"));
    entry(Icons::Music, tr("Music"), QStringLiteral("library"));
    entry(Icons::Queue, tr("Now Playing"), QStringLiteral("queue"));
    entry(Icons::Radio, tr("Radio"), QStringLiteral("radio"));
    const QStringList pls = m_lib->playlists();
    section(tr("PLAYLISTS"));
    for (const QString &p : pls)
        entry(Icons::Playlist, p, QStringLiteral("playlist"), p);
    {
        // always there, so making a playlist is one click away (right-click has more)
        QTreeWidgetItem *add = entry(Icons::Add, tr("New playlist…"), QStringLiteral("newplaylist"));
        add->setForeground(0, Theme::dim());
        add->setToolTip(0, tr("Right-click a playlist to rename or delete it"));
    }

    // reselect the current view
    for (int i = 0; i < m_nav->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_nav->topLevelItem(i);
        const QString kind = it->data(0, NavKind).toString();
        const bool match = (m_view == View::Library && kind == QLatin1String("library")) ||
                           (m_view == View::NowPlaying && kind == QLatin1String("queue")) ||
                           (m_view == View::Radio && kind == QLatin1String("radio")) ||
                           (m_view == View::Playlist && kind == QLatin1String("playlist") &&
                            it->data(0, NavName).toString() == m_playlist);
        if (match)
            m_nav->setCurrentItem(it);
    }
}

// Translated headers (Russian "Прослушивания", say) can be far wider than the
// English defaults; never let a fixed-width column cut its own label off.
void MainWindow::fitNarrowColumns()
{
    QHeaderView *hh = m_trackView->horizontalHeader();
    const QFontMetrics fm(hh->font());
    for (int c : {int(TrackTableModel::ColNum), int(TrackTableModel::ColYear), int(TrackTableModel::ColGenre),
                  int(TrackTableModel::ColTime), int(TrackTableModel::ColPlays), int(TrackTableModel::ColAdded)}) {
        const QString label = m_trackModel->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        const int need = fm.horizontalAdvance(label) + 26; // padding + sort arrow
        if (hh->sectionSize(c) < need)
            hh->resizeSection(c, need);
    }
}

// ---------------------------------------------------------------------------
// Views

void MainWindow::switchView(View v, const QString &playlist)
{
    QHeaderView *hh = m_trackView->horizontalHeader();
    if (m_view == View::Library && v != View::Library)
        m_headerStateLibrary = hh->saveState();
    const View old = m_view;
    m_view = v;
    m_playlist = playlist;
    m_tabRow->setVisible(v == View::Library);
    if (v != View::Library)
        m_tabHint->hide();
    switch (v) {
    case View::Library:
        m_stack->setCurrentWidget(m_libraryPage);
        m_browser->show();
        m_trackModel->setQueueMode(false);
        m_trackModel->setSortEnabled(true);
        if (old != View::Library && !m_headerStateLibrary.isEmpty())
            hh->restoreState(m_headerStateLibrary);
        fitNarrowColumns();
        hh->setSortIndicator(m_trackModel->sortColumn(), m_trackModel->sortOrder());
        m_viewTitle->setText(tr("Music"));
        refreshLibrary(0);
        m_trackModel->setPlaying(m_pc->currentTrackId());
        break;
    case View::NowPlaying:
        m_stack->setCurrentWidget(m_libraryPage);
        m_browser->hide();
        m_trackModel->setQueueMode(true);
        m_trackModel->setSortEnabled(false);
        hh->setSortIndicator(-1, Qt::AscendingOrder);
        m_trackModel->setIds(m_pc->queue(), false);
        m_trackModel->setPlaying(m_pc->currentTrackId(), m_pc->queueIndex());
        m_viewTitle->setText(tr("Now Playing"));
        if (m_pc->queueIndex() >= 0)
            m_trackView->scrollTo(m_trackModel->index(m_pc->queueIndex(), 0), QAbstractItemView::PositionAtCenter);
        break;
    case View::Playlist:
        m_stack->setCurrentWidget(m_libraryPage);
        m_browser->hide();
        m_trackModel->setQueueMode(false);
        m_trackModel->setSortEnabled(false);
        hh->setSortIndicator(-1, Qt::AscendingOrder);
        m_trackModel->setIds(m_lib->playlistTracks(playlist), false);
        m_trackModel->setPlaying(m_pc->currentTrackId());
        m_viewTitle->setText(playlist);
        break;
    case View::Radio:
        m_stack->setCurrentWidget(m_radioPage);
        m_viewTitle->setText(tr("Radio"));
        break;
    }
    rebuildNavigator();
    updateStatus();
}

void MainWindow::showRadio() { switchView(View::Radio); }

QSet<QString> MainWindow::selectedKeys(QListView *view, BrowserModel *model) const
{
    QSet<QString> keys;
    for (const QModelIndex &i : view->selectionModel()->selectedRows()) {
        if (i.row() == 0)
            return {};
        keys.insert(model->keyAt(i.row()));
    }
    return keys;
}

void MainWindow::restoreSelection(QListView *view, BrowserModel *model, const QSet<QString> &keys)
{
    m_restoringSelection = true;
    QItemSelectionModel *sm = view->selectionModel();
    sm->clearSelection();
    bool any = false;
    for (const QString &k : keys) {
        const int row = model->rowForKey(k);
        if (row > 0) {
            sm->select(model->index(row), QItemSelectionModel::Select);
            if (!any)
                view->scrollTo(model->index(row));
            any = true;
        }
    }
    if (!any)
        sm->select(model->index(0), QItemSelectionModel::Select);
    m_restoringSelection = false;
}

static QVector<BrowserModel::Entry> group(Library *lib, const QVector<qint64> &ids, QString (*key)(const Track &), int kind)
{
    QHash<QString, BrowserModel::Entry> map;
    for (qint64 id : ids) {
        const Track *t = lib->byId(id);
        if (!t)
            continue;
        const QString k = key(*t);
        auto it = map.find(k);
        if (it == map.end()) {
            BrowserModel::Entry e;
            e.key = k;
            e.display = kind == 2 ? t->album : k;
            e.count = 1;
            map.insert(k, e);
        } else {
            it->count++;
        }
    }
    QVector<BrowserModel::Entry> out = map.values().toVector();
    if (kind == 2) {
        // same album title from different artists: say whose it is
        QHash<QString, int> seen;
        for (const auto &e : out)
            seen[e.display]++;
        for (auto &e : out)
            if (seen.value(e.display) > 1 && !e.display.isEmpty())
                e.display += QStringLiteral("  (%1)").arg(e.key.section(QChar(0x1f), 0, 0));
    }
    const QCollator coll = makeCollator();
    std::sort(out.begin(), out.end(), [&coll](const BrowserModel::Entry &a, const BrowserModel::Entry &b) {
        if (a.display.isEmpty() != b.display.isEmpty())
            return b.display.isEmpty(); // unknowns last
        return coll.compare(a.display, b.display) < 0;
    });
    return out;
}

static QVector<qint64> filterBy(Library *lib, const QVector<qint64> &ids, const QSet<QString> &keys,
                                QString (*key)(const Track &))
{
    if (keys.isEmpty())
        return ids;
    QVector<qint64> out;
    for (qint64 id : ids) {
        const Track *t = lib->byId(id);
        if (t && keys.contains(key(*t)))
            out.push_back(id);
    }
    return out;
}

void MainWindow::updateTabHint()
{
    const QStringList tf = currentTabFolders();
    const bool empty = m_view == View::Library && m_tabNames.size() > 1 && tf.size() == 1 && tf.first() == QStringLiteral("\x01");
    m_tabHint->setGeometry(m_trackView->viewport()->rect());
    m_tabHint->setVisible(empty);
}

void MainWindow::refreshLibrary(int from)
{
    if (m_view != View::Library)
        return;
    QSet<QString> gSel = selectedKeys(m_genreView, m_genreModel);
    QSet<QString> aSel = selectedKeys(m_artistView, m_artistModel);
    QSet<QString> alSel = selectedKeys(m_albumView, m_albumModel);

    if (from <= 0) {
        m_base.clear();
        const QStringList words = fold(m_search->text()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QHash<QString, QString> cache; // artist/album/genre strings repeat a lot
        auto cached = [&cache](const QString &s) -> QString {
            auto it = cache.constFind(s);
            if (it != cache.constEnd())
                return *it;
            const QString f = fold(s);
            cache.insert(s, f);
            return f;
        };
        const QStringList tabFolders = currentTabFolders(); // empty: every folder
        updateTabHint();
        for (const Track &t : m_lib->tracks()) {
            if (t.id <= 0)
                continue;
            if (!tabFolders.isEmpty()) {
                const QString path = t.filePath();
                bool inTab = false;
                for (const QString &f : tabFolders)
                    if (path.startsWith(f, Qt::CaseSensitivity(kPathCase))) {
                        inTab = true;
                        break;
                    }
                if (!inTab)
                    continue;
            }
            if (!words.isEmpty()) {
                const QString hay = fold(t.displayTitle()) + QLatin1Char(' ') + cached(t.artist) + QLatin1Char(' ') +
                                    cached(t.album) + QLatin1Char(' ') + cached(t.albumArtist) + QLatin1Char(' ') + cached(t.genre);
                bool ok = true;
                for (const QString &w : words)
                    if (!hay.contains(w)) {
                        ok = false;
                        break;
                    }
                if (!ok)
                    continue;
            }
            m_base.push_back(t.id);
        }
        m_genreModel->setEntries(group(m_lib, m_base, genreKey, 0));
        restoreSelection(m_genreView, m_genreModel, gSel);
        gSel = selectedKeys(m_genreView, m_genreModel);
    }
    if (from <= 1) {
        m_afterGenre = filterBy(m_lib, m_base, gSel, genreKey);
        m_artistModel->setEntries(group(m_lib, m_afterGenre, artistKey, 1));
        restoreSelection(m_artistView, m_artistModel, aSel);
        aSel = selectedKeys(m_artistView, m_artistModel);
    }
    if (from <= 2) {
        m_afterArtist = filterBy(m_lib, m_afterGenre, aSel, artistKey);
        m_albumModel->setEntries(group(m_lib, m_afterArtist, albumKey, 2));
        restoreSelection(m_albumView, m_albumModel, alSel);
        alSel = selectedKeys(m_albumView, m_albumModel);
    }
    m_trackModel->setIds(filterBy(m_lib, m_afterArtist, alSel, albumKey));
    m_trackModel->setPlaying(m_pc->currentTrackId());
    updateStatus();
}

void MainWindow::updateStatus()
{
    if (m_lib->isScanning())
        return;
    if (m_lib->count() == 0 && m_lib->folders().isEmpty()) {
        m_status->setText(tr("No music folder yet. Use the menu (☰) → Add music folder…"));
        return;
    }
    qint64 ms = 0;
    for (qint64 id : m_trackModel->ids())
        if (const Track *t = m_lib->byId(id))
            ms += t->durationMs;
    m_status->setText(countOf(m_trackModel->rowCount(), tr("track"), tr("tracks")) + QStringLiteral("  ·  ") + formatLong(ms));
}

// ---------------------------------------------------------------------------
// Playback UI

void MainWindow::playFromView(int row)
{
    if (row < 0)
        return;
    if (m_view == View::NowPlaying)
        m_pc->playQueueIndex(row);
    else
        m_pc->playTracks(m_trackModel->ids(), row);
}

void MainWindow::onTrackChanged(qint64 id)
{
    m_trackModel->setPlaying(id, m_view == View::NowPlaying ? m_pc->queueIndex() : -1);
    if (id != 0 && id != m_coverFor) {
        m_coverFor = id;
        if (const Track *t = m_lib->byId(id)) {
            m_artToken = m_art->request(t->filePath(), 400);
            m_artPending = true;
        }
    } else if (id == 0) {
        m_logoKey.clear();
        m_coverFor = 0;
        m_coverImage = QImage();
        ++m_artToken;
        m_np->setCover(QImage(), m_pc->mode() == PlaybackController::Mode::Radio ? Icons::Radio : Icons::Music);
        m_bar->setCover(QImage(), m_pc->mode() == PlaybackController::Mode::Radio ? Icons::Radio : Icons::Music);
        if (m_mpris)
            m_mpris->setArtFile(QString());
    }
    updateNowPlaying();
    updateUpNextOrHistory();
}

void MainWindow::onArt(int token, const QImage &img)
{
    if (token != m_artToken)
        return;
    m_artPending = false;
    m_coverImage = img;
    if (m_notifyPending)
        notifyNow();
    m_np->setCover(img, Icons::Music);
    m_bar->setCover(img.isNull() ? img : img.scaled(112, 112, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation), Icons::Music);
    if (m_mpris && m_mpris->isRegistered()) {
        QString file;
        if (!img.isNull()) {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
            file = (dir.isEmpty() ? QDir::tempPath() : dir) + QStringLiteral("/ruche-cover-%1-%2.jpg")
                                                                  .arg(QCoreApplication::applicationPid())
                                                                  .arg(token % 2);
            img.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(file, "JPG", 88);
        }
        m_mpris->setArtFile(file);
    }
}

QString MainWindow::techLine() const
{
    if (m_pc->mode() == PlaybackController::Mode::Library) {
        const Track *t = m_lib->byId(m_pc->currentTrackId());
        if (!t)
            return {};
        QStringList parts;
        parts << t->suffix();
        if (t->sampleRate > 0)
            parts << QStringLiteral("%1 kHz").arg(t->sampleRate / 1000.0, 0, 'g', 3);
        if (t->bitrate > 0)
            parts << QStringLiteral("%1 kbps").arg(t->bitrate);
        if (t->channels == 1)
            parts << tr("mono");
        return parts.join(QStringLiteral("  ·  "));
    }
    if (m_pc->mode() == PlaybackController::Mode::Radio) {
        const QVariantMap a = m_pc->audioParams();
        QStringList parts;
        if (a.contains(QStringLiteral("codec")))
            parts << a.value(QStringLiteral("codec")).toString().toUpper();
        const double br = a.value(QStringLiteral("bitrate")).toDouble();
        if (br > 0)
            parts << QStringLiteral("%1 kbps").arg(qRound(br / 1000));
        const qint64 sr = a.value(QStringLiteral("samplerate")).toLongLong();
        if (sr > 0)
            parts << QStringLiteral("%1 kHz").arg(sr / 1000.0, 0, 'g', 3);
        return parts.join(QStringLiteral("  ·  "));
    }
    return {};
}

void MainWindow::updateNowPlaying()
{
    const auto mode = m_pc->mode();
    if (mode == PlaybackController::Mode::Library) {
        const Track *t = m_lib->byId(m_pc->currentTrackId());
        if (t) {
            QString album = t->album;
            if (t->year > 0)
                album += album.isEmpty() ? QString::number(t->year) : QStringLiteral(" (%1)").arg(t->year);
            m_np->setInfo(t->displayTitle(), t->artist, album, techLine());
            m_logoKey.clear();
            m_bar->setTrack(t->displayTitle(), t->artist.isEmpty() ? t->album : t->artist);
            setWindowTitle(QStringLiteral("%1 – %2 · Ruche").arg(t->displayTitle(), t->artist));
        }
    } else if (mode == PlaybackController::Mode::Radio) {
        const QString title = m_pc->streamTitle();
        const QString station = m_pc->stationName().isEmpty() ? m_pc->stationUrl().host() : m_pc->stationName();
        const QString kind = m_pc->streamIsHls() ? tr("Live  ·  HLS") : tr("Live stream");
        m_np->setInfo(title.isEmpty() ? station : title, title.isEmpty() ? QString() : station, kind, techLine());
        const QString key = m_pc->stationUrl().toString();
        if (key != m_logoKey) {
            m_logoKey = key;
            const QString path = m_stations->logoPathForUrl(key);
            const QImage logo = path.isEmpty() ? QImage() : QImage(path);
            m_np->setLogo(logo);
            m_bar->setLogo(logo);
            if (m_mpris)
                m_mpris->setArtFile(path);
        }
        m_bar->setTrack(title.isEmpty() ? station : title, title.isEmpty() ? tr("Connecting…") : station);
        setWindowTitle(QStringLiteral("%1 · Ruche").arg(title.isEmpty() ? station : title));
    } else {
        m_np->setInfo(QString(), QString(), QString(), QString());
        m_bar->setTrack(QString(), QString());
        setWindowTitle(QStringLiteral("Ruche"));
    }
}

void MainWindow::updateUpNextOrHistory()
{
    QListWidget *l = m_np->list();
    l->clear();
    if (m_pc->mode() == PlaybackController::Mode::Radio) {
        m_np->setListTitle(tr("On air history"));
        for (const auto &h : m_pc->history()) {
            auto *it = new QListWidgetItem(h.when.toString(QStringLiteral("HH:mm")) + QStringLiteral("   ") + h.title, l);
            it->setData(Qt::UserRole, h.title);
            it->setToolTip(h.title);
        }
        return;
    }
    m_np->setListTitle(tr("Up next"));
    const QVector<int> up = m_pc->upcomingIndexes(60);
    for (int qi : up) {
        const Track *t = m_lib->byId(m_pc->queue().value(qi));
        if (!t)
            continue;
        auto *it = new QListWidgetItem(t->artist.isEmpty() ? t->displayTitle() : t->displayTitle() + QStringLiteral("  —  ") + t->artist, l);
        it->setToolTip(t->filePath());
    }
}

void MainWindow::onStateChanged()
{
    m_bar->setPlaying(m_pc->isActive(), m_pc->isPaused());
    m_bar->setShuffle(m_pc->shuffle());
    m_bar->setRepeat(int(m_pc->repeat()));
    m_bar->setQueueControlsEnabled(m_pc->mode() != PlaybackController::Mode::Radio);
    if (!m_pc->isActive()) {
        m_bar->setPosition(0, 0, false, false);
        updateNowPlaying();
    }
}

// ---------------------------------------------------------------------------
// Track actions

QVector<int> MainWindow::selectedRows() const
{
    QVector<int> rows;
    for (const QModelIndex &i : m_trackView->selectionModel()->selectedRows())
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    return rows;
}

QVector<qint64> MainWindow::selectedTrackIds() const
{
    QVector<qint64> ids;
    for (int r : selectedRows())
        ids << m_trackModel->idAt(r);
    return ids;
}

void MainWindow::addToPlaylistMenu(QMenu *menu, const QVector<qint64> &ids)
{
    QMenu *sub = menu->addMenu(Icons::icon(Icons::Playlist), tr("Add to playlist"));
    sub->addAction(Icons::icon(Icons::Add), tr("New playlist…"), this, [this, ids] {
        const QString name = askPlaylistName(tr("New playlist"));
        if (!name.isEmpty()) {
            m_lib->appendToPlaylist(name, ids);
            flash(tr("Added %1 to “%2”").arg(countOf(ids.size(), tr("track"), tr("tracks")), name));
        }
    });
    const QStringList pls = m_lib->playlists();
    if (!pls.isEmpty())
        sub->addSeparator();
    for (const QString &p : pls)
        sub->addAction(p, this, [this, p, ids] {
            m_lib->appendToPlaylist(p, ids);
            flash(tr("Added %1 to “%2”").arg(countOf(ids.size(), tr("track"), tr("tracks")), p));
        });
}

void MainWindow::trackContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_trackView->indexAt(pos);
    if (!idx.isValid())
        return;
    const QVector<qint64> ids = selectedTrackIds();
    if (ids.isEmpty())
        return;
    QMenu menu(this);
    menu.addAction(Icons::icon(Icons::Play), tr("Play"), this, [this, idx] { playFromView(idx.row()); });
    menu.addAction(Icons::icon(Icons::Next), tr("Play next"), this, [this, ids] { m_pc->playNext(ids); });
    menu.addAction(Icons::icon(Icons::Queue), tr("Add to Now Playing"), this, [this, ids] { m_pc->enqueue(ids); });
    addToPlaylistMenu(&menu, ids);
    menu.addSeparator();
    if (m_view == View::Playlist)
        menu.addAction(Icons::icon(Icons::Remove), tr("Remove from playlist"), this, &MainWindow::removeSelected);
    if (m_view == View::NowPlaying)
        menu.addAction(Icons::icon(Icons::Remove), tr("Remove from Now Playing"), this, &MainWindow::removeSelected);
    menu.addAction(Icons::icon(Icons::Folder), tr("Open containing folder"), this, [this, idx] {
        if (const Track *t = m_lib->byId(m_trackModel->idAt(idx.row())))
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(t->filePath()).absolutePath()));
    });
    menu.exec(m_trackView->viewport()->mapToGlobal(pos));
}

void MainWindow::removeSelected()
{
    const QVector<int> rows = selectedRows();
    if (rows.isEmpty())
        return;
    if (m_view == View::NowPlaying) {
        m_pc->removeFromQueue(rows);
    } else if (m_view == View::Playlist) {
        QVector<qint64> ids = m_trackModel->ids();
        for (int i = rows.size() - 1; i >= 0; --i)
            ids.removeAt(rows[i]);
        m_lib->savePlaylist(m_playlist, ids);
        m_trackModel->setIds(ids, false);
        updateStatus();
    }
}

// ---------------------------------------------------------------------------
// Radio

namespace {
struct StationForm {
    QString name, url, logoUrl;
    QImage logoImage;   // a new picture picked or pasted in the dialog
    bool logoCleared = false;
    bool logoUrlChanged = false;
};

QString imageFilter()
{
    return QObject::tr("Images (*.png *.jpg *.jpeg *.svg *.svgz *.webp *.ico *.gif *.bmp);;All files (*)");
}

bool stationDialog(QWidget *parent, const QString &title, StationForm *f, const QString &currentLogo)
{
    QDialog d(parent);
    d.setWindowTitle(title);
    d.setMinimumWidth(520);
    auto *form = new QFormLayout(&d);
    auto *n = new QLineEdit(f->name, &d);
    auto *u = new QLineEdit(f->url, &d);
    u->setPlaceholderText(QStringLiteral("https://… (Icecast, Shoutcast, .m3u8 HLS, .pls)"));
    form->addRow(QObject::tr("Name"), n);
    form->addRow(QObject::tr("Address"), u);

    auto *preview = new QLabel(&d);
    preview->setFixedSize(72, 72);
    preview->setAlignment(Qt::AlignCenter);
    auto *choose = new QPushButton(QObject::tr("Choose image…"), &d);
    auto *paste = new QPushButton(QObject::tr("Paste"), &d);
    auto *clear = new QPushButton(QObject::tr("Remove"), &d);
    auto *logoRow = new QHBoxLayout;
    logoRow->addWidget(preview);
    auto *btns = new QVBoxLayout;
    btns->addWidget(choose);
    btns->addWidget(paste);
    btns->addWidget(clear);
    logoRow->addLayout(btns);
    logoRow->addStretch();
    form->addRow(QObject::tr("Logo"), logoRow);
    auto *logoUrl = new QLineEdit(f->logoUrl, &d);
    logoUrl->setPlaceholderText(QObject::tr("or a web address: https://…/logo.png (optional)"));
    form->addRow(QString(), logoUrl);

    QImage shown = currentLogo.isEmpty() ? QImage() : QImage(currentLogo);
    auto refresh = [&] {
        if (shown.isNull()) {
            preview->setPixmap(QPixmap());
            preview->setText(QObject::tr("No logo"));
            preview->setStyleSheet(QStringLiteral("color: %1; border: 1px dashed %2; border-radius: 10px;").arg(Theme::faint().name(), Theme::mixed(0.13).name()));
        } else {
            preview->setStyleSheet(QString());
            preview->setPixmap(Logo::tileFromImage(shown, 72, d.devicePixelRatioF()));
        }
        clear->setEnabled(!shown.isNull() || !logoUrl->text().isEmpty());
    };
    QObject::connect(choose, &QPushButton::clicked, &d, [&] {
        const QString file = QFileDialog::getOpenFileName(&d, QObject::tr("Choose a logo"), QString(), imageFilter());
        if (file.isEmpty())
            return;
        QFile in(file);
        const QImage img = in.open(QIODevice::ReadOnly) ? Logo::decode(in.read(16 * 1024 * 1024)) : QImage();
        if (img.isNull()) {
            QMessageBox::warning(&d, QObject::tr("Logo"), QObject::tr("%1 is not an image Ruche can read.").arg(QFileInfo(file).fileName()));
            return;
        }
        f->logoImage = shown = img;
        f->logoCleared = false;
        refresh();
    });
    QObject::connect(paste, &QPushButton::clicked, &d, [&] {
        const QMimeData *m = QApplication::clipboard()->mimeData();
        if (m && m->hasImage()) {
            f->logoImage = shown = Logo::normalise(qvariant_cast<QImage>(m->imageData()));
            f->logoCleared = false;
        } else {
            const QString text = QApplication::clipboard()->text().trimmed();
            if (text.startsWith(QLatin1String("http")))
                logoUrl->setText(text);
        }
        refresh();
    });
    QObject::connect(clear, &QPushButton::clicked, &d, [&] {
        f->logoImage = shown = QImage();
        f->logoCleared = true;
        logoUrl->clear();
        refresh();
    });
    QObject::connect(logoUrl, &QLineEdit::textChanged, &d, [&] { refresh(); });
    refresh();

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    const QString originalLogoUrl = f->logoUrl;
    if (d.exec() != QDialog::Accepted || u->text().trimmed().isEmpty())
        return false;
    f->name = n->text().trimmed();
    f->url = u->text().trimmed();
    f->logoUrl = logoUrl->text().trimmed();
    f->logoUrlChanged = !f->logoUrl.isEmpty() && f->logoUrl != originalLogoUrl;
    return true;
}

void applyLogo(Stations *stations, int row, const StationForm &f)
{
    if (!f.logoImage.isNull())
        stations->setLogoFromImage(row, f.logoImage);
    else if (f.logoUrlChanged)
        stations->setLogoFromUrl(row, QUrl::fromUserInput(f.logoUrl));
    else if (f.logoCleared)
        stations->clearLogo(row);
}
} // namespace

void MainWindow::addStation()
{
    StationForm f;
    if (!stationDialog(this, tr("Add station"), &f, QString()))
        return;
    const int row = m_stations->add(f.name, QUrl::fromUserInput(f.url).toString());
    applyLogo(m_stations, row, f);
    m_stationView->selectRow(row);
}

void MainWindow::editStation(int row)
{
    if (row < 0 || row >= m_stations->list().size())
        return;
    const Station &s = m_stations->list()[row];
    StationForm f;
    f.name = s.name;
    f.url = s.url;
    f.logoUrl = s.logoUrl.startsWith(QLatin1String("http")) ? s.logoUrl : QString();
    if (!stationDialog(this, tr("Edit station"), &f, m_stations->logoPath(row)))
        return;
    m_stations->update(row, f.name, QUrl::fromUserInput(f.url).toString());
    applyLogo(m_stations, row, f);
}

void MainWindow::chooseLogoFile(int row)
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose a logo"), QString(), imageFilter());
    if (file.isEmpty())
        return;
    QString err;
    if (!m_stations->setLogoFromFile(row, file, &err))
        QMessageBox::warning(this, tr("Logo"), err);
}

void MainWindow::pasteLogo(int row)
{
    if (row < 0 || row >= m_stations->list().size())
        return;
    const QMimeData *m = QApplication::clipboard()->mimeData();
    if (!m)
        return;
    if (m->hasImage()) {
        m_stations->setLogoFromImage(row, qvariant_cast<QImage>(m->imageData()));
        return;
    }
    QUrl u;
    if (m->hasUrls() && !m->urls().isEmpty())
        u = m->urls().first();
    else if (m->text().trimmed().startsWith(QLatin1String("http")) || m->text().trimmed().startsWith(QLatin1Char('/')))
        u = QUrl::fromUserInput(m->text().trimmed());
    if (u.isValid())
        m_stations->setLogoFromUrl(row, u);
    else
        flash(tr("The clipboard has no image or image address."));
}

void MainWindow::exportStations()
{
    if (m_stations->list().isEmpty()) {
        flash(tr("No stations to export."));
        return;
    }
    const QString file = QFileDialog::getSaveFileName(this, tr("Export stations"),
                                                      QDir::home().filePath(QStringLiteral("stations.m3u8")),
                                                      tr("M3U8 station list (*.m3u8)"));
    if (file.isEmpty())
        return;
    QString err;
    if (m_stations->exportM3u(file, &err))
        flash(tr("Exported %1 with logos").arg(countOf(m_stations->list().size(), tr("station"), tr("stations"))));
    else
        QMessageBox::warning(this, tr("Export"), err);
}

void MainWindow::importStationsFile()
{
    const QString f = QFileDialog::getOpenFileName(this, tr("Import stations"), QString(), tr("Station lists (*.m3u *.m3u8 *.pls);;All files (*)"));
    if (f.isEmpty())
        return;
    const int n = m_stations->importFile(f);
    flash(n ? tr("Imported %1").arg(countOf(n, tr("station"), tr("stations"))) : tr("No new stations in that file."));
}

void MainWindow::importStationsUrl()
{
    bool ok = false;
    const QString url = QInputDialog::getText(this, tr("Import from URL"),
                                              tr("Address of an M3U/M3U8/PLS station list, an HLS stream or a stream:"),
                                              QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || url.isEmpty())
        return;
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_stations, &Stations::imported, this, [this, conn](int n, const QString &err) {
        disconnect(*conn);
        flash(n ? tr("Imported %1").arg(countOf(n, tr("station"), tr("stations"))) : err);
    });
    m_stations->importUrl(QUrl::fromUserInput(url));
}

void MainWindow::playStationRow(int row)
{
    if (row < 0 || row >= m_stations->list().size())
        return;
    const Station s = m_stations->list()[row];
    m_stationModel->setLive(s.url, QString());
    m_pc->playStation(s.name, QUrl(s.url));
}

void MainWindow::stationContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_stationView->indexAt(pos);
    if (!idx.isValid())
        return;
    const int row = idx.row();
    QMenu menu(this);
    menu.addAction(Icons::icon(Icons::Play), tr("Play"), this, [this, row] { playStationRow(row); });
    menu.addAction(Icons::icon(Icons::Edit), tr("Edit…"), this, [this, row] { editStation(row); });
    menu.addAction(tr("Copy address"), this, [this, row] { QApplication::clipboard()->setText(m_stations->list()[row].url); });
    menu.addSeparator();
    menu.addAction(Icons::icon(Icons::Folder), tr("Set logo from file…"), this, [this, row] { chooseLogoFile(row); });
    menu.addAction(Icons::icon(Icons::Import), tr("Set logo from web address…"), this, [this, row] {
        bool ok = false;
        const QString url = QInputDialog::getText(this, tr("Logo"), tr("Address of the logo image:"), QLineEdit::Normal,
                                                  QString(), &ok).trimmed();
        if (ok && !url.isEmpty())
            m_stations->setLogoFromUrl(row, QUrl::fromUserInput(url));
    });
    menu.addAction(tr("Paste logo"), this, [this, row] { pasteLogo(row); }, QKeySequence::Paste);
    if (!m_stations->logoPath(row).isEmpty())
        menu.addAction(tr("Remove logo"), this, [this, row] { m_stations->clearLogo(row); });
    menu.addSeparator();
    menu.addAction(Icons::icon(Icons::Remove), tr("Remove"), this, [this, row] { m_stations->remove(row); });
    menu.exec(m_stationView->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// Files, settings

void MainWindow::addFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Add music folder"),
                                                          QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    if (dir.isEmpty())
        return;
    QStringList f = m_lib->folders();
    if (!f.contains(dir))
        f << dir;
    if (m_libTabs->currentIndex() >= 0) { // into the tab being looked at
        m_folderTab.insert(dir, m_libTabs->tabText(m_libTabs->currentIndex()));
        saveLibraryTabs();
    }
    m_lib->setFolders(f);
    m_lib->rescan();
}

void MainWindow::openSettings(const QString &page)
{
    QDialog d(this);
    d.setWindowTitle(tr("Settings"));
    d.setMinimumSize(580, 440);
    auto *v = new QVBoxLayout(&d);
    auto *tabs = new QTabWidget(&d);
    v->addWidget(tabs, 1);
    QSettings s(paths::configFile(), QSettings::IniFormat);

    // --- General: language, updates
    auto *genPage = new QWidget;
    auto *gf = new QFormLayout(genPage);
    gf->setVerticalSpacing(12);
    auto *lang = new QComboBox(genPage);
    lang->addItem(tr("System language"), QString());
    for (const auto &l : i18n::languages())
        lang->addItem(l.second, l.first);
    const QString langBefore = s.value(QStringLiteral("ui/language")).toString();
    lang->setCurrentIndex(qMax(0, lang->findData(langBefore)));
    gf->addRow(tr("Language"), lang);
    auto *langHint = new QLabel(tr("Takes effect when Ruche restarts. The translations were made by machine and may "
                                   "have mistakes; corrections are welcome on GitHub."),
                                genPage);
    langHint->setObjectName(QStringLiteral("hint"));
    langHint->setWordWrap(true);
    gf->addRow(QString(), langHint);
    auto *updBox = new QCheckBox(tr("Check for a new version when Ruche starts"), genPage);
    updBox->setChecked(s.value(QStringLiteral("updates/onStartup"), false).toBool());
    auto *updRow = new QHBoxLayout;
    auto *checkNow = new QPushButton(tr("Check now"), genPage);
    updRow->addWidget(updBox);
    updRow->addStretch();
    updRow->addWidget(checkNow);
    gf->addRow(tr("Updates"), updRow);
    auto *updStatus = new QLabel(genPage);
    updStatus->setObjectName(QStringLiteral("hint"));
    updStatus->setWordWrap(true);
    updStatus->setText(tr("This copy: %1 (%2). Updates come from %3.")
                           .arg(QStringLiteral(RUCHE_VERSION), Updater::kindName(Updater::kind()), Updater::releasesPage()));
    gf->addRow(QString(), updStatus);
    connect(checkNow, &QPushButton::clicked, &d, [this, updStatus] { checkForUpdates(true, updStatus); });
    // Crash logs: only exist if Ruche crashed; size shown so they can't go unnoticed
    auto *crashRow = new QHBoxLayout;
    auto *crashInfo = new QLabel(genPage);
    auto *crashOpen = new QPushButton(Icons::icon(Icons::Folder), tr("Open folder"), genPage);
    auto *crashClear = new QPushButton(Icons::icon(Icons::Remove), tr("Delete all"), genPage);
    crashRow->addWidget(crashInfo, 1);
    crashRow->addWidget(crashOpen);
    crashRow->addWidget(crashClear);
    gf->addRow(tr("Crash logs"), crashRow);
    auto *crashHint = new QLabel(tr("Written only if Ruche crashes: what it was doing, to attach to a bug report on GitHub. "
                                    "At most the 20 newest are kept."),
                                 genPage);
    crashHint->setObjectName(QStringLiteral("hint"));
    crashHint->setWordWrap(true);
    gf->addRow(QString(), crashHint);
    auto showCrashInfo = [crashInfo, crashClear] {
        const int n = int(crashlog::logs().size());
        crashInfo->setText(n == 0 ? tr("None")
                                  : tr("%1 (%2)").arg(countOf(n, tr("log"), tr("logs")), QLocale().formattedDataSize(crashlog::totalSize())));
        crashClear->setEnabled(n > 0);
    };
    showCrashInfo();
    connect(crashOpen, &QPushButton::clicked, &d, [] {
        QDir().mkpath(crashlog::dir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(crashlog::dir()));
    });
    connect(crashClear, &QPushButton::clicked, &d, [showCrashInfo] {
        crashlog::clear();
        showCrashInfo();
    });
    tabs->addTab(genPage, tr("General"));

    // --- Library: tabs, and which folder goes in which
    auto *libPage = new QWidget;
    auto *lv = new QVBoxLayout(libPage);
    auto *tabsLbl = new QLabel(tr("Tabs"), libPage);
    tabsLbl->setObjectName(QStringLiteral("sectionLabel"));
    lv->addWidget(tabsLbl);
    auto *tabList = new QListWidget(libPage);
    tabList->setDragDropMode(QAbstractItemView::InternalMove);
    tabList->setMaximumHeight(110);
    int nextTabId = 0;
    for (const QString &t : m_tabNames) {
        auto *it = new QListWidgetItem(t, tabList);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        it->setData(Qt::UserRole, nextTabId++);
    }
    lv->addWidget(tabList);
    auto *tabRow = new QHBoxLayout;
    auto *addTab = new QPushButton(tr("Add tab"), libPage);
    auto *remTab = new QPushButton(tr("Remove tab"), libPage);
    tabRow->addWidget(addTab);
    tabRow->addWidget(remTab);
    tabRow->addStretch();
    lv->addLayout(tabRow);
    auto *tabsHint = new QLabel(tr("Tabs work like a browser's: + on the tab bar makes one, × or a middle-click closes one, "
                                   "double-click renames, and dragging reorders. Each folder below belongs to one tab."),
                                libPage);
    tabsHint->setObjectName(QStringLiteral("hint"));
    tabsHint->setWordWrap(true);
    lv->addWidget(tabsHint);
    auto *confirmClose = new QCheckBox(tr("Ask before closing a tab that has folders"), libPage);
    confirmClose->setChecked(s.value(QStringLiteral("library/confirmCloseTab"), true).toBool());
    lv->addWidget(confirmClose);
    lv->addSpacing(6);
    auto *lbl = new QLabel(tr("Music folders"), libPage);
    lbl->setObjectName(QStringLiteral("sectionLabel"));
    lv->addWidget(lbl);
    auto *list = new QTreeWidget(libPage);
    list->setColumnCount(2);
    list->setHeaderLabels({tr("Folder"), tr("Tab")});
    list->setRootIsDecorated(false);
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lv->addWidget(list, 1);
    // each folder's tab is a combo box holding tab ids, so renaming and reordering tabs keeps it
    auto fillCombo = [tabList](QComboBox *c) {
        const int keep = c->currentData().isValid() ? c->currentData().toInt() : -1;
        QSignalBlocker b(c);
        c->clear();
        for (int i = 0; i < tabList->count(); ++i)
            c->addItem(tabList->item(i)->text(), tabList->item(i)->data(Qt::UserRole));
        c->setCurrentIndex(qMax(0, c->findData(keep)));
    };
    auto addFolderRow = [list, tabList, fillCombo](const QString &folder, const QString &tab) {
        auto *it = new QTreeWidgetItem(list, {QDir::toNativeSeparators(folder)});
        it->setData(0, Qt::UserRole, folder);
        auto *c = new QComboBox(list);
        fillCombo(c);
        for (int i = 0; i < tabList->count(); ++i)
            if (tabList->item(i)->text() == tab)
                c->setCurrentIndex(i);
        list->setItemWidget(it, 1, c);
    };
    for (const QString &f : m_lib->folders())
        addFolderRow(f, m_folderTab.value(f, m_tabNames.value(0)));
    auto refreshCombos = [list, fillCombo] {
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (auto *c = qobject_cast<QComboBox *>(list->itemWidget(list->topLevelItem(i), 1)))
                fillCombo(c);
    };
    connect(tabList, &QListWidget::itemChanged, &d, [refreshCombos](QListWidgetItem *it) {
        if (it->text().trimmed().isEmpty())
            it->setText(tr("Untitled"));
        refreshCombos();
    });
    connect(tabList->model(), &QAbstractItemModel::rowsMoved, &d, [refreshCombos] { refreshCombos(); });
    connect(addTab, &QPushButton::clicked, &d, [tabList, refreshCombos, nextTabId]() mutable {
        auto *it = new QListWidgetItem(tr("New tab"), tabList);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        it->setData(Qt::UserRole, 1000 + nextTabId++);
        refreshCombos();
        tabList->setCurrentItem(it);
        tabList->editItem(it);
    });
    connect(remTab, &QPushButton::clicked, &d, [tabList, refreshCombos] {
        if (tabList->count() <= 1 || !tabList->currentItem())
            return; // there's always one tab
        delete tabList->currentItem(); // its folders fall back to the first tab
        refreshCombos();
    });
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add folder…"), libPage);
    auto *rem = new QPushButton(tr("Remove folder"), libPage);
    row->addWidget(add);
    row->addWidget(rem);
    row->addStretch();
    lv->addLayout(row);
    connect(add, &QPushButton::clicked, &d, [&d, list, addFolderRow, tabList] {
        const QString dir = QFileDialog::getExistingDirectory(&d, QObject::tr("Add music folder"));
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == dir)
                return;
        if (!dir.isEmpty())
            addFolderRow(dir, tabList->currentItem() ? tabList->currentItem()->text() : QString());
    });
    connect(rem, &QPushButton::clicked, &d, [list] { delete list->currentItem(); });
    tabs->addTab(libPage, tr("Library"));

    // --- Playback
    auto *playPage = new QWidget;
    auto *pf = new QFormLayout(playPage);
    auto *rg = new QComboBox(playPage);
    rg->addItem(tr("Off"), QStringLiteral("no"));
    rg->addItem(tr("Track gain"), QStringLiteral("track"));
    rg->addItem(tr("Album gain"), QStringLiteral("album"));
    rg->setCurrentIndex(qMax(0, rg->findData(s.value(QStringLiteral("replaygain"), QStringLiteral("no")).toString())));
    pf->addRow(tr("ReplayGain"), rg);
    tabs->addTab(playPage, tr("Playback"));

    // --- Appearance: changes show at once; Cancel puts the old look back.
    const Theme::Choice before = Theme::loadChoice();
    auto choice = std::make_shared<Theme::Choice>(before);
    if (!choice->customWindow.isValid())
        choice->customWindow = Theme::window();
    if (!choice->customText.isValid())
        choice->customText = Theme::text();
    auto *lookPage = new QWidget;
    auto *af = new QFormLayout(lookPage);
    af->setVerticalSpacing(12);
    const qreal dpr = devicePixelRatioF();
    // A little preview tile for each theme: its background with the accent on it.
    auto themeIcon = [dpr](const QColor &bg, const QColor &fg, const QColor &acc, const QColor &bg2 = QColor()) {
        QPixmap pm(QSize(28, 18) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath r;
        r.addRoundedRect(QRectF(0.5, 0.5, 27, 17), 4, 4);
        p.fillPath(r, bg);
        if (bg2.isValid()) {
            p.save();
            p.setClipRect(QRectF(14, 0, 14, 18));
            p.fillPath(r, bg2);
            p.restore();
        }
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(4, 5, 9, 2.5), 1, 1);
        p.drawRoundedRect(QRectF(4, 10, 6, 2.5), 1, 1);
        p.setBrush(acc);
        p.drawEllipse(QRectF(17, 4.5, 8, 8));
        p.setPen(QPen(QColor(128, 128, 128, 110), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(r);
        return QIcon(pm);
    };
    auto *theme = new QComboBox(lookPage);
    theme->setIconSize(QSize(28, 18));
    const auto &presets = Theme::presets();
    for (const Theme::Preset &p : presets)
        theme->addItem(themeIcon(p.window, p.text, p.accent), p.name, p.id);
    theme->insertItem(2, themeIcon(presets[0].window, presets[0].text, presets[0].accent, presets[1].window),
                      tr("Match the system (dark or light)"), QStringLiteral("system"));
    theme->addItem(tr("Custom…"), QStringLiteral("custom"));
    theme->setCurrentIndex(qMax(0, theme->findData(before.theme)));
    af->addRow(tr("Theme"), theme);

    // Accent: the theme's own, a few ready-made ones, or any colour.
    const QVector<QPair<QColor, QString>> accents = {
        {QColor(0xf5, 0xa5, 0x24), tr("Honey")},  {QColor(0xff, 0x6b, 0x3d), tr("Coral")},
        {QColor(0xe8, 0x4a, 0x5f), tr("Red")},    {QColor(0xe8, 0x6f, 0xa3), tr("Rose")},
        {QColor(0x9b, 0x7b, 0xff), tr("Violet")}, {QColor(0x5a, 0xa9, 0xff), tr("Blue")},
        {QColor(0x3c, 0xc8, 0xb4), tr("Teal")},   {QColor(0x6c, 0xc3, 0x8b), tr("Green")},
    };
    auto *accentRow = new QHBoxLayout;
    accentRow->setSpacing(6);
    QVector<QToolButton *> dots;
    auto dotIcon = [dpr](const QColor &c, bool selected, bool split = false, const QColor &c2 = QColor()) {
        QPixmap pm(QSize(24, 24) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (selected) {
            p.setPen(QPen(Theme::text(), 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(1, 1, 22, 22));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        if (split) { // the theme's own accent: half accent, half background
            p.drawPie(QRectF(5, 5, 14, 14), 90 * 16, 180 * 16);
            p.setBrush(c2);
            p.drawPie(QRectF(5, 5, 14, 14), 270 * 16, 180 * 16);
            p.setPen(QPen(QColor(128, 128, 128, 140), 1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(5, 5, 14, 14));
        } else {
            p.drawEllipse(QRectF(5, 5, 14, 14));
        }
        return QIcon(pm);
    };
    auto *autoDot = new QToolButton(lookPage);
    autoDot->setToolTip(tr("The theme's own accent"));
    dots << autoDot;
    accentRow->addWidget(autoDot);
    for (const auto &a : accents) {
        auto *b = new QToolButton(lookPage);
        b->setToolTip(a.second);
        dots << b;
        accentRow->addWidget(b);
    }
    auto *moreAccent = new QPushButton(tr("Other…"), lookPage);
    accentRow->addSpacing(4);
    accentRow->addWidget(moreAccent);
    accentRow->addStretch();
    for (QToolButton *b : dots) {
        b->setIconSize(QSize(24, 24));
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
    }
    af->addRow(tr("Accent"), accentRow);

    auto *bgBtn = new QPushButton(lookPage);
    auto *fgBtn = new QPushButton(lookPage);
    for (QPushButton *b : {bgBtn, fgBtn}) {
        b->setObjectName(QStringLiteral("swatch"));
        b->setIconSize(QSize(18, 18));
    }
    auto *bgRow = new QHBoxLayout;
    bgRow->addWidget(bgBtn);
    bgRow->addStretch();
    auto *fgRow = new QHBoxLayout;
    fgRow->addWidget(fgBtn);
    fgRow->addStretch();
    af->addRow(tr("Background"), bgRow);
    af->addRow(tr("Text"), fgRow);
    auto *lookHint = new QLabel(tr("A custom theme is made from these two colours and the accent; every other shade is mixed from them."), lookPage);
    lookHint->setObjectName(QStringLiteral("hint"));
    lookHint->setWordWrap(true);
    af->addRow(QString(), lookHint);

    auto colorButton = [dpr](QPushButton *b, const QColor &c) {
        QPixmap pm(QSize(18, 18) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(128, 128, 128, 160), 1));
        p.setBrush(c);
        p.drawRoundedRect(QRectF(0.5, 0.5, 17, 17), 4, 4);
        p.end();
        b->setIcon(QIcon(pm));
        b->setText(c.name());
    };
    auto refreshLook = [=] {
        // the theme's own accent, for the first dot
        QColor own = Theme::Brand;
        for (const Theme::Preset &p : presets)
            if (p.id == choice->theme)
                own = p.accent;
        if (choice->theme == QLatin1String("system"))
            own = Theme::systemPrefersDark() ? presets[0].accent : presets[1].accent;
        bool matched = !choice->accent.isValid();
        autoDot->setIcon(dotIcon(own, matched, true, Theme::window()));
        for (int i = 0; i < accents.size(); ++i) {
            const bool sel = choice->accent.isValid() && choice->accent.rgb() == accents[i].first.rgb();
            matched = matched || sel;
            dots[i + 1]->setIcon(dotIcon(accents[i].first, sel));
        }
        moreAccent->setText(matched ? tr("Other…") : choice->accent.name());
        if (!matched) {
            QPixmap pm(QSize(14, 14) * dpr);
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(choice->accent);
            p.drawEllipse(QRectF(0, 0, 14, 14));
            p.end();
            moreAccent->setIcon(QIcon(pm));
        } else {
            moreAccent->setIcon(QIcon());
        }
        colorButton(bgBtn, choice->customWindow);
        colorButton(fgBtn, choice->customText);
        const bool custom = choice->theme == QLatin1String("custom");
        af->setRowVisible(bgRow, custom);
        af->setRowVisible(fgRow, custom);
        af->setRowVisible(lookHint, custom);
    };
    auto preview = [this, choice, refreshLook] {
        applyTheme(*choice);
        refreshLook();
    };
    connect(theme, &QComboBox::currentIndexChanged, &d, [=](int) {
        choice->theme = theme->currentData().toString();
        preview();
    });
    connect(autoDot, &QToolButton::clicked, &d, [=] {
        choice->accent = QColor();
        preview();
    });
    for (int i = 0; i < accents.size(); ++i)
        connect(dots[i + 1], &QToolButton::clicked, &d, [=] {
            choice->accent = accents[i].first;
            preview();
        });
    auto pick = [&d, preview](QColor *target, const QColor &start, const QString &title) {
        const QColor c = QColorDialog::getColor(start, &d, title);
        if (c.isValid()) {
            *target = c;
            preview();
        }
    };
    connect(moreAccent, &QPushButton::clicked, &d, [=] { pick(&choice->accent, Theme::accent(), tr("Accent colour")); });
    connect(bgBtn, &QPushButton::clicked, &d, [=] { pick(&choice->customWindow, choice->customWindow, tr("Background")); });
    connect(fgBtn, &QPushButton::clicked, &d, [=] { pick(&choice->customText, choice->customText, tr("Text")); });

    // Theme files: stylesheets layered over everything above (THEMES.md)
    auto *themeFile = new QComboBox(lookPage);
    auto fillThemeFiles = [themeFile, choice] {
        QSignalBlocker block(themeFile);
        themeFile->clear();
        themeFile->addItem(tr("None"), QString());
        for (const Theme::File &f : Theme::files()) {
            QString label = f.name;
            if (!f.author.isEmpty())
                label += QStringLiteral("  ·  ") + f.author;
            if (f.bundled)
                label += QStringLiteral("  (") + tr("example") + QLatin1Char(')');
            themeFile->addItem(label, f.id);
        }
        themeFile->setCurrentIndex(qMax(0, themeFile->findData(choice->file)));
    };
    fillThemeFiles();
    af->addRow(tr("Theme file"), themeFile);
    auto *fileInfo = new QLabel(lookPage);
    fileInfo->setObjectName(QStringLiteral("hint"));
    fileInfo->setWordWrap(true);
    af->addRow(QString(), fileInfo);
    auto *importTheme = new QPushButton(Icons::icon(Icons::Import), tr("Import…"), lookPage);
    auto *importMenu = new QMenu(importTheme);
    importTheme->setMenu(importMenu);
    auto *openThemes = new QPushButton(Icons::icon(Icons::Folder), tr("Open themes folder"), lookPage);
    auto *fileButtons = new QHBoxLayout;
    fileButtons->addWidget(importTheme);
    fileButtons->addWidget(openThemes);
    fileButtons->addStretch();
    af->addRow(QString(), fileButtons);
    auto *fileHint = new QLabel(tr("A theme file is a Qt stylesheet (.qss) that can restyle every part of Ruche and set its own colours. "
                                   "Import a .qss, a theme folder or a downloaded .zip; edits to the file in use show up straight away. "
                                   "How to make one: THEMES.md (shipped with Ruche and in the source)."),
                                lookPage);
    fileHint->setObjectName(QStringLiteral("hint"));
    fileHint->setWordWrap(true);
    af->addRow(QString(), fileHint);
    auto showFileInfo = [fileInfo, choice] {
        const Theme::File f = Theme::fileById(choice->file);
        QStringList parts;
        if (!f.description.isEmpty())
            parts << f.description;
        if (!f.version.isEmpty())
            parts << tr("Version %1").arg(f.version);
        if (f.window.isValid())
            parts << tr("Sets its own colours, so the theme above only matters for what it leaves alone.");
        fileInfo->setText(parts.join(QLatin1Char('\n')));
        fileInfo->setVisible(!parts.isEmpty());
    };
    showFileInfo();
    connect(themeFile, &QComboBox::currentIndexChanged, &d, [=](int) {
        choice->file = themeFile->currentData().toString();
        showFileInfo();
        preview();
    });
    auto doImport = [=, &d](const QString &src) {
        if (src.isEmpty())
            return;
        QString err;
        const QString id = Theme::importTheme(src, &err);
        if (id.isEmpty()) {
            QMessageBox::warning(&d, tr("Import theme"), err);
            return;
        }
        choice->file = id;
        fillThemeFiles();
        showFileInfo();
        preview();
    };
    importMenu->addAction(tr("Theme file or .zip…"), &d, [=, &d] {
        doImport(QFileDialog::getOpenFileName(&d, tr("Import theme"), QString(), tr("Themes (*.qss *.zip)")));
    });
    importMenu->addAction(tr("Theme folder…"), &d, [=, &d] { doImport(QFileDialog::getExistingDirectory(&d, tr("Import theme"))); });
    connect(openThemes, &QPushButton::clicked, &d, [=] {
        QDir().mkpath(Theme::userThemesDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(Theme::userThemesDir()));
        fillThemeFiles(); // pick up anything dropped in by hand when the dialog comes back
    });
    refreshLook();
    tabs->addTab(lookPage, tr("Appearance"));

    // --- Desktop: notifications, tray, applications menu
    auto *deskPage = new QWidget;
    auto *df = new QFormLayout(deskPage);
    auto *notify = new QComboBox(deskPage);
    notify->addItem(tr("Off"), QStringLiteral("off"));
    notify->addItem(tr("When Ruche isn't the active window"), QStringLiteral("background"));
    notify->addItem(tr("Always"), QStringLiteral("always"));
    notify->setCurrentIndex(qMax(0, notify->findData(m_notifyWhen)));
    df->addRow(tr("Song notifications"), notify);
    auto *notifyHint = new QLabel(tr("A pop-up with the title, artist and cover when a track starts or the radio title changes."), deskPage);
    notifyHint->setObjectName(QStringLiteral("hint"));
    notifyHint->setWordWrap(true);
    df->addRow(QString(), notifyHint);
    auto *trayBox = new QCheckBox(tr("Keep playing in the system tray when the window is closed"), deskPage);
    trayBox->setChecked(m_closeToTray);
    trayBox->setEnabled(m_closeToTray || QSystemTrayIcon::isSystemTrayAvailable());
    df->addRow(tr("Close to tray"), trayBox);
    QCheckBox *menuBox = nullptr;
#ifndef Q_OS_WIN
    // Only the AppImage needs this; the .deb/.rpm install their own menu entry.
    if (DesktopEntry::available()) {
        menuBox = new QCheckBox(tr("Show Ruche in the applications menu, so it can be pinned to the taskbar"), deskPage);
        menuBox->setChecked(DesktopEntry::isInstalled());
        df->addRow(tr("Applications menu"), menuBox);
    }
#endif
    // Discord: opt-in, says nothing to anyone until ticked
    auto *discordBox = new QCheckBox(tr("Show what I'm listening to on Discord"), deskPage);
    discordBox->setChecked(m_discord->isEnabled());
    df->addRow(tr("Discord"), discordBox);
    auto *discordRadio = new QCheckBox(tr("Include radio stations"), deskPage);
    discordRadio->setChecked(s.value(QStringLiteral("discord/radio"), true).toBool());
    df->addRow(QString(), discordRadio);
    auto *discordCovers = new QComboBox(deskPage);
    discordCovers->addItem(tr("Cover art (album cover, or the station's logo for radio)"), true);
    discordCovers->addItem(tr("Ruche logo"), false);
    discordCovers->setCurrentIndex(s.value(QStringLiteral("discord/covers"), true).toBool() ? 0 : 1);
    discordCovers->setToolTip(tr("Covers are looked up by artist and album (or song) on iTunes and MusicBrainz, "
                                 "since Discord can only show pictures from the web."));
    df->addRow(tr("Picture"), discordCovers);
    auto *discordId = new QLineEdit(m_discord->clientId(), deskPage);
    discordId->setPlaceholderText(tr("Application ID from discord.com/developers"));
    df->addRow(tr("Application ID"), discordId);
    auto *discordStatus = new QLabel(deskPage);
    discordStatus->setObjectName(QStringLiteral("hint"));
    discordStatus->setWordWrap(true);
    df->addRow(QString(), discordStatus);
    auto *discordHint = new QLabel(tr("Works with the Discord app and with Vesktop (turn on its arRPC / Rich Presence option). "
                                      "Shows the song, the artist and the picture chosen above; for radio, only a clean \"Artist - Title\", "
                                      "with the song's cover when one is found. Nothing is shared while paused or stopped."),
                                   deskPage);
    discordHint->setObjectName(QStringLiteral("hint"));
    discordHint->setWordWrap(true);
    df->addRow(QString(), discordHint);
    auto showDiscordStatus = [this, discordStatus, discordBox, discordRadio, discordCovers, discordId] {
        discordRadio->setEnabled(discordBox->isChecked());
        discordCovers->setEnabled(discordBox->isChecked());
        discordId->setEnabled(discordBox->isChecked());
        discordStatus->setText(discordBox->isChecked() && m_discord->isEnabled() ? m_discord->statusText() : QString());
    };
    connect(m_discord, &DiscordPresence::statusChanged, discordStatus, showDiscordStatus);
    connect(discordBox, &QCheckBox::toggled, discordStatus, showDiscordStatus);
    showDiscordStatus();
    tabs->addTab(deskPage, tr("Desktop"));

    // --- Plugins: changes here take effect immediately
    auto *plugPage = new QWidget;
    auto *pv = new QVBoxLayout(plugPage);
    auto *plugList = new QListWidget(plugPage);
    plugList->setWordWrap(true);
    plugList->setSpacing(2);
    pv->addWidget(plugList, 1);
    auto *plugRow = new QHBoxLayout;
    auto *configureBtn = new QPushButton(tr("Configure…"), plugPage);
    auto *logBtn = new QPushButton(tr("Log…"), plugPage);
    auto *folderBtn = new QPushButton(tr("Open plugins folder"), plugPage);
    auto *reloadBtn = new QPushButton(tr("Reload"), plugPage);
    plugRow->addWidget(configureBtn);
    plugRow->addWidget(logBtn);
    plugRow->addStretch();
    plugRow->addWidget(folderBtn);
    plugRow->addWidget(reloadBtn);
    pv->addLayout(plugRow);
    auto *plugHint = new QLabel(tr("Plugins are Lua scripts in %1. They run with your permissions, so only turn on plugins "
                                   "you trust. Writing one: PLUGINS.md (shipped with Ruche and in its source).")
                                    .arg(QDir::toNativeSeparators(PluginHost::userDir())),
                                plugPage);
    plugHint->setObjectName(QStringLiteral("hint"));
    plugHint->setWordWrap(true);
    plugHint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pv->addWidget(plugHint);
    auto fillPlugins = [this, plugList] {
        const QString current = plugList->currentItem() ? plugList->currentItem()->data(Qt::UserRole).toString() : QString();
        QSignalBlocker b(plugList);
        plugList->clear();
        for (const PluginHost::Plugin *p : m_plugins->plugins()) {
            QString text = p->name;
            if (!p->version.isEmpty())
                text += QStringLiteral("  ") + p->version;
            if (p->bundled)
                text += QStringLiteral("  ·  ") + tr("example");
            if (!p->description.isEmpty())
                text += QLatin1Char('\n') + p->description;
            if (!p->error.isEmpty())
                text += QLatin1Char('\n') + tr("Error: %1").arg(p->error);
            auto *it = new QListWidgetItem(text, plugList);
            it->setData(Qt::UserRole, p->id);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            it->setCheckState(p->enabled ? Qt::Checked : Qt::Unchecked);
            QString tip = p->script;
            if (!p->author.isEmpty())
                tip = tr("By %1").arg(p->author) + QLatin1Char('\n') + tip;
            it->setToolTip(tip);
            if (!p->error.isEmpty())
                it->setForeground(QColor(0xe8, 0x4a, 0x5f));
            if (p->id == current)
                plugList->setCurrentItem(it);
        }
        if (plugList->count() == 0) {
            auto *it = new QListWidgetItem(tr("No plugins yet. Put one in the plugins folder and press Reload."), plugList);
            it->setFlags(Qt::NoItemFlags);
        }
    };
    auto updateButtons = [this, plugList, configureBtn, logBtn] {
        const PluginHost::Plugin *p =
            plugList->currentItem() ? m_plugins->find(plugList->currentItem()->data(Qt::UserRole).toString()) : nullptr;
        configureBtn->setEnabled(p && !p->settings.isEmpty());
        logBtn->setEnabled(p != nullptr);
    };
    fillPlugins();
    updateButtons();
    connect(m_plugins, &PluginHost::changed, plugList, [=] {
        fillPlugins();
        updateButtons();
    });
    connect(plugList, &QListWidget::currentItemChanged, plugPage, updateButtons);
    connect(plugList, &QListWidget::itemChanged, plugPage, [this](QListWidgetItem *it) {
        m_plugins->setEnabled(it->data(Qt::UserRole).toString(), it->checkState() == Qt::Checked);
    });
    connect(folderBtn, &QPushButton::clicked, plugPage, [] {
        QDir().mkpath(PluginHost::userDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(PluginHost::userDir()));
    });
    connect(reloadBtn, &QPushButton::clicked, plugPage, [this] { m_plugins->reloadAll(); });
    connect(logBtn, &QPushButton::clicked, plugPage, [this, plugList, &d] {
        if (!plugList->currentItem())
            return;
        const PluginHost::Plugin *p = m_plugins->find(plugList->currentItem()->data(Qt::UserRole).toString());
        if (!p)
            return;
        QDialog ld(&d);
        ld.setWindowTitle(tr("%1: log").arg(p->name));
        ld.resize(640, 360);
        auto *l = new QVBoxLayout(&ld);
        auto *text = new QPlainTextEdit(&ld);
        text->setReadOnly(true);
        text->setPlainText(p->log.isEmpty() ? tr("Nothing logged yet.") : p->log.join(QLatin1Char('\n')));
        l->addWidget(text);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &ld);
        connect(bb, &QDialogButtonBox::rejected, &ld, &QDialog::reject);
        l->addWidget(bb);
        ld.exec();
    });
    connect(configureBtn, &QPushButton::clicked, plugPage, [this, plugList, &d] {
        if (!plugList->currentItem())
            return;
        configurePlugin(plugList->currentItem()->data(Qt::UserRole).toString(), &d);
    });
    tabs->addTab(plugPage, tr("Plugins"));

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    const int tab = page == QLatin1String("plugins")   ? tabs->indexOf(plugPage)
                    : page == QLatin1String("library") ? tabs->indexOf(libPage)
                                                       : s.value(QStringLiteral("window/settingsTab"), 0).toInt();
    tabs->setCurrentIndex(qBound(0, tab, tabs->count() - 1));
    const bool ok = d.exec() == QDialog::Accepted;
    s.setValue(QStringLiteral("window/settingsTab"), tabs->currentIndex());
    if (!ok) {
        applyTheme(before);
        return;
    }
    Theme::saveChoice(*choice);
    s.setValue(QStringLiteral("updates/onStartup"), updBox->isChecked());
    s.setValue(QStringLiteral("library/confirmCloseTab"), confirmClose->isChecked());
    if (lang->currentData().toString() != langBefore) {
        s.setValue(QStringLiteral("ui/language"), lang->currentData().toString());
        const QString program = Updater::selfProgram();
        if (!program.isEmpty() &&
            QMessageBox::question(this, tr("Language"), tr("Restart Ruche now to change the language?")) == QMessageBox::Yes &&
            Updater::startAfterExit(program, {}))
            QTimer::singleShot(0, this, &MainWindow::quitApp);
        else if (program.isEmpty())
            flash(tr("The new language is used the next time Ruche starts."));
    }
    QStringList folders;
    m_tabNames.clear();
    for (int i = 0; i < tabList->count(); ++i) {
        QString name = tabList->item(i)->text().trimmed();
        while (m_tabNames.contains(name)) // names identify tabs: keep them unique
            name += QLatin1Char('\'');
        m_tabNames << name;
    }
    m_folderTab.clear();
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const QString f = list->topLevelItem(i)->data(0, Qt::UserRole).toString();
        folders << f;
        if (auto *c = qobject_cast<QComboBox *>(list->itemWidget(list->topLevelItem(i), 1)))
            m_folderTab.insert(f, m_tabNames.value(qMax(0, c->currentIndex())));
    }
    saveLibraryTabs();
    rebuildLibraryTabs();
    if (folders != m_lib->folders()) {
        m_lib->setFolders(folders);
        m_lib->rescan();
    }
    const QString mode = rg->currentData().toString();
    s.setValue(QStringLiteral("replaygain"), mode);
    m_pc->setReplayGain(mode);
    if (trayBox->isChecked() != m_closeToTray)
        setCloseToTray(trayBox->isChecked());
    if (notify->currentData().toString() != m_notifyWhen)
        setNotifyWhen(notify->currentData().toString());
    s.setValue(QStringLiteral("discord/enabled"), discordBox->isChecked());
    s.setValue(QStringLiteral("discord/radio"), discordRadio->isChecked());
    if (discordId->text().trimmed() == DiscordPresence::builtInClientId())
        s.remove(QStringLiteral("discord/clientId"));
    else
        s.setValue(QStringLiteral("discord/clientId"), discordId->text().trimmed());
    m_discord->setClientId(discordId->text());
    m_discord->setShowRadio(discordRadio->isChecked());
    s.setValue(QStringLiteral("discord/covers"), discordCovers->currentData().toBool());
    m_discord->setLookUpCovers(discordCovers->currentData().toBool());
    m_discord->setEnabled(discordBox->isChecked());
    if (menuBox && menuBox->isChecked() != DesktopEntry::isInstalled()) {
        QString err;
        const bool on = menuBox->isChecked();
        if (on ? DesktopEntry::install(&err) : DesktopEntry::remove(&err))
            flash(on ? tr("Ruche is in your applications menu. Right-click it there or in the taskbar to pin it.")
                     : tr("Removed from the applications menu."));
        else
            QMessageBox::warning(this, tr("Applications menu"), err);
    }
}

// Edits to the theme file in use show up straight away (theme authors' live preview).
void MainWindow::watchThemeFile()
{
    if (!m_themeWatch) {
        m_themeWatch = new QFileSystemWatcher(this);
        auto *debounce = new QTimer(this);
        debounce->setSingleShot(true);
        debounce->setInterval(250);
        connect(m_themeWatch, &QFileSystemWatcher::fileChanged, debounce, qOverload<>(&QTimer::start));
        connect(debounce, &QTimer::timeout, this, [this] { applyTheme(m_themeChoice); });
    }
    if (!m_themeWatch->files().isEmpty())
        m_themeWatch->removePaths(m_themeWatch->files());
    if (!Theme::activeFilePath().isEmpty())
        m_themeWatch->addPath(Theme::activeFilePath()); // editors that save by replacing the file drop the watch
}

void MainWindow::applyTheme(const Theme::Choice &c)
{
    m_themeChoice = c;
    Theme::apply(*qApp, c);
    watchThemeFile();
    m_trackModel->setAccent(Theme::accentText());
    m_stationModel->setAccent(Theme::accentText());
    rebuildNavigator();
    m_logoKey.clear();
    if (!m_coverImage.isNull() && m_pc->mode() == PlaybackController::Mode::Library)
        m_np->setCover(m_coverImage, Icons::Music);
    updateNowPlaying();
    m_bar->refreshTheme();
    for (QWidget *w : QApplication::allWidgets())
        w->update();
}

void MainWindow::importPlaylistFile(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QString text = meta::decodeLoose(f.read(8 * 1024 * 1024));
    f.close();
    // A station list points at network streams; a playlist points at files. Look at the
    // entries themselves (a station list may carry file:// logo addresses in its attributes).
    const QUrl base = QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath());
    const QVector<meta::M3uEntry> entries = QFileInfo(file).suffix().compare(QLatin1String("pls"), Qt::CaseInsensitive) == 0
                                                ? meta::parsePls(text, base)
                                                : meta::parseM3u(text, base);
    bool hasRemote = false;
    for (const meta::M3uEntry &e : entries)
        if (!e.url.isLocalFile() && e.url.scheme().startsWith(QLatin1String("http")))
            hasRemote = true;
    if (meta::looksLikeHls(text) || hasRemote) {
        const int before = m_stations->list().size();
        const int n = m_stations->importFile(file);
        switchView(View::Radio);
        flash(n ? tr("Imported %1").arg(countOf(n, tr("station"), tr("stations"))) : tr("Those stations are already in your list."));
        if (n == 1)
            m_stationView->selectRow(before);
        return;
    }
    const QString name = m_lib->importPlaylist(file);
    if (name.isEmpty()) {
        flash(tr("Nothing playable found in %1").arg(QFileInfo(file).fileName()));
        return;
    }
    switchView(View::Playlist, name);
    flash(tr("Imported playlist “%1”").arg(name));
}

void MainWindow::openFiles(const QStringList &pathsIn, bool play)
{
    QVector<qint64> ids;
    for (const QString &p : pathsIn) {
        const QFileInfo fi(p);
        if (fi.isDir()) {
            QStringList files;
            QDirIterator it(fi.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString f = it.next();
                if (isAudioFile(f))
                    files << f;
            }
            const QCollator coll = makeCollator();
            std::sort(files.begin(), files.end(), [&coll](const QString &a, const QString &b) { return coll.compare(a, b) < 0; });
            for (const QString &f : files)
                ids << m_lib->addExternal(f);
        } else if (fi.suffix().toLower() == QLatin1String("m3u") || fi.suffix().toLower() == QLatin1String("m3u8") ||
                   fi.suffix().toLower() == QLatin1String("pls")) {
            importPlaylistFile(fi.absoluteFilePath());
        } else if (fi.exists() && isAudioFile(fi.fileName())) {
            ids << m_lib->addExternal(fi.absoluteFilePath());
        }
    }
    if (ids.isEmpty())
        return;
    if (play || !m_pc->isActive())
        m_pc->playTracks(ids, 0);
    else
        m_pc->enqueue(ids);
}

void MainWindow::openArguments(const QStringList &args)
{
    QStringList files;
    for (const QString &a : args) {
        const QUrl u(a);
        if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https")) {
            int row = m_stations->indexOfUrl(u.toString());
            const bool known = row >= 0;
            if (!known)
                row = m_stations->add(QString(), u.toString());
            m_stationModel->setLive(u.toString(), QString());
            m_pc->playStation(known ? m_stations->list()[row].name : QString(), u);
            switchView(View::Radio);
            m_stationView->selectRow(row);
            return;
        }
        files << (u.isLocalFile() ? u.toLocalFile() : a);
    }
    // A single station list on the command line: import it and play the first station.
    if (files.size() == 1) {
        const QFileInfo fi(files.first());
        const QString ext = fi.suffix().toLower();
        if (ext == QLatin1String("m3u") || ext == QLatin1String("m3u8") || ext == QLatin1String("pls")) {
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly)) {
                const QString text = meta::decodeLoose(f.read(8 * 1024 * 1024));
                QVector<meta::M3uEntry> entries = ext == QLatin1String("pls")
                                                      ? meta::parsePls(text, QUrl::fromLocalFile(fi.absoluteFilePath()))
                                                      : meta::parseM3u(text, QUrl::fromLocalFile(fi.absoluteFilePath()));
                const bool remote = !entries.isEmpty() && !entries.first().url.isLocalFile();
                if (remote || meta::looksLikeHls(text)) {
                    importPlaylistFile(fi.absoluteFilePath());
                    const QString url = meta::looksLikeHls(text) ? QUrl::fromLocalFile(fi.absoluteFilePath()).toString()
                                                                 : entries.first().url.toString();
                    const int row = m_stations->indexOfUrl(url);
                    if (row >= 0)
                        playStationRow(row);
                    return;
                }
            }
        }
    }
    openFiles(files, true);
}

// Tracks dragged from the list onto the sidebar: a playlist gets them appended,
// "New playlist…" makes a playlist of them.
bool MainWindow::navDropEvent(QEvent *ev)
{
    auto *de = static_cast<QDropEvent *>(ev);
    if (!de->mimeData()->hasFormat(QString::fromLatin1(TrackTableModel::kIdsMime)))
        return false;
    QTreeWidgetItem *it = m_nav->itemAt(de->position().toPoint());
    const QString kind = it ? it->data(0, NavKind).toString() : QString();
    const bool target = kind == QLatin1String("playlist") || kind == QLatin1String("newplaylist");
    if (ev->type() == QEvent::DragEnter) {
        de->acceptProposedAction(); // keep receiving moves; the target is decided per position
        return true;
    }
    if (ev->type() == QEvent::DragMove) {
        if (target) {
            de->acceptProposedAction();
            m_nav->setCurrentItem(it);
        } else {
            de->ignore();
        }
        return true;
    }
    // Drop
    const QVector<qint64> ids = TrackTableModel::idsFromMime(de->mimeData());
    if (target && !ids.isEmpty()) {
        de->acceptProposedAction();
        QString name = it->data(0, NavName).toString();
        if (kind == QLatin1String("newplaylist")) {
            name = askPlaylistName(tr("New playlist"));
            if (name.isEmpty()) {
                rebuildNavigator();
                return true;
            }
        }
        m_lib->appendToPlaylist(name, ids);
        flash(tr("Added %1 to “%2”").arg(countOf(ids.size(), tr("track"), tr("tracks")), name));
        if (m_view == View::Playlist && m_playlist == name)
            switchView(View::Playlist, name);
    }
    rebuildNavigator();
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    // Rows dragged from the track list only drop onto the sidebar's playlists
    if (e->mimeData()->hasFormat(QString::fromLatin1(TrackTableModel::kIdsMime)))
        return;
    if (e->mimeData()->hasUrls() || e->mimeData()->hasImage())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    // An image dropped on a station row becomes that station's logo.
    if (m_view == View::Radio) {
        const QPoint vp = m_stationView->viewport()->mapFrom(this, e->position().toPoint());
        const QModelIndex idx = m_stationView->viewport()->rect().contains(vp) ? m_stationView->indexAt(vp) : QModelIndex();
        if (idx.isValid()) {
            const QMimeData *m = e->mimeData();
            QUrl imageUrl;
            for (const QUrl &u : m->urls())
                if (Logo::isImageFileName(u.path())) {
                    imageUrl = u;
                    break;
                }
            if (imageUrl.isValid()) {
                m_stations->setLogoFromUrl(idx.row(), imageUrl);
                e->acceptProposedAction();
                return;
            }
            if (m->hasImage()) {
                m_stations->setLogoFromImage(idx.row(), qvariant_cast<QImage>(m->imageData()));
                e->acceptProposedAction();
                return;
            }
        }
    }
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls()) {
        if (u.isLocalFile())
            files << u.toLocalFile();
        else if (u.scheme().startsWith(QLatin1String("http"))) {
            m_stations->add(QString(), u.toString());
            switchView(View::Radio);
        }
    }
    if (!files.isEmpty())
        openFiles(files, false);
}

void MainWindow::flash(const QString &text)
{
    m_flash->setText(text);
    m_flashTimer.start(7000);
}

void MainWindow::loadSettings()
{
    loadLibraryTabs();
    QSettings s(paths::configFile(), QSettings::IniFormat);
    if (!restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1360, 820);
    m_mainSplit->restoreState(s.value(QStringLiteral("window/mainSplit")).toByteArray());
    m_libSplit->restoreState(s.value(QStringLiteral("window/libSplit")).toByteArray());
    m_browserSplit->restoreState(s.value(QStringLiteral("window/browserSplit")).toByteArray());
    m_headerStateLibrary = s.value(QStringLiteral("window/trackHeader")).toByteArray();
    if (!m_headerStateLibrary.isEmpty())
        m_trackView->horizontalHeader()->restoreState(m_headerStateLibrary);
    for (int c : {int(TrackTableModel::ColTitle), int(TrackTableModel::ColArtist), int(TrackTableModel::ColAlbum)})
        m_trackView->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
    fitNarrowColumns();
    {
        // the library's sort (clicked column header) survives a restart
        const int col = s.value(QStringLiteral("library/sortColumn"), -1).toInt();
        const auto order = Qt::SortOrder(s.value(QStringLiteral("library/sortOrder"), 0).toInt());
        if (col >= -1 && col < TrackTableModel::ColCount) {
            m_trackModel->sort(col, order);
            m_trackView->horizontalHeader()->setSortIndicator(col, order);
        }
    }
    const int vol = s.value(QStringLiteral("player/volume"), 80).toInt();
    m_pc->setVolume(vol);
    m_bar->setVolume(vol);
    m_pc->setShuffle(s.value(QStringLiteral("player/shuffle"), false).toBool());
    m_pc->setRepeat(PlaybackController::Repeat(s.value(QStringLiteral("player/repeat"), 0).toInt()));
    m_pc->setReplayGain(s.value(QStringLiteral("replaygain"), QStringLiteral("no")).toString());
    if (s.value(QStringLiteral("window/closeToTray"), false).toBool())
        setCloseToTray(true, false);
    setNotifyWhen(s.value(QStringLiteral("notify/when"), QStringLiteral("background")).toString(), false);
    m_discord->setClientId(s.value(QStringLiteral("discord/clientId"), DiscordPresence::builtInClientId()).toString());
    m_discord->setShowRadio(s.value(QStringLiteral("discord/radio"), true).toBool());
    m_discord->setLookUpCovers(s.value(QStringLiteral("discord/covers"), true).toBool());
    m_discord->setEnabled(s.value(QStringLiteral("discord/enabled"), false).toBool()); // opt-in

    if (!s.value(QStringLiteral("firstRunDone")).toBool()) {
        s.setValue(QStringLiteral("firstRunDone"), true);
        const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        if (m_lib->folders().isEmpty() && !music.isEmpty() && QFileInfo(music).isDir() &&
            music != QDir::homePath()) {
            m_lib->setFolders({music});
            m_lib->rescan();
        }
    }
}

void MainWindow::saveSettings()
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/mainSplit"), m_mainSplit->saveState());
    s.setValue(QStringLiteral("window/libSplit"), m_libSplit->saveState());
    s.setValue(QStringLiteral("window/browserSplit"), m_browserSplit->saveState());
    s.setValue(QStringLiteral("window/trackHeader"),
               m_view == View::Library ? m_trackView->horizontalHeader()->saveState() : m_headerStateLibrary);
    s.setValue(QStringLiteral("library/sortColumn"), m_trackModel->sortColumn());
    s.setValue(QStringLiteral("library/sortOrder"), int(m_trackModel->sortOrder()));
    s.setValue(QStringLiteral("player/volume"), m_pc->volume());
    s.setValue(QStringLiteral("player/shuffle"), m_pc->shuffle());
    s.setValue(QStringLiteral("player/repeat"), int(m_pc->repeat()));
    m_stations->save();
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_libTabs && e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::MiddleButton) { // middle-click closes, as in browsers
            closeLibraryTab(m_libTabs->tabAt(me->position().toPoint()));
            return true;
        }
    }
    if (m_tabHint && o == m_trackView->viewport() && e->type() == QEvent::Resize)
        m_tabHint->setGeometry(m_trackView->viewport()->rect());
    if (o == m_nav->viewport() && (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove ||
                                   e->type() == QEvent::DragLeave || e->type() == QEvent::Drop)) {
        if (e->type() == QEvent::DragLeave ? true : navDropEvent(e)) {
            if (e->type() == QEvent::DragLeave)
                rebuildNavigator();
            return true;
        }
    }
    if (m_stationHint && o == m_stationView->viewport() && e->type() == QEvent::Resize)
        m_stationHint->setGeometry(m_stationView->viewport()->rect());
    return QMainWindow::eventFilter(o, e);
}

static QIcon trayIcon()
{
    // Fixed pixmaps so the tray (StatusNotifierItem) gets every size it asks for.
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        Icons::paint(&p, QRectF(0, 0, size, size), Icons::Logo, Theme::Brand);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

void MainWindow::setCloseToTray(bool on, bool persist)
{
    m_closeToTray = on;
    if (persist) {
        QSettings s(paths::configFile(), QSettings::IniFormat);
        s.setValue(QStringLiteral("window/closeToTray"), on);
    }
    // With the window hidden, closing a dialog must not count as closing the last window.
    qApp->setQuitOnLastWindowClosed(!on);
    syncTray();
}

void MainWindow::syncTray()
{
    const bool need = m_closeToTray || (Notifier::needsTray() && m_notifyWhen != QLatin1String("off"));
    if (!need) {
        if (m_notifier)
            m_notifier->setTray(nullptr);
        delete m_tray;
        m_tray = nullptr;
        return;
    }
    if (m_tray)
        return;
    m_tray = new QSystemTrayIcon(trayIcon(), this);
    auto *menu = new QMenu(this);
    m_trayTitle = menu->addAction(QString());
    m_trayTitle->setEnabled(false);
    menu->addSeparator();
    m_trayPlay = menu->addAction(tr("Play / Pause"), m_pc, &PlaybackController::togglePause);
    menu->addAction(tr("Next track"), m_pc, &PlaybackController::next);
    menu->addAction(tr("Previous track"), m_pc, &PlaybackController::previous);
    menu->addSeparator();
    m_trayShow = menu->addAction(tr("Show Ruche"), this, [this] {
        if (isVisible())
            hide();
        else
            showFromTray();
    });
    menu->addAction(tr("Quit"), this, &MainWindow::quitApp);
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::updateTray);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::MiddleClick) {
            m_pc->togglePause();
        } else if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
            if (isVisible() && !isMinimized())
                hide();
            else
                showFromTray();
        }
    });
    updateTray();
    m_tray->show();
    if (m_notifier)
        m_notifier->setTray(m_tray);
}

void MainWindow::setNotifyWhen(const QString &when, bool persist)
{
    const QString w = when == QLatin1String("always") || when == QLatin1String("off") ? when : QStringLiteral("background");
    m_notifyWhen = w;
    if (persist) {
        QSettings s(paths::configFile(), QSettings::IniFormat);
        s.setValue(QStringLiteral("notify/when"), w);
        if (w != QLatin1String("off"))
            s.setValue(QStringLiteral("notify/onMode"), w);
    }
    syncTray();
}

bool MainWindow::wantNotification() const
{
    if (m_notifyWhen == QLatin1String("off") || !m_notifier || !m_notifier->available())
        return false;
    if (m_notifyWhen == QLatin1String("always"))
        return true;
    return !isVisible() || isMinimized() || !isActiveWindow();
}

void MainWindow::scheduleTrackNotification()
{
    if (!wantNotification())
        return;
    m_notifyPending = true;
    if (m_artPending)
        m_notifyTimer.start(700); // the cover usually arrives well before this
    else
        notifyNow();
}

static QImage logoImage()
{
    QImage img(128, 128, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    Icons::paint(&p, QRectF(8, 8, 112, 112), Icons::Logo, Theme::Brand);
    return img;
}

void MainWindow::notifyNow()
{
    m_notifyPending = false;
    m_notifyTimer.stop();
    if (!m_notifier)
        return;
    if (m_pc->mode() == PlaybackController::Mode::Library) {
        const Track *t = m_lib->byId(m_pc->currentTrackId());
        if (!t)
            return;
        m_lastNotifiedTrack = t->id;
        QStringList body;
        if (!t->artist.isEmpty())
            body << t->artist;
        QString album = t->album;
        if (t->year > 0 && !album.isEmpty())
            album += QStringLiteral(" (%1)").arg(t->year);
        if (!album.isEmpty())
            body << album;
        const QImage cover = m_coverFor == t->id && !m_coverImage.isNull() ? m_coverImage : logoImage();
        const bool canSkip = !m_pc->upcomingIndexes(1).isEmpty();
        m_notifier->show(t->displayTitle(), body.join(QLatin1Char('\n')), cover, canSkip);
    } else if (m_pc->mode() == PlaybackController::Mode::Radio) {
        const QString title = m_pc->streamTitle();
        if (title.isEmpty())
            return;
        const QString station = m_pc->stationName().isEmpty() ? m_pc->stationUrl().host() : m_pc->stationName();
        QString summary = title;
        QStringList body;
        const int dash = title.indexOf(QLatin1String(" - "));
        if (dash > 0) {
            body << title.left(dash).trimmed();
            summary = title.mid(dash + 3).trimmed();
        }
        body << station;
        const QString logoPath = m_stations->logoPathForUrl(m_pc->stationUrl().toString());
        QImage logo = logoPath.isEmpty() ? QImage() : QImage(logoPath);
        if (logo.isNull())
            logo = logoImage();
        m_notifier->show(summary, body.join(QLatin1Char('\n')), logo, false);
    }
}

void MainWindow::updateTray()
{
    if (!m_tray)
        return;
    QString line;
    if (m_pc->mode() == PlaybackController::Mode::Library) {
        if (const Track *t = m_lib->byId(m_pc->currentTrackId()))
            line = t->artist.isEmpty() ? t->displayTitle() : t->displayTitle() + QStringLiteral(" — ") + t->artist;
    } else if (m_pc->mode() == PlaybackController::Mode::Radio) {
        const QString station = m_pc->stationName().isEmpty() ? m_pc->stationUrl().host() : m_pc->stationName();
        line = m_pc->streamTitle().isEmpty() ? station : m_pc->streamTitle() + QStringLiteral(" — ") + station;
    }
    const QString state = !m_pc->isActive() ? tr("Stopped") : m_pc->isPaused() ? tr("Paused") : QString();
    m_tray->setToolTip(line.isEmpty() ? QStringLiteral("Ruche") : (state.isEmpty() ? line : state + QStringLiteral(": ") + line));
    m_trayTitle->setText(line.isEmpty() ? tr("Nothing playing") : line);
    m_trayPlay->setText(m_pc->isActive() && !m_pc->isPaused() ? tr("Pause") : tr("Play"));
    m_trayShow->setText(isVisible() ? tr("Hide Ruche") : tr("Show Ruche"));
}

void MainWindow::showFromTray()
{
    showNormal();
    raise();
    activateWindow();
    updateTray();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    saveSettings();
    if (m_closeToTray && m_tray && !m_quitting) {
        hide();
        e->ignore();
        QSettings s(paths::configFile(), QSettings::IniFormat);
        if (!s.value(QStringLiteral("window/trayHintShown"), false).toBool()) {
            s.setValue(QStringLiteral("window/trayHintShown"), true);
            m_tray->showMessage(QStringLiteral("Ruche"),
                                tr("Still playing in the system tray. Right-click the tray icon to quit."),
                                trayIcon(), 6000);
        }
        updateTray();
        return;
    }
    e->accept();
}

#ifdef RUCHE_DBUS
void MainWindow::onPortalSetting(const QString &ns, const QString &key, const QDBusVariant &)
{
    if (ns == QLatin1String("org.freedesktop.appearance") && key == QLatin1String("color-scheme") &&
        m_themeChoice.theme == QLatin1String("system"))
        applyTheme(m_themeChoice);
}
#endif

void MainWindow::configurePlugin(const QString &id, QWidget *parent)
{
    const PluginHost::Plugin *p = m_plugins ? m_plugins->find(id) : nullptr;
    if (!p || p->settings.isEmpty())
        return;
    QDialog d(parent);
    d.setWindowTitle(tr("%1: settings").arg(p->name));
    d.setMinimumWidth(460);
    auto *v = new QVBoxLayout(&d);
    auto *form = new QFormLayout;
    v->addLayout(form);
    const QJsonObject current = m_plugins->config(id);
    QVector<std::function<QJsonValue()>> getters;
    QStringList keys;
    for (const PluginHost::Setting &st : p->settings) {
        const QJsonValue val = current.value(st.key);
        QWidget *w = nullptr;
        if (st.type == QLatin1String("bool")) {
            auto *c = new QCheckBox(&d);
            c->setChecked(val.toBool());
            getters << [c] { return QJsonValue(c->isChecked()); };
            w = c;
        } else if (st.type == QLatin1String("int")) {
            auto *sp = new QSpinBox(&d);
            sp->setRange(st.min, st.max);
            sp->setValue(val.toInt());
            getters << [sp] { return QJsonValue(sp->value()); };
            w = sp;
        } else if (st.type == QLatin1String("choice")) {
            auto *cb = new QComboBox(&d);
            cb->addItems(st.choices);
            cb->setCurrentIndex(qMax(0, int(st.choices.indexOf(val.toString()))));
            getters << [cb] { return QJsonValue(cb->currentText()); };
            w = cb;
        } else {
            auto *le = new QLineEdit(val.toVariant().toString(), &d);
            if (st.type == QLatin1String("password"))
                le->setEchoMode(QLineEdit::PasswordEchoOnEdit);
            getters << [le] { return QJsonValue(le->text()); };
            w = le;
        }
        if (!st.help.isEmpty())
            w->setToolTip(st.help);
        form->addRow(st.label, w);
        if (!st.help.isEmpty()) {
            auto *h = new QLabel(st.help, &d);
            h->setObjectName(QStringLiteral("hint"));
            h->setWordWrap(true);
            form->addRow(QString(), h);
        }
        keys << st.key;
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted)
        return;
    QJsonObject values;
    for (int i = 0; i < keys.size(); ++i)
        values.insert(keys.at(i), getters.at(i)());
    m_plugins->setConfig(id, values);
}

void MainWindow::quitApp()
{
    m_quitting = true; // past close-to-tray
    qApp->quit();
}

// After a crash, say so once, and point at the log.
void MainWindow::showCrashNotice()
{
    const QStringList fresh = crashlog::unseen();
    if (fresh.isEmpty())
        return;
    QMessageBox box(QMessageBox::Warning, tr("Ruche"), tr("Ruche closed unexpectedly last time."), QMessageBox::Ok, this);
    box.setInformativeText(tr("A crash log was saved. If it keeps happening, please open an issue on GitHub "
                              "and attach the log; it says what Ruche was doing, nothing personal beyond file names "
                              "it had just played."));
    QPushButton *open = box.addButton(tr("Open folder"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open)
        QDesktopServices::openUrl(QUrl::fromLocalFile(crashlog::dir()));
}

void MainWindow::startupUpdateCheck()
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    if (!s.contains(QStringLiteral("updates/onStartup"))) {
        // First start: ask once.
        QMessageBox box(QMessageBox::Question, tr("Updates"),
                        tr("Should Ruche check for a new version each time it starts?"), QMessageBox::NoButton, this);
        box.setInformativeText(tr("It asks GitHub for the newest release of Ruche; nothing else is sent. "
                                  "When there is one, Ruche asks before installing it. "
                                  "You can change this later in Settings → General."));
        QPushButton *yes = box.addButton(tr("Check on start"), QMessageBox::AcceptRole);
        box.addButton(tr("Don't check"), QMessageBox::RejectRole);
        box.setDefaultButton(yes);
        box.exec();
        s.setValue(QStringLiteral("updates/onStartup"), box.clickedButton() == yes);
    }
    if (s.value(QStringLiteral("updates/onStartup"), false).toBool())
        checkForUpdates(false);
}

void MainWindow::checkForUpdates(bool manual, QLabel *status)
{
    if (status)
        status->setText(tr("Checking…"));
    QPointer<QLabel> label(status);
    connect(m_updater, &Updater::checked, this, [this, manual, label](bool newer, const Updater::Release &rel, const QString &error) {
        QSettings s(paths::configFile(), QSettings::IniFormat);
        s.setValue(QStringLiteral("updates/lastCheck"), QDateTime::currentDateTime());
        if (!error.isEmpty()) {
            if (label)
                label->setText(tr("Couldn't check for updates: %1").arg(error));
            else if (manual)
                QMessageBox::warning(this, tr("Updates"), tr("Couldn't check for updates: %1").arg(error));
            return;
        }
        if (!newer) {
            const QString text = tr("You have the newest version (%1).").arg(QStringLiteral(RUCHE_VERSION));
            if (label)
                label->setText(text);
            else if (manual)
                QMessageBox::information(this, tr("Updates"), text);
            return;
        }
        if (label)
            label->setText(tr("Ruche %1 is available.").arg(rel.version));
        // an automatic check stays quiet about a version the user chose to skip
        if (!manual && s.value(QStringLiteral("updates/skip")).toString() == rel.version)
            return;
        showUpdate(rel, manual);
    }, Qt::SingleShotConnection);
    m_updater->check();
}

void MainWindow::showUpdate(const Updater::Release &rel, bool manual)
{
    const Updater::Kind kind = Updater::kind();
    QDialog d(this);
    d.setWindowTitle(tr("Update available"));
    d.setMinimumSize(520, 380);
    auto *v = new QVBoxLayout(&d);
    auto *head = new QLabel(tr("<h3>Ruche %1 is available</h3>You have %2 (%3).")
                                .arg(rel.version.toHtmlEscaped(), QStringLiteral(RUCHE_VERSION), Updater::kindName(kind)),
                            &d);
    head->setWordWrap(true);
    v->addWidget(head);
    auto *notes = new QTextBrowser(&d);
    notes->setOpenExternalLinks(true);
    notes->setMarkdown(rel.notes.isEmpty() ? tr("No release notes.") : rel.notes);
    v->addWidget(notes, 1);
    auto *bb = new QDialogButtonBox(&d);
    QString action = tr("Update now");
    if (kind == Updater::Kind::Flatpak || kind == Updater::Kind::WindowsPortable)
        action = tr("Download");
    if (rel.assetUrl.isEmpty())
        action = tr("Open the release page");
    QPushButton *go = bb->addButton(action, QDialogButtonBox::AcceptRole);
    bb->addButton(tr("Later"), QDialogButtonBox::RejectRole);
    QPushButton *skip = manual ? nullptr : bb->addButton(tr("Skip this version"), QDialogButtonBox::DestructiveRole);
    go->setDefault(true);
    v->addWidget(bb);
    bool accepted = false;
    connect(bb, &QDialogButtonBox::clicked, &d, [&](QAbstractButton *b) {
        if (b == go)
            accepted = true;
        else if (skip && b == skip)
            QSettings(paths::configFile(), QSettings::IniFormat).setValue(QStringLiteral("updates/skip"), rel.version);
        d.accept();
    });
    d.exec();
    if (!accepted)
        return;
    if (rel.assetUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(rel.page));
        return;
    }
    // download with progress, then install
    auto *progress = new QProgressDialog(tr("Downloading Ruche %1…").arg(rel.version), tr("Cancel"), 0, 100, this);
    progress->setWindowTitle(tr("Update"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    connect(progress, &QProgressDialog::canceled, m_updater, &Updater::cancel);
    auto done = std::make_shared<QMetaObject::Connection>();
    auto fail = std::make_shared<QMetaObject::Connection>();
    auto prog = std::make_shared<QMetaObject::Connection>();
    auto cleanup = [done, fail, prog, progress] {
        QObject::disconnect(*done);
        QObject::disconnect(*fail);
        QObject::disconnect(*prog);
        progress->deleteLater();
    };
    *prog = connect(m_updater, &Updater::progress, progress, [progress](qint64 got, qint64 total) {
        if (total > 0)
            progress->setValue(int(got * 100 / total));
    });
    *fail = connect(m_updater, &Updater::failed, this, [this, cleanup](const QString &error) {
        cleanup();
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Update"), tr("The update failed: %1").arg(error));
    });
    *done = connect(m_updater, &Updater::downloaded, this, [this, cleanup, rel](const QString &file) {
        cleanup();
        QString message;
        if (m_updater->install(rel, file, this, &message)) {
            flash(tr("Restarting into Ruche %1…").arg(rel.version));
            QTimer::singleShot(300, this, &MainWindow::quitApp);
        } else if (!message.isEmpty()) {
            QMessageBox::information(this, tr("Update"), message);
        }
    });
    m_updater->download(rel);
}

void MainWindow::showAbout()
{
    QDialog d(this);
    d.setWindowTitle(tr("About Ruche"));
    auto *v = new QVBoxLayout(&d);
    auto *top = new QHBoxLayout;
    auto *logo = new QLabel(&d);
    logo->setPixmap(Icons::accentIcon(Icons::Logo).pixmap(64, 64));
    top->addWidget(logo, 0, Qt::AlignTop);
    auto *text = new QLabel(tr("<h3>Ruche %1</h3><p>A light music player with a MusicBee-style layout.</p>"
                               "<p>Playback by libmpv. Tags by TagLib. Live stream titles from ICY, "
                               "in-band tags, HLS #EXTINF and HLS ID3 timed metadata.</p>"
                               "<p><a href=\"%2\">%2</a></p>")
                                .arg(QStringLiteral(RUCHE_VERSION), QStringLiteral("https://github.com/") + Updater::repo()),
                            &d);
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    text->setMinimumWidth(380);
    auto *status = new QLabel(&d);
    status->setObjectName(QStringLiteral("hint"));
    status->setWordWrap(true);
    auto *col = new QVBoxLayout; // the update status lines up with the text
    col->addWidget(text);
    col->addWidget(status);
    top->addLayout(col, 1);
    v->addLayout(top);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    QPushButton *check = bb->addButton(tr("Check for updates"), QDialogButtonBox::ActionRole);
    connect(check, &QPushButton::clicked, &d, [this, status] { checkForUpdates(true, status); });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.exec();
}

// ---------------------------------------------------------------- library tabs
// Stored as library/tabs (names, in order) and library/folderTabs ("tab<TAB>folder" lines).

// The default tab is stored as "Music" and shown in the UI language, so it
// follows a language change; tabs the user named are stored as typed.
static const QLatin1String kDefaultTab("Music");

QString MainWindow::tabFromStored(const QString &stored) const
{
    return stored == kDefaultTab ? tr("Music") : stored;
}

QString MainWindow::tabToStored(const QString &name) const
{
    return name == tr("Music") ? QString(kDefaultTab) : name;
}

void MainWindow::loadLibraryTabs()
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    m_tabNames.clear();
    for (const QString &t : s.value(QStringLiteral("library/tabs")).toStringList())
        if (!t.isEmpty() && !m_tabNames.contains(tabFromStored(t)))
            m_tabNames << tabFromStored(t);
    if (m_tabNames.isEmpty())
        m_tabNames << tr("Music");
    m_folderTab.clear();
    for (const QString &line : s.value(QStringLiteral("library/folderTabs")).toStringList()) {
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab > 0)
            m_folderTab.insert(line.mid(tab + 1), tabFromStored(line.left(tab)));
    }
    rebuildLibraryTabs();
}

void MainWindow::saveLibraryTabs()
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    QStringList stored;
    for (const QString &t : std::as_const(m_tabNames))
        stored << tabToStored(t);
    s.setValue(QStringLiteral("library/tabs"), stored);
    QStringList lines;
    for (auto it = m_folderTab.cbegin(); it != m_folderTab.cend(); ++it)
        if (m_tabNames.contains(it.value()))
            lines << tabToStored(it.value()) + QLatin1Char('\t') + it.key();
    s.setValue(QStringLiteral("library/folderTabs"), lines);
}

void MainWindow::rebuildLibraryTabs()
{
    m_tabsUpdating = true;
    const QString current = tabFromStored(QSettings(paths::configFile(), QSettings::IniFormat).value(QStringLiteral("library/currentTab")).toString());
    while (m_libTabs->count())
        m_libTabs->removeTab(0);
    for (const QString &t : std::as_const(m_tabNames)) {
        const int i = m_libTabs->addTab(t);
        if (m_tabNames.size() > 1) { // the last tab can't be closed
            auto *x = new QToolButton(m_libTabs);
            x->setObjectName(QStringLiteral("libTabClose"));
            x->setIcon(Icons::icon(Icons::Close));
            x->setIconSize(QSize(12, 12));
            x->setAutoRaise(true);
            x->setFixedSize(18, 18);
            x->setToolTip(tr("Close tab"));
            connect(x, &QToolButton::clicked, this, [this, x] {
                for (int j = 0; j < m_libTabs->count(); ++j) // tabs move, so look the button up
                    if (m_libTabs->tabButton(j, QTabBar::RightSide) == x) {
                        closeLibraryTab(j);
                        return;
                    }
            });
            m_libTabs->setTabButton(i, QTabBar::RightSide, x);
        }
    }
    m_libTabs->setCurrentIndex(qMax(0, int(m_tabNames.indexOf(current))));
    // the scroll arrows' room would leave a gap before + when there's only one tab
    m_libTabs->setUsesScrollButtons(m_tabNames.size() > 1);
    m_libTabs->setFixedHeight(m_libTabs->sizeHint().height());
    m_tabsUpdating = false;
}

QStringList MainWindow::currentTabFolders() const
{
    if (m_tabNames.size() <= 1 || m_libTabs->currentIndex() < 0)
        return {};
    const QString tab = m_libTabs->tabText(m_libTabs->currentIndex());
    const QString first = m_tabNames.value(0);
    QStringList out;
    for (const QString &f : m_lib->folders()) {
        QString owner = m_folderTab.value(f, first);
        if (!m_tabNames.contains(owner))
            owner = first; // its tab was removed
        if (owner == tab)
            out << (f.endsWith(QLatin1Char('/')) ? f : f + QLatin1Char('/'));
    }
    if (out.isEmpty())
        out << QStringLiteral("\x01"); // a tab without folders shows nothing
    return out;
}

void MainWindow::renameLibraryTab(int i)
{
    if (i < 0 || i >= m_tabNames.size())
        return;
    // Edit the name in place, like a browser bookmark: Enter keeps it, Esc cancels.
    const QString old = m_tabNames.at(i);
    auto *edit = new QLineEdit(old, m_libTabs);
    edit->setObjectName(QStringLiteral("libTabEdit"));
    edit->setGeometry(m_libTabs->tabRect(i).adjusted(4, 3, -4, -3));
    edit->selectAll();
    edit->show();
    edit->setFocus();
    auto finish = [this, edit, old](bool keep) {
        if (edit->property("done").toBool())
            return;
        edit->setProperty("done", true);
        const QString name = edit->text().trimmed();
        edit->deleteLater();
        if (!keep || name.isEmpty() || name == old)
            return;
        if (m_tabNames.contains(name)) {
            flash(tr("There's already a tab called “%1”.").arg(name));
            return;
        }
        const int at = m_tabNames.indexOf(old);
        if (at < 0)
            return;
        m_tabNames[at] = name;
        for (auto it = m_folderTab.begin(); it != m_folderTab.end(); ++it)
            if (it.value() == old)
                it.value() = name;
        QSettings s(paths::configFile(), QSettings::IniFormat);
        if (tabFromStored(s.value(QStringLiteral("library/currentTab")).toString()) == old)
            s.setValue(QStringLiteral("library/currentTab"), tabToStored(name));
        saveLibraryTabs();
        rebuildLibraryTabs();
    };
    connect(edit, &QLineEdit::editingFinished, this, [finish] { finish(true); });
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), edit, nullptr, nullptr, Qt::WidgetShortcut);
    connect(esc, &QShortcut::activated, this, [finish] { finish(false); });
}

// Folders with no tab of their own belong to the first tab. Before tabs move or close,
// write that down, so they stay where they are.
void MainWindow::pinFolderTabs()
{
    const QString first = m_tabNames.value(0);
    for (const QString &f : m_lib->folders())
        if (!m_tabNames.contains(m_folderTab.value(f)))
            m_folderTab.insert(f, first);
}

void MainWindow::newLibraryTab()
{
    QString name = tr("New tab");
    for (int n = 2; m_tabNames.contains(name); ++n)
        name = tr("New tab %1").arg(n);
    m_tabNames << name;
    saveLibraryTabs();
    QSettings(paths::configFile(), QSettings::IniFormat).setValue(QStringLiteral("library/currentTab"), tabToStored(name));
    rebuildLibraryTabs();
    if (m_view != View::Library)
        switchView(View::Library);
    refreshLibrary(0);
    // straight into naming it, once the bar has laid the new tab out
    QTimer::singleShot(0, this, [this] { renameLibraryTab(m_tabNames.size() - 1); });
}

void MainWindow::closeLibraryTab(int i)
{
    if (i < 0 || i >= m_tabNames.size() || m_tabNames.size() <= 1)
        return;
    const QString name = m_tabNames.at(i);
    QStringList folders;
    for (const QString &f : m_lib->folders()) {
        QString owner = m_folderTab.value(f, m_tabNames.value(0));
        if (!m_tabNames.contains(owner))
            owner = m_tabNames.value(0);
        if (owner == name)
            folders << f;
    }
    QSettings s(paths::configFile(), QSettings::IniFormat);
    if (!folders.isEmpty() && s.value(QStringLiteral("library/confirmCloseTab"), true).toBool()) {
        QMessageBox box(QMessageBox::Question, tr("Close tab"),
                        tr("Close the tab “%1”?").arg(name), QMessageBox::Cancel, this);
        box.setInformativeText(tr("Its music folders are taken out of the library:\n%1\n\nThe files themselves stay where they are, "
                                  "and adding the folders again brings them back.")
                                   .arg(QDir::toNativeSeparators(folders.join(QLatin1Char('\n')))));
        QPushButton *close = box.addButton(tr("Close tab"), QMessageBox::AcceptRole);
        box.setDefaultButton(close);
        auto *dontAsk = new QCheckBox(tr("Don't ask again"), &box);
        box.setCheckBox(dontAsk);
        box.exec();
        if (box.clickedButton() != close)
            return;
        if (dontAsk->isChecked())
            s.setValue(QStringLiteral("library/confirmCloseTab"), false);
    }
    pinFolderTabs();
    m_tabNames.removeAt(i);
    for (const QString &f : std::as_const(folders))
        m_folderTab.remove(f);
    QStringList keep = m_lib->folders();
    for (const QString &f : std::as_const(folders))
        keep.removeAll(f);
    if (tabFromStored(s.value(QStringLiteral("library/currentTab")).toString()) == name)
        s.setValue(QStringLiteral("library/currentTab"), tabToStored(m_tabNames.value(qMin(i, int(m_tabNames.size()) - 1))));
    saveLibraryTabs();
    rebuildLibraryTabs();
    if (!folders.isEmpty()) {
        m_lib->setFolders(keep);
        m_lib->rescan();
    }
    refreshLibrary(0);
}
