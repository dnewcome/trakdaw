-- panic.lua — emergency stop: halts transport, stops every clip slot,
-- sends note-off for every note on every track, clears pending follow
-- callbacks. Useful when a generative script goes rogue or a stuck note
-- won't shut up.
--
--   daw.load_script("examples/panic.lua")

local NUM_TRACKS = 2
local NUM_SLOTS  = 2

print("[panic] stopping transport")
daw.stop()

print("[panic] stopping every clip slot")
for t = 1, NUM_TRACKS do
    for s = 1, NUM_SLOTS do
        daw.clip(t, s).stop()
    end
end

print("[panic] note-off for all 128 notes on every track")
for t = 1, NUM_TRACKS do
    for note = 0, 127 do
        daw.note_off(t, note)
    end
end

print("[panic] clearing follow-action callbacks")
for t = 1, NUM_TRACKS do
    for s = 1, NUM_SLOTS do
        daw.on_end(t, s, nil)
    end
end

daw.emit("panic", { at = os.time() })
print("[panic] done")
