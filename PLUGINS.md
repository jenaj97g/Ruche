# Writing Ruche plugins

Plugins are small [Lua 5.4](https://www.lua.org/manual/5.4/) scripts. They react to what
Ruche plays, can control playback, pop up notifications, add entries to the ☰ → Plugins
menu, run timers and talk to web services. Ruche ships three examples (in Settings →
Plugins, marked "example"): a sleep timer, a ListenBrainz scrobbler, and a now-playing
text file for stream overlays. Their source is in [`plugins/`](plugins/).

## Where plugins live

| | Linux | Windows |
|---|---|---|
| Your plugins | `~/.local/share/ruche/plugins/` | `%LOCALAPPDATA%\ruche\plugins\` |
| Examples shipped with Ruche | `<install>/share/ruche/plugins/` | `<install>\plugins\` |
| Plugin data (`ruche.data_dir`) | `~/.local/share/ruche/plugin-data/<id>/` | `%LOCALAPPDATA%\ruche\plugin-data\<id>\` |

Settings → Plugins → **Open plugins folder** opens yours. A plugin is either

- a single file, `sleep-timer.lua`, or
- a folder with `main.lua` and optionally `plugin.json` (and any modules you `require`):

```
my-plugin/
  main.lua
  plugin.json
  helpers.lua     -- require("helpers")
```

The file or folder name is the plugin's **id**. A plugin in your folder replaces an
example with the same id, so you can copy an example and change it.

New plugins are **off** until you tick them in Settings → Plugins. After editing a plugin,
press **Reload**. **Log…** shows what it printed and any errors.

Plugins run with your user's permissions (they can read and write files, like any
program you run). Only turn on plugins you trust.

## Describing a plugin

A single-file plugin describes itself in its first comment lines:

```lua
-- @name Sleep timer
-- @version 1.0
-- @author you
-- @description Stops playback after a while.
```

A folder plugin uses `plugin.json`, which can also declare **settings**. Ruche shows them
as a form (Settings → Plugins → Configure…) and the plugin reads them with `ruche.config`:

```json
{
  "name": "ListenBrainz",
  "version": "1.0",
  "description": "Sends what you listen to to ListenBrainz.",
  "author": "you",
  "settings": [
    { "key": "token", "label": "User token", "type": "password", "default": "",
      "help": "From listenbrainz.org/settings." },
    { "key": "radio", "label": "Also send radio songs", "type": "bool", "default": false },
    { "key": "delay", "label": "Delay (s)", "type": "int", "default": 30, "min": 0, "max": 600 },
    { "key": "mode", "label": "Mode", "type": "choice", "choices": ["fast", "careful"], "default": "fast" }
  ]
}
```

Setting types: `string`, `password` (hidden while not being edited), `bool`, `int`
(with `min`/`max`), `choice` (with `choices`).

## A first plugin

```lua
-- @name Hello
-- @description Says what's playing.

ruche.on("track", function(t)
  ruche.notify(t.title, t.artist or "")
end)

ruche.on("title", function(t)            -- radio title changed
  print("on air: " .. (t.stream_title or ""))
end)

ruche.add_action("Say hello", function()
  ruche.flash("Hello from a plugin!")
end)
```

The script runs once when the plugin starts; from then on Ruche calls the functions you
registered. Everything runs on Ruche's main thread, so keep callbacks short: a callback
that runs longer than **3 seconds** is stopped (the error shows in the plugin list) and
the plugin carries on with its next event. For anything slow, use `ruche.http` (it doesn't
block) and timers.

## API

Everything is in the global table `ruche`. `print(...)` writes to the plugin's log.

### Events: `ruche.on(event, function)`

| Event | Argument | When |
|---|---|---|
| `track` | now-playing table | a library track starts |
| `title` | now-playing table | a radio station's title changes |
| `station` | `{station, station_url}` | a radio station starts |
| `state` | `"playing"`, `"paused"` or `"stopped"` | playback state changes |
| `config` | the plugin's settings table | the user saved its settings |
| `unload` | | the plugin is turned off or reloaded |
| `quit` | | Ruche is closing |

When a plugin starts while something is already playing, it gets a `track` (or `title`)
event straight away.

### What's playing

`ruche.now()` returns the **now-playing table** (the same one `track` and `title` get).
Fields that are unknown are absent (`nil`):

| Field | |
|---|---|
| `source` | `"library"`, `"radio"` or `"none"` |
| `state` | `"playing"`, `"paused"`, `"stopped"` |
| `title`, `artist`, `album`, `album_artist`, `genre` | for radio, `artist` and `title` come from an "Artist - Title" stream title |
| `year`, `track_number`, `disc_number` | integers |
| `duration`, `position` | seconds |
| `path` | the file (library) |
| `station`, `station_url`, `stream_title` | radio |
| `is_song` | radio: `artist` and `title` look like a real song (not the station's slogan, web address or frequency) |
| `logo_url`, `logo_file` | the station's logo, if Ruche has one |

Also: `ruche.state()`, `ruche.position()` (seconds), `ruche.history()` (the radio's
on-air history: a list of `{title, time}`, newest first, `time` in Unix seconds),
`ruche.stations()` (your saved stations: `{name, url, last_title}`).

### Playback

`ruche.play()`, `ruche.pause()`, `ruche.play_pause()`, `ruche.stop()`, `ruche.next()`,
`ruche.previous()`, `ruche.seek(seconds)`, `ruche.volume([0..100])` (returns the
volume; sets it when given a number), `ruche.play_station(url [, name])`.

### Interface

| | |
|---|---|
| `ruche.notify(summary [, body])` | a desktop notification (a Windows notification on Windows) |
| `ruche.flash(text)` | a short message in Ruche's status bar |
| `ruche.raise()` | brings the window to the front |
| `ruche.add_action(label, function)` | adds an entry to ☰ → Plugins |
| `ruche.log(...)` / `print(...)` | writes to the plugin's log (Settings → Plugins → Log…) |

### Timers

`ruche.after(ms, function)` runs once, `ruche.every(ms, function)` repeats (50 ms at the
fastest). Both return an id for `ruche.cancel(id)`. Timers stop when the plugin is turned off.

### Web requests

```lua
ruche.http({
  url = "https://api.example.org/thing",  -- http or https
  method = "POST",                         -- default: GET, or POST when there's a body
  headers = { Authorization = "Token abc" },
  json = { hello = "world" },              -- sent as JSON (or body = "raw text")
  timeout = 15000,                         -- ms, default 15 s
}, function(r)
  -- r.status (0 when no answer), r.body, r.headers (lower-case names),
  -- r.json (decoded, when the answer is JSON), r.error (text, when it failed)
end)
```

Requests never block; the function is called when the answer arrives. Redirects to the
same or a safer scheme are followed.

### JSON and settings

`ruche.json_encode(value)` and `ruche.json_decode(text)` (returns `nil, error` for bad
JSON). An empty table encodes as `{}`.

`ruche.config(key [, default])` reads a setting (the user's value, else the default from
`plugin.json`, else `default`); `ruche.config()` returns them all.
`ruche.set_config(key, value)` stores a value (any JSON-able value; `nil` removes it).
Both use `config.json` in the plugin's data folder.

### Information

`ruche.version` (Ruche's version), `ruche.api_version` (`"1"`; it goes up only for
changes that could break plugins), `ruche.platform` (`"linux"` or `"windows"`),
`ruche.id`, `ruche.plugin_dir`, `ruche.data_dir` (the plugin's own writable folder).

### Lua libraries

The standard libraries are all there (`string`, `table`, `math`, `utf8`, `os`, `io`,
`coroutine`, `debug`). `require` finds modules in the plugin's folder; native (C) modules
aren't loaded.
