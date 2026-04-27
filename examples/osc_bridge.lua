-- osc_bridge.lua — minimal OSC listener that translates incoming
-- SuperDirt-style /dirt/play messages into daw.note_on calls.
--
-- Designed for routing TidalCycles patterns into trakdaw VSTs without
-- needing SuperCollider in the loop:
--
--   tidal repl  →  /dirt/play OSC msgs  →  this script  →  daw.note_on  →  Surge XT
--
-- Usage:
--   1. Load Surge (or anything) on track 1, 2, 3 and dial in patches.
--      daw.load_plugin(1, "/usr/lib/vst3/Surge XT.vst3")
--      daw.load_plugin(2, "/usr/lib/vst3/Surge XT.vst3")
--      daw.load_plugin(3, "/usr/lib/vst3/Surge XT.vst3")
--   2. Load this script:
--      daw.load_script("examples/osc_bridge.lua")
--   3. Verify with a CLI test:
--      oscsend localhost 57120 /dirt/play s bass n 0 gain 1.0
--   4. Point Tidal at port 57120 (instead of SuperDirt's default 57110).

local PORT = 57120

-- Map SuperDirt "sound" names to your trakdaw tracks. Add as many as
-- you want; unknown names will print a warning the first time.
local SOUND_TO_TRACK = {
    bass  = 1,
    bd    = 1,
    chord = 2,
    keys  = 2,
    lead  = 3,
    pad   = 3,
}

-- Center pitch for sounds with no explicit "note" — Tidal sends "n" as
-- a relative offset in semitones, so the absolute MIDI note becomes
-- DEFAULT_NOTE + n.
local DEFAULT_NOTE = 60   -- C4

local GATE_BEATS = 0.25   -- note length

local warned = {}

local function parse_kv(args)
    local kv = {}
    for i = 1, #args, 2 do
        kv[args[i]] = args[i + 1]
    end
    return kv
end

function on_osc(addr, args)
    if addr ~= "/dirt/play" then return end

    local p = parse_kv(args)
    local sound = p.s
    if not sound then return end

    local track = SOUND_TO_TRACK[sound]
    if not track then
        if not warned[sound] then
            print("[osc-bridge] unknown sound: " .. sound)
            warned[sound] = true
        end
        return
    end

    local note = DEFAULT_NOTE + math.floor(p.n or 0)
    local vel  = math.max(1, math.min(127, math.floor((p.gain or 1.0) * 100)))

    daw.note_on(track, note, vel)
    daw.after(GATE_BEATS, function() daw.note_off(track, note) end)

    daw.emit("tidal_hit", {
        sound = sound, track = track, note = note, vel = vel,
    })
end

if daw.osc_listen(PORT) then
    print("[osc-bridge] /dirt/play handler listening on " .. PORT)
    print("[osc-bridge] sound → track:")
    for s, t in pairs(SOUND_TO_TRACK) do
        print(string.format("    %-8s → track %d", s, t))
    end
else
    print("[osc-bridge] could not bind port " .. PORT
          .. " — is something else listening?")
end
