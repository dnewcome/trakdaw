-- play_keyboard.lua — load Surge on track 1 and route every connected
-- MIDI keyboard to it via Tracktion's "All MIDI Ins" virtual device.
-- Notes go straight into Surge with no on_midi handler in the way.
--
--   daw.load_script("examples/play_keyboard.lua")
--
-- If your keyboard isn't responding, run daw.list_engine_midi_inputs()
-- to see what Tracktion actually sees and adjust DEVICE_HINT below.

local SURGE        = "/usr/lib/vst3/Surge XT.vst3"
local TRACK        = 1
local DEVICE_HINT  = "All MIDI"   -- fuzzy match; "" picks the first device

daw.load_plugin(TRACK, SURGE)
daw.show_editor(TRACK)

if daw.assign_midi_input(DEVICE_HINT, TRACK) then
    print("[play] " .. DEVICE_HINT .. " → track " .. TRACK
          .. " — start playing!")
else
    print("[play] couldn't bind '" .. DEVICE_HINT
          .. "'. Available MIDI inputs:")
    daw.list_engine_midi_inputs()
end
