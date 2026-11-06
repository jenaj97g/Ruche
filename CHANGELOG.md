# Changelog

## 0.1.6 (2026-11-06)

### Added
- **Plugins**, written in Lua 5.4. They react to what's playing (track, radio title,
  station, state), control playback, show notifications, add entries to ☰ → Plugins, run
  timers and make web requests; a plugin can declare settings that Ruche shows as a form.
  Settings → Plugins lists them (off until ticked), with Configure…, Log…, Reload and Open
  plugins folder. A plugin that runs for more than 3 s at a time is stopped without taking
  Ruche down. The API is documented in [PLUGINS.md](PLUGINS.md).
- Three example plugins: **Sleep timer** (stops after 15 to 90 minutes, fading out),
  **ListenBrainz** (scrobbles to listenbrainz.org, with a "playing now" status), and
  **Now playing file** (writes the current song to a text file for OBS overlays).
- **Discord Rich Presence**: "Listening to Ruche" with the song, artist, album cover and a
  progress bar. Off unless you turn it on (Settings → Desktop); nothing is shared while
  paused or stopped. Works with the Discord apps and with Vesktop, installed natively, as a
  Flatpak or a Snap, and on Windows.
  - Album covers are looked up by artist and album on iTunes, then MusicBrainz / Cover Art
    Archive (Discord can only show images from the web), and remembered. Can be turned off.
  - Radio shows only a clean "Artist - Title", with the station's logo. Anything else a
    station sends as its title (slogans, web addresses, frequencies, ads) is never shown;
    Discord shows the station's name instead.
- **Flatpak**: `ruche-<version>.flatpak`, installed with `flatpak install --user`.
- **Updates** from GitHub releases. On first start Ruche asks whether to check on each
  start (Settings → General changes it); About Ruche has a Check for updates button. The
  update installs itself the right way for an AppImage, .deb/.rpm (through `pkexec`),
  Flatpak, or the Windows installer, and every download is checked against the release's
  SHA256SUMS.
- **Library tabs**: put music folders in separate tabs (say Music and Airchecks), each
  with its own browser. Set up in Settings → Library; double-click a tab to rename it.
- **Languages**: Belarusian, Catalan, Chinese (Simplified), Czech, Dutch, French, German, Hungarian,
  Irish, Japanese, Portuguese (Portugal and Brazil), Romanian, Russian, Serbian (Cyrillic
  and Latin), Spanish and Swedish, besides English. Follows the system language, or pick
  one in the new Settings → General tab. Machine-translated; corrections welcome.

### Changed
- Inside Flatpak, notifications, MPRIS and the Wayland app id use the Flatpak app id
  (`be.weizkule.Ruche`), so the right icon and name show.
- Narrow track columns (Year, Time, Plays…) widen to fit their header in any language.

### Fixed
- ☰ → Quit and the tray's Quit didn't quit when Close to tray was on.

## 0.1.5 (2026-11-06)

### Added
- **Themes.** Dark stays the default. Also Light, Midnight, Frost, Forest, Plum, Ember,
  Paper and Black (OLED), plus **Match the system**, which follows the desktop's dark/light
  setting and switches along with it.
- **Accent colour** on top of any theme: eight ready-made ones or any colour. An accent
  that would be hard to read as text is darkened or lightened for text automatically;
  buttons and sliders keep the exact colour.
- **Custom theme** from a background and a text colour; every other shade is mixed from them.
- Theme changes show immediately in Settings; Cancel restores the previous look.

### Changed
- **Settings** is now split into tabs: Library, Playback, Appearance, Desktop.
- "Show in applications menu", "Close to tray" and "Song notifications" moved from the ☰
  menu into Settings → Desktop.
- Station logos with white artwork get a dark tile on light themes (dark artwork already
  got a light tile on dark themes).
- The app icon, tray icon and launcher icon stay honey whatever the theme.

### Fixed
- A station list whose `tvg-logo` entries are `file://` paths was taken for a local
  playlist and imported as nothing. Station lists are now recognised by their stream
  addresses.

## 0.1.4 (2026-11-06)

### Added
- **.rpm and .deb packages** (Fedora 40+, openSUSE Tumbleweed, Ubuntu 24.04+, Debian 13).
  They hold the same program as the AppImage under `/opt/ruche`, with `ruche` on the PATH,
  a menu entry with Play/Pause, Next and Previous actions, and icons.

### Fixed
- A pinned taskbar or menu entry stopped working once the AppImage it was made for was
  replaced, renamed or moved. The launcher now goes through a small script that finds the
  newest Ruche AppImage next to the old one or in `~/Applications`, `~/.local/bin`, `~/bin`
  or Downloads, and says so plainly if there is none. Run the new AppImage once to update
  a launcher made by an older version.

## 0.1.3 (2026-11-06)

### Added
- **Song notifications**: a pop-up with the title, artist and album and the cover when a
  track starts, or the song, artist, station and logo when the radio title changes.
  Off / when Ruche isn't the active window (default) / always.
  - Linux: desktop notifications that replace each other instead of piling up, stay out
    of the notification history, have a **Next** button, and bring Ruche forward on click.
  - Windows: Windows notifications from the tray icon.
- **Windows build**: installer (Start menu entry, "Open with" for audio files and
  playlists, uninstaller) and portable zip. Same lean audio-only libmpv as the AppImage,
  with WASAPI output and Schannel for HTTPS. Media keys work while another window has the
  focus; a second launch hands its files to the open window.
- `RUCHE_LOG_FILE=<path>` writes Ruche's log to a file (useful on Windows, which has no
  terminal for it).

### Fixed
- File names outside the system code page (Cyrillic and so on) are read correctly on
  Windows, for tags, covers and the library.
