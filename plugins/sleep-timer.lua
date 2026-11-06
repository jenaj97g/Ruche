-- @name Sleep timer
-- @version 1.0
-- @author Ruche
-- @description Stops playback after 15, 30, 60 or 90 minutes, fading out over the last 30 seconds.

local timer, fade, ends, volume

local function cancel(quiet)
  if timer then ruche.cancel(timer) timer = nil end
  if fade then ruche.cancel(fade) fade = nil end
  if volume then ruche.volume(volume) volume = nil end
  ends = nil
  if not quiet then ruche.flash("Sleep timer cancelled") end
end

local function set(minutes)
  cancel(true)
  ends = os.time() + minutes * 60
  ruche.flash(("Playback stops at %s"):format(os.date("%H:%M", ends)))
  timer = ruche.after(math.max(0, minutes * 60 - 30) * 1000, function()
    timer = nil
    volume = ruche.volume()
    local steps, step = 30, 0
    fade = ruche.every(1000, function()
      step = step + 1
      ruche.volume(math.floor(volume * (1 - step / steps)))
      if step >= steps then
        ruche.cancel(fade) fade = nil
        ruche.pause()
        ruche.volume(volume) volume = nil -- back to normal for next time
        ends = nil
        ruche.notify("Sleep timer", "Playback stopped. Good night.")
      end
    end)
  end)
end

for _, m in ipairs({ 15, 30, 60, 90 }) do
  ruche.add_action(("Sleep in %d minutes"):format(m), function() set(m) end)
end
ruche.add_action("Cancel sleep timer", function()
  if ends then cancel(false) else ruche.flash("No sleep timer running") end
end)
ruche.on("unload", function() cancel(true) end)
