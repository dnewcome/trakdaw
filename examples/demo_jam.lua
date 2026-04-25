-- demo_jam.lua — three Surge instances playing together.
--   Track 1: bass     (low octave, half-beat steps)
--   Track 2: lead     (mid octave, quarter-beat steps)
--   Track 3: drums    (Surge with a drum-style patch)
--
-- The script sets up the rack and starts the loops; dialing in patches
-- is up to you in each Surge editor. Surge ships with a preset browser
-- — open it in each window and pick a bass / lead / drum patch from
-- the factory library, then save your selection with daw.save_patch.
--
--   daw.load_script("examples/demo_jam.lua")
--   daw.cancel_all()                         -- stop everything
--   daw.save_patch(1, "patches/bass.xml")
--   daw.save_patch(2, "patches/lead.xml")
--   daw.save_patch(3, "patches/drums.xml")
--
-- Drums note: Surge's drum-style patches (look for "Percussion" in the
-- factory library) usually map a single drum sound to one playing voice
-- — note pitch may shift the timbre slightly. For a real multi-drum
-- kit, point Sitala or Geonkick at a different track instead.

local SURGE = "/usr/lib/vst3/Surge XT.vst3"

daw.cancel_all()

daw.load_plugin(1, SURGE)   -- bass
daw.load_plugin(2, SURGE)   -- lead
daw.load_plugin(3, SURGE)   -- drums

-- Optional: load saved patches. Uncomment after saving them once.
-- daw.load_patch(1, "patches/bass.xml")
-- daw.load_patch(2, "patches/lead.xml")
-- daw.load_patch(3, "patches/drums.xml")

daw.show_editor(1)
daw.show_editor(2)
daw.show_editor(3)

-- Bass: C minor 7, low octave.
local BASS = { 36, 43, 39, 46 }       -- C2, G2, Eb2, Bb2
local BASS_STEP = 0.5

daw.every(BASS_STEP, function(n)
    local note = BASS[((n - 1) % #BASS) + 1]
    daw.note_on(1, note, 100)
    daw.after(BASS_STEP * 0.9, function() daw.note_off(1, note) end)
    daw.emit("jam", { track = "bass", note = note, step = n })
end)

-- Lead: same scale, higher, faster, asymmetric note count for movement
-- against the bass cycle.
local LEAD = { 60, 63, 67, 70, 72 }   -- C4, Eb4, G4, Bb4, C5
local LEAD_STEP = 0.25

daw.every(LEAD_STEP, function(n)
    local note = LEAD[((n - 1) % #LEAD) + 1]
    daw.note_on(2, note, 88)
    daw.after(LEAD_STEP * 0.9, function() daw.note_off(2, note) end)
    daw.emit("jam", { track = "lead", note = note, step = n })
end)

-- Drums: kick on every beat, accent on beats 2 & 4 with a higher note
-- (gives Surge's voicing a snare-ish character on most drum patches).
-- Adjust KICK / SNARE pitches to match whatever patch you picked.
local KICK   = 36   -- C1
local SNARE  = 40   -- E1 — use an unused drum slot in your patch
local DRUM_STEP = 1   -- one beat per step

daw.every(DRUM_STEP, function(n)
    local beat = ((n - 1) % 4) + 1     -- 1..4 within the bar
    local hit  = (beat == 2 or beat == 4) and SNARE or KICK
    daw.note_on(3, hit, 110)
    daw.after(0.1, function() daw.note_off(3, hit) end)
    daw.emit("jam", { track = "drums", beat = beat, note = hit, step = n })
end)

print("[demo_jam] running — bass/lead/drums on tracks 1/2/3")
print("[demo_jam] daw.cancel_all() to stop")
