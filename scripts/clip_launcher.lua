-- clip_launcher.lua
-- Map MIDI notes to clip slots:
--   C3 (note 48) → Bass A (track 1, slot 1)
--   D3 (note 50) → Melody A (track 2, slot 1)
-- Any note-off or low velocity stops all clips.

local note_map = {
    [48] = { track = 1, slot = 1 },   -- C3 → Bass A
    [50] = { track = 2, slot = 1 },   -- D3 → Melody A
}

function on_midi(m)
    if m.type == "note_on" then
        local mapping = note_map[m.note]
        if mapping then
            print(string.format("[launcher] note %d → clip(%d,%d) launch",
                m.note, mapping.track, mapping.slot))
            daw.play()
            daw.clip(mapping.track, mapping.slot).launch()
        end
    elseif m.type == "note_off" then
        local mapping = note_map[m.note]
        if mapping then
            print(string.format("[launcher] note %d → clip(%d,%d) stop",
                m.note, mapping.track, mapping.slot))
            daw.clip(mapping.track, mapping.slot).stop()
        end
    end
end

print("clip_launcher ready — C3/D3 launch Bass A / Melody A")
