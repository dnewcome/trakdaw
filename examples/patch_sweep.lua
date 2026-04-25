-- patch_sweep.lua — generate a bank of patches by sweeping one parameter.
-- Demonstrates set_param + save_patch as a pair: load a synth, programmatically
-- vary one parameter, save each variation as its own .xml file.
--
-- Workflow:
--   daw.load_plugin(1, "/usr/lib/vst3/Surge XT.vst3")
--   daw.list_params(1)                                  -- find the param ID you want
--   -- edit PARAM below to match, then:
--   daw.load_script("examples/patch_sweep.lua")
--
-- Resulting files: patches/sweep_<value>.xml — load each with
--   daw.load_patch(1, "patches/sweep_0.5.xml")

local TRACK = 1
local PARAM = "filter1.cutoff"   -- adjust to a real param ID from list_params
local STEPS = 10

for i = 0, STEPS do
    local v = i / STEPS
    if not daw.set_param(TRACK, PARAM, v) then
        print("[patch_sweep] aborting — param not found: " .. PARAM)
        return
    end
    local path = string.format("patches/sweep_%.2f.xml", v)
    daw.save_patch(TRACK, path)
    daw.emit("patch_sweep", { step = i, value = v, path = path })
end

print(string.format("[patch_sweep] wrote %d patches sweeping %s 0..1",
                    STEPS + 1, PARAM))
