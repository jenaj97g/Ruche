-- Writes "Artist - Title" (or any format) to a text file whenever the song changes.

local function target()
  local p = ruche.config("path", "")
  if p == nil or p == "" then
    local sep = ruche.platform == "windows" and "\\" or "/"
    p = ruche.data_dir .. sep .. "now-playing.txt"
  end
  return p
end

-- "{artist} - {title}": a separator next to an empty field disappears with it.
local function render(fmt, info)
  local out = fmt:gsub("{(%w+)}", function(k)
    local v = info[k]
    if v == nil then return "\0" end
    return tostring(v)
  end)
  out = out:gsub("%s*[-|:/]%s*\0", ""):gsub("\0%s*[-|:/]%s*", ""):gsub("\0", "")
  return (out:gsub("^%s+", ""):gsub("%s+$", ""))
end

local last
local function write(text)
  if text == last then return end
  last = text
  local f, err = io.open(target(), "w")
  if not f then
    ruche.log("can't write " .. target() .. ": " .. tostring(err))
    return
  end
  f:write(text)
  f:close()
end

local function update(info)
  info = info or ruche.now()
  if info.state ~= "playing" then
    if ruche.config("clear_when_stopped", true) then write("") end
    return
  end
  if info.title == nil and info.station == nil then return end
  write(render(ruche.config("format", "{artist} - {title}"), info))
end

ruche.on("track", update)
ruche.on("title", update)
ruche.on("state", function() update() end)
ruche.on("config", function() last = nil; update() end)
ruche.on("unload", function() if ruche.config("clear_when_stopped", true) then write("") end end)

ruche.log("writing to " .. target())
update()
