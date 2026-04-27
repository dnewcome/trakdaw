-- build_session.lua — programmatically build a session-view layout from
-- scratch: tracks, plugins, clips, notes. Demonstrates the recently-
-- shipped daw.add_track / create_clip / add_note APIs.
--
-- Loads Surge XT on three tracks, fills slot 1 of each with a 4-beat
-- MIDI clip, and populates the clips with a bass / chord / lead pattern
-- so launching them produces actual sound.
--
--   daw.load_script("examples/build_session.lua")
--   daw.clip(1,1).launch()
--   daw.clip(2,1).launch()
--   daw.clip(3,1).launch()
--   daw.stop()

local SURGE = "/usr/lib/vst3/Surge XT.vst3"

-- The default edit has 3 tracks. Skip add_track unless you want more.

-- Plugins on each track. Pick presets from Surge's browser to taste.
daw.load_plugin(1, SURGE)   -- bass
daw.load_plugin(2, SURGE)   -- chords
daw.load_plugin(3, SURGE)   -- lead

local function clear_and_create(track, slot, beats)
    daw.clear_clip(track, slot)         -- safe to call on empty slot
    daw.create_clip(track, slot, beats)
end

-- Bass — 4-beat clip, root + fifth pattern
clear_and_create(1, 1, 4)
daw.add_note(1, 1, 36, 0.0, 0.9, 100)   -- C2 on beat 1
daw.add_note(1, 1, 36, 1.0, 0.9, 90)    -- C2 on beat 2
daw.add_note(1, 1, 43, 2.0, 0.9, 100)   -- G2 on beat 3
daw.add_note(1, 1, 36, 3.0, 0.9, 90)    -- C2 on beat 4

-- Chords — 4-beat clip, sustained C minor triad held across the bar
clear_and_create(2, 1, 4)
for _, n in ipairs({ 60, 63, 67 }) do   -- C4, Eb4, G4
    daw.add_note(2, 1, n, 0.0, 4.0, 70)
end

-- Lead — 4-beat clip, eighth-note arpeggio
clear_and_create(3, 1, 4)
local LEAD = { 72, 75, 79, 82 }   -- C5, Eb5, G5, Bb5
for i = 0, 7 do
    daw.add_note(3, 1, LEAD[(i % 4) + 1], i * 0.5, 0.45, 95)
end

print("[build_session] built three 4-beat clips. Launch with:")
print("  daw.clip(1,1).launch()")
print("  daw.clip(2,1).launch()")
print("  daw.clip(3,1).launch()")
