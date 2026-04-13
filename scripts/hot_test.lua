-- hot_test.lua — version 2
daw.store.reloads = (daw.store.reloads or 0) + 1
print("loaded version 2  (reloads=" .. daw.store.reloads .. ")")

function on_midi(m)
    if m.type == "note_on" then
        print("[v2] NOTE=" .. m.note .. "  <-- hot reloaded!")
    end
end
