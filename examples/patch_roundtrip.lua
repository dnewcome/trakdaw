-- patch_roundtrip.lua — demo of daw.save_patch / daw.load_patch.
--
-- Workflow:
--   1. Load Surge XT (or any VST3) on track 1 from the REPL:
--        daw.load_plugin(1, "/usr/lib/vst3/Surge XT.vst3")
--   2. Open its editor, dial in a sound:
--        daw.show_editor(1)
--   3. Run this script to save it:
--        daw.load_script("examples/patch_roundtrip.lua")
--   4. Quit trakdaw, restart, reload the plugin, and run with LOAD = true
--      below to confirm the patch round-trips:
--        daw.load_plugin(1, "/usr/lib/vst3/Surge XT.vst3")
--        (edit this file, set LOAD = true, save → hot reload picks it up)
--
-- Note: this captures the entire plugin chain on the track, not just one
-- plugin. So if you have synth + delay + reverb, they all save and restore
-- together — that's the "instrument rack" use case.

local PATH = "patches/track1.xml"
local LOAD = false   -- flip to true to load instead of save

if LOAD then
    daw.load_patch(1, PATH)
else
    daw.save_patch(1, PATH)
end
