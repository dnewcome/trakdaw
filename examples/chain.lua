-- chain.lua — deterministic follow-action sequence. Each clip flips to
-- the other slot when it ends, so track 1 goes 1→2→1→2... forever, same
-- for track 2. Compare scripts/drunk_walk.lua for the probabilistic
-- version.
--
-- Publishes a 'chain_step' event on each transition, which shows up in
-- the browser log (and would light up any custom visualizer).
--
--   daw.load_script("examples/chain.lua")
--
-- Testing without audio: daw.trigger_follow(1, 1) will pretend slot 1
-- just finished on track 1 and fire the callback.

local function flip(slot) return (slot % 2) + 1 end

local function arm(track, slot)
    daw.on_end(track, slot, function(info)
        local nxt = flip(info.slot)
        daw.emit("chain_step", {
            track = info.track, from = info.slot, to = nxt,
        })
        daw.clip(info.track, nxt).launch()
        arm(info.track, nxt)
        -- clear the old slot so its callback doesn't fire again
        daw.on_end(info.track, info.slot, nil)
    end)
end

print("[chain] launching 1,1 and 2,1 — each will flip to the other slot on end")
daw.clip(1, 1).launch(); arm(1, 1)
daw.clip(2, 1).launch(); arm(2, 1)
