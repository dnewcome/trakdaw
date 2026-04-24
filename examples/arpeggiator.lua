-- arpeggiator.lua — a simple Lua-driven arpeggiator built on daw.every.
-- Plays a four-note pattern (C major triad + octave) at 1/4-beat
-- intervals on track 1. Assumes you already loaded an instrument —
-- e.g. daw.load_4osc(1) or daw.load_plugin(1, "/path/to/Surge XT.vst3").
--
--   daw.load_script("examples/arpeggiator.lua")
--
-- Stop it: daw.cancel_all()   (or let it finish at count == 32)

local PATTERN = { 60, 64, 67, 72 }   -- C major triad + octave
local STEP    = 0.25                 -- beats per note
local GATE    = 0.22                 -- note length in beats (slightly < step)
local VEL     = 96
local TRACK   = 1
local MAX     = 32                   -- stop after 32 notes

-- Cancel any prior run of this script (hot-reload already clears the
-- scheduler, but an explicit cancel makes the REPL version predictable too).
daw.cancel_all()

daw.every(STEP, function(n)
    local note = PATTERN[((n - 1) % #PATTERN) + 1]
    daw.note_on(TRACK, note, VEL)
    daw.after(GATE, function() daw.note_off(TRACK, note) end)
    daw.emit("arp_step", { step = n, note = note })
    if n >= MAX then return false end
end)

print("[arp] running — " .. MAX .. " steps at " .. STEP .. " beats/step")
