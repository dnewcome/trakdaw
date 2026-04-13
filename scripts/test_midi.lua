-- test_midi.lua: basic on_midi callback test
function on_midi(m)
    local lat = string.format("%.3f", m.latency_ms)
    if m.type == "note_on" then
        print("[MIDI] note_on  note=" .. m.note .. " vel=" .. m.velocity
              .. " ch=" .. m.channel .. " lat=" .. lat .. "ms")
    elseif m.type == "note_off" then
        print("[MIDI] note_off note=" .. m.note .. " ch=" .. m.channel)
    elseif m.type == "cc" then
        print("[MIDI] cc=" .. m.cc .. " val=" .. m.value)
    else
        print("[MIDI] " .. m.type)
    end
end

print("on_midi callback registered")
