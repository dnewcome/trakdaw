-- midi_probe.lua — decode every incoming MIDI message and republish it
-- as a 'midi' event on the SSE stream. Good for:
--   - confirming a controller is wired up
--   - watching what a keyboard/knob actually sends
--   - driving a custom visualizer that only cares about MIDI
--
-- This does NOT route MIDI to an instrument. For that, either add
-- daw.note_on calls below, or use Tracktion's native routing:
--   daw.assign_midi_input("keyboard-name", track)
--
--   daw.load_script("examples/midi_probe.lua")

local NOTE_NAMES = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" }
local function note_name(n)
    return NOTE_NAMES[(n % 12) + 1] .. tostring(math.floor(n / 12) - 1)
end

function on_midi(m)
    local out = { channel = m.channel, type = m.type }
    if m.type == "note_on" or m.type == "note_off" then
        out.note     = m.note
        out.name     = note_name(m.note)
        out.velocity = m.velocity
    elseif m.type == "cc" then
        out.cc    = m.cc
        out.value = m.value
    elseif m.type == "pitchbend" then
        out.value = m.value
    end
    daw.emit("midi", out)
end

print("[midi_probe] installed — every MIDI message will be emitted")
