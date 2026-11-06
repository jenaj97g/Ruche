-- Submits listens to ListenBrainz. A song counts as listened once you've heard half of it
-- or four minutes, whichever comes first (the usual scrobbling rule).

local current   -- what's playing: metadata plus how long it has been heard
local queue = {} -- listens that couldn't be sent yet (offline); retried later

local function token() return ruche.config("token", "") end

local function metadata(info)
  local m = { artist_name = info.artist, track_name = info.title }
  if info.album then m.release_name = info.album end
  m.additional_info = { media_player = "Ruche", media_player_version = ruche.version,
                        submission_client = "Ruche ListenBrainz plugin",
                        submission_client_version = "1.0" }
  if info.duration then m.additional_info.duration_ms = math.floor(info.duration * 1000) end
  if info.track_number then m.additional_info.tracknumber = info.track_number end
  if info.source == "radio" and info.station then m.additional_info.music_service_name = info.station end
  return m
end

local function submit(kind, payload, done)
  if token() == "" then return end
  ruche.http({
    url = ruche.config("server", "https://api.listenbrainz.org") .. "/1/submit-listens",
    headers = { Authorization = "Token " .. token() },
    json = { listen_type = kind, payload = payload },
  }, function(r)
    if r.status == 200 then
      if done then done(true) end
    else
      ruche.log(kind .. " failed: " .. tostring(r.status) .. " " .. (r.error or r.body or ""))
      if r.status == 401 then ruche.flash("ListenBrainz: the user token was refused") end
      if done then done(false, r.status) end
    end
  end)
end

local function flush()
  if #queue == 0 or token() == "" then return end
  local batch = queue
  queue = {}
  submit("import", batch, function(ok, status)
    if not ok and status ~= 400 and status ~= 401 then
      for _, l in ipairs(batch) do table.insert(queue, l) end -- keep for the next try
    end
  end)
end

local function eligible(info)
  if info.title == nil or info.artist == nil then return false end
  if info.source == "radio" then return ruche.config("radio", false) and info.is_song end
  return info.source == "library"
end

local function start(info)
  current = nil
  if not eligible(info) then return end
  current = { meta = metadata(info), started = os.time(), heard = 0, sent = false,
              need = math.min(240, (info.duration or 480) / 2) }
  if info.source == "radio" then current.need = 60 end -- radio songs: no length, a minute will do
  if ruche.config("now_playing", true) and ruche.state() == "playing" then
    submit("playing_now", { { track_metadata = current.meta } })
  end
end

-- Count only time actually played (pauses don't count).
ruche.every(5000, function()
  if current and not current.sent and ruche.state() == "playing" then
    current.heard = current.heard + 5
    if current.heard >= current.need then
      current.sent = true
      table.insert(queue, { listened_at = current.started, track_metadata = current.meta })
      flush()
    end
  end
end)
ruche.every(10 * 60 * 1000, flush)

ruche.on("track", start)
ruche.on("title", start)
ruche.on("state", function(s) if s == "stopped" then current = nil end end)

if token() == "" then
  ruche.log("no user token yet: Settings → Plugins → ListenBrainz → Configure…")
end
