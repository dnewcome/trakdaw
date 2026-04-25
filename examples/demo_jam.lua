-- demo_jam.lua — two Surge instances + two arpeggiators.
-- Track 1: bass (low octave, slower steps).
-- Track 2: lead (higher octave, faster steps).
--
-- The script sets up the rack and starts the arpeggiators; dialing in
-- patches is up to you in each Surge editor. Surge ships with a preset
-- browser — open it in either window and pick a bass / lead from the
-- factory library, or save your own with daw.save_patch.
--
--   daw.load_script("examples/demo_jam.lua")
--   daw.cancel_all()                   -- stop the arpeggiators
--   daw.save_patch(1, "patches/bass.xml")    -- after dialing in a sound
--   daw.save_patch(2, "patches/lead.xml")

local SURGE = "/usr/lib/vst3/Surge XT.vst3"

-- Drop any pending scheduler tasks from a previous run.
daw.cancel_all()

-- Two Surges. Reload from saved patches if you have them.
daw.load_plugin(1, SURGE)
daw.load_plugin(2, SURGE)
-- daw.load_patch(1, "patches/bass.xml")
-- daw.load_patch(2, "patches/lead.xml")

-- Show editors so you can pick presets visually.
daw.show_editor(1)
daw.show_editor(2)

-- Bass arpeggiator: C minor seventh, root + 5th + b3 + b7, low octave.
local BASS_NOTES = { 36, 43, 39, 46 }   -- C2, G2, Eb2, Bb2
local BASS_STEP  = 0.5                  -- half-beat per note

daw.every(BASS_STEP, function(n)
    local note = BASS_NOTES[((n - 1) % #BASS_NOTES) + 1]
    daw.note_on(1, note, 100)
    daw.after(BASS_STEP * 0.9, function() daw.note_off(1, note) end)
    daw.emit("jam", { track = "bass", note = note, step = n })
end)

-- Lead arpeggiator: same scale, two octaves up, twice as fast, with a
-- 5th note for asymmetry against the bass cycle.
local LEAD_NOTES = { 60, 63, 67, 70, 72 }   -- C4, Eb4, G4, Bb4, C5
local LEAD_STEP  = 0.25

daw.every(LEAD_STEP, function(n)
    local note = LEAD_NOTES[((n - 1) % #LEAD_NOTES) + 1]
    daw.note_on(2, note, 88)
    daw.after(LEAD_STEP * 0.9, function() daw.note_off(2, note) end)
    daw.emit("jam", { track = "lead", note = note, step = n })
end)

print("[demo_jam] running — bass on track 1 (every "
      .. BASS_STEP .. " beats), lead on track 2 (every "
      .. LEAD_STEP .. " beats)")
print("[demo_jam] daw.cancel_all() to stop")
