-- drunk_walk.lua — Phase 6: script-defined follow actions
--
-- "Drunk walk" sequencer: when a clip ends, randomly pick the next clip
-- from the available slots (can repeat the same slot or jump to another).
-- Uses daw.on_end() callbacks registered after each launch so the next
-- step is always chosen at clip-end time.
--
-- Grid:
--   Track 1 (Bass):   slot 1 = Bass A,   slot 2 = Bass B
--   Track 2 (Melody): slot 1 = Melody A, slot 2 = Melody B

math.randomseed(os.time())

local NUM_SLOTS = 2

-- Pick a random slot, biased toward *not* repeating the last one (but
-- allowed — that's what makes it "drunk").
local function next_slot(current)
    if math.random() < 0.7 then
        -- 70 %: pick the other slot
        return (current % NUM_SLOTS) + 1
    else
        -- 30 %: stay on the same slot
        return current
    end
end

-- Track which slot is currently active on each track
local active = { [1] = 1, [2] = 1 }

-- Forward-declared so callbacks can reference each other
local function arm(track, slot)
    daw.on_end(track, slot, function(info)
        local nxt = next_slot(info.slot)
        print(string.format(
            "[drunk_walk] track %d: slot %d ended → launching slot %d",
            info.track, info.slot, nxt))
        active[track] = nxt
        daw.clip(track, nxt).launch()
        -- Re-arm the callback for the new slot
        arm(track, nxt)
        -- Clear callback from the old slot so it doesn't fire again
        daw.on_end(info.track, info.slot, nil)
    end)
end

-- Kick everything off: launch slot 1 on both tracks and arm callbacks.
print("[drunk_walk] starting — Bass and Melody entering the drunk walk")
daw.clip(1, 1).launch()
arm(1, 1)

daw.clip(2, 1).launch()
arm(2, 1)

print("[drunk_walk] ready — use daw.trigger_follow(t,s) to test without audio")
