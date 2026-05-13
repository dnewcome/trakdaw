-- surge_win_smoke.lua
-- End-to-end smoke test for the Windows build:
--   1. open the Realtek audio device
--   2. load Surge XT from the standard Windows VST3 location
--   3. create a 4-beat MIDI clip with a C major arpeggio
--   4. launch the clip
--
-- Run from the REPL:  daw.load_script("scripts/surge_win_smoke.lua")

local AUDIO_DEVICE = "Speaker/HP (Realtek High Definition Audio)"
local SURGE_PATH   = "C:/Program Files/Common Files/VST3/Surge Synth Team/Surge XT.vst3"
local TRACK = 1
local SLOT  = 1

daw.open_audio_device(AUDIO_DEVICE)
daw.audio_info()

daw.load_plugin(TRACK, SURGE_PATH)
daw.plugin_info(TRACK)

daw.set_bpm(120)

-- Fresh 4-beat clip and a C major arpeggio (C E G C, quarter notes).
daw.clear_clip(TRACK, SLOT)
daw.create_clip(TRACK, SLOT, 4)
daw.add_note(TRACK, SLOT, 60, 0, 1)   -- C4
daw.add_note(TRACK, SLOT, 64, 1, 1)   -- E4
daw.add_note(TRACK, SLOT, 67, 2, 1)   -- G4
daw.add_note(TRACK, SLOT, 72, 3, 1)   -- C5

daw.play()
daw.clip(TRACK, SLOT).launch()

print("[smoke] surge_win_smoke loaded — clip launching on track " .. TRACK .. " slot " .. SLOT)
