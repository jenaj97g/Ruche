# Ruche themes

A theme file can restyle every part of Ruche. It's a Qt stylesheet (`.qss`): CSS-like
rules for Qt widgets, layered on top of Ruche's own stylesheet. Themes can't run code,
so a downloaded one is safe to try.

Pick one in **Settings → Appearance → Theme file**. **Import…** takes a `.qss` file, a
theme folder, or a downloaded `.zip`. **None** goes back to the plain look.

## Quick start

1. Settings → Appearance → **Open themes folder**.
2. Make a file there called `mine.qss`:

   ```css
   /**
    * @name Mine
    * @author you
    * @window #101418
    * @text #e8eef2
    * @accent #4fc3f7
    */

   QWidget#playerBar { border-top: 2px solid @accent; }
   QLabel#brand { font-size: 18px; }
   ```

3. Choose **Mine** under Theme file. While it's in use, Ruche reloads it every time you
   save, so you can keep it open in an editor and watch the changes.

## The header

The first `/* … */` comment in the file describes the theme. Every line is optional:

| Line | What it does |
|---|---|
| `@name` | Name shown in Settings (default: the file name, or the folder name for `theme.qss`) |
| `@author`, `@version`, `@description` | Shown in Settings |
| `@window` | Background colour |
| `@text` | Text colour |
| `@accent` | Accent colour (play button, sliders, selection marks). An accent picked in Settings still wins |

`@window` and `@text` only count together. With them, every other shade in Ruche
(panels, lines, hover, selection…) is mixed from those two colours. Without them, the
theme picked above it in Settings provides the colours.

## Colour tokens

Anywhere in the rules, `@name` is replaced by one of Ruche's current colours. A theme that
uses tokens instead of fixed colours keeps working with whatever colours the user picks.

| Token | What it is |
|---|---|
| `@window`, `@text`, `@accent` | The three base colours |
| `@accenttext` | The accent, adjusted to stay readable as text |
| `@onaccent` | Text and icons drawn on an accent fill |
| `@accenthover`, `@accentpressed` | Accent fills when hovered and pressed |
| `@strong`, `@soft`, `@dim`, `@status`, `@faint` | Text from brightest to faintest |
| `@seltext` | Text on a selection |
| `@icon` | Icon colour |
| `@deep1` … `@deep4` | Background darker than the window (or lighter, in light themes): browser, sidebar, top bar, player bar |
| `@base`, `@altbase` | List rows, and alternate rows |
| `@dialog` | Dialog background |
| `@field`, `@raised` | Text fields and menus; buttons and tooltips |
| `@line`, `@line1` … `@line4` | Borders, from faint to clear |
| `@navsel`, `@browsesel`, `@selection` | Selection in the sidebar, the browser, and lists |
| `@groove` | Slider tracks |
| `@scroll`, `@scrollhover` | Scroll bar handles |

## What to style

Standard Qt classes work as selectors (`QPushButton`, `QTableView`, `QMenu`…), and
Ruche's own parts have names you can target with `#name`:

| Selector | Part |
|---|---|
| `QWidget#topBar` | The bar at the top (☰, logo, view title, search) |
| `QLabel#brand`, `QLabel#viewTitle`, `QLineEdit#search` | Its logo text, title and search field |
| `QTreeWidget#navigator` | The sidebar (Music, Now Playing, Radio, playlists) |
| `QWidget#libTabRow`, `QTabBar#libTabs` | The library tab bar |
| `QToolButton#libTabAdd`, `QToolButton#libTabClose` | Its + and × buttons |
| `QListView#browser` | The Genre / Artist / Album columns |
| `QTableView` | The track list (`QHeaderView::section` for its headers) |
| `QWidget#statusBar`, `QLabel#status` | The line under the track list |
| `QWidget#nowPlaying` | The panel on the right |
| `QLabel#npTitle`, `QLabel#npArtist`, `QLabel#npAlbum`, `QLabel#npTech` | Its text |
| `QListWidget#upNext` | The Up next list |
| `QWidget#playerBar` | The bar at the bottom |
| `QLabel#barTitle`, `QLabel#barArtist`, `QLabel#time` | Its text and the times |
| `QToolButton#playButton` | The round play button |
| `QSlider` | The seek bar and volume (`::groove`, `::sub-page`, `::handle`) |
| `QLabel#sectionLabel`, `QLabel#hint` | Section headings and grey hints in dialogs |
| `QDialog`, `QMenu`, `QToolTip` | Dialogs, menus, tooltips |

## Pictures

`url(...)` paths are relative to the theme file, so a theme can bring its own images:

```css
QTreeWidget#navigator { background-image: url(grid.png); }
```

Put the `.qss` and its images in one folder. If the folder's stylesheet is called
`theme.qss`, the theme is named after the folder.

## Sharing a theme

Zip the folder, or share the single `.qss` file. Importing a theme with the same name again
replaces the old copy, which is how a theme gets updated.

## Limits

These come from Qt stylesheets, not from Ruche:

- No animations or transitions, no blur, and no `::before` / `::after` or `content`.
- Fonts: `font-family`, `font-size`, `font-weight` work. Ruche doesn't load font files,
  so use fonts that are installed.
- The music icons (play, next…) are drawn by Ruche in `@icon` / `@onaccent` colours. Themes
  can't swap them for images. The play button's icon is drawn on an accent fill, so keep that
  button's background accent-coloured.

## Examples

Ruche ships two, listed as *(example)* in Settings:
[`themes/synthwave/`](themes/synthwave/theme.qss) (own colours, gradients and an image)
and [`themes/terminal.qss`](themes/terminal.qss) (one file, monospace, square corners).
Copy one into your themes folder to start from it.
