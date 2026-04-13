# trakdaw

A scriptable, clip-based DAW built on [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) and [JUCE](https://juce.com/), with a live Lua REPL, hot-reload, Python arrangement scripting, and follow-action sequencing.

Most DAWs bolt scripting on as an afterthought. trakdaw is built from the engine up to be scriptable. The script API is not an escape hatch — it's the primary interface.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Lua REPL (LuaJIT + sol2)                   │  ← real-time scripting, MIDI callbacks
│    daw.* API                                │
│    hot-reload (bar-boundary sync)           │
│    follow actions (on_end callbacks)        │
├─────────────────────────────────────────────┤
│  Python host (CPython 3.12 + pybind11)      │  ← arrangement scripting
│    daw Python module                        │
│    runs scripts off the audio thread        │
├─────────────────────────────────────────────┤
│  Tracktion Engine                           │  ← audio/MIDI engine
│    Edit (2 tracks × 2 clip slots)          │
│    Session-view clip launcher               │
│    Plugin chain (VST3 via JUCE)            │
├─────────────────────────────────────────────┤
│  JUCE (audio I/O via JACK / PipeWire)       │
└─────────────────────────────────────────────┘
```

## Build

### Dependencies

- CMake 3.22+
- GCC with C++20
- JUCE 8 (submodule: `vendor/JUCE`)
- Tracktion Engine (submodule: `vendor/tracktion_engine`)
- LuaJIT (`pkg-config luajit`)
- sol2 3.3.0 (submodule: `vendor/sol2`)
- Python 3.12 via pyenv (`~/.pyenv/versions/3.12.9`)
- pybind11 (installed via pip in pyenv Python)
- JACK (`pkg-config jack`) — for PipeWire audio on Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

### Running

On PipeWire systems (Linux), use `pw-jack` to connect through PipeWire's JACK bridge:

```bash
pw-jack ./build/trakdaw_artefacts/Debug/trakdaw
```

## Lua REPL API

```lua
-- Transport
daw.play()
daw.stop()
daw.playing()          -- bool
daw.position()         -- seconds
daw.bpm()
daw.set_bpm(140)

-- Clip slots (1-based)
daw.clip(track, slot).launch()
daw.clip(track, slot).stop()
daw.clip(track, slot).playing()  -- bool
daw.clip(track, slot).name       -- string

-- Plugins
daw.load_4osc(track)                          -- Tracktion built-in synth (works)
daw.load_plugin(track, "/path/to/foo.vst3")   -- external VST3 (see known issues)

-- Scripts
daw.load_script("scripts/foo.lua")   -- load + watch for hot reload
daw.run_python("scripts/foo.py")     -- run Python arrangement script

-- Follow actions
daw.on_end(track, slot, function(info) ... end)  -- callback when clip finishes
daw.trigger_follow(track, slot)                  -- manually fire callback (testing)

-- MIDI
daw.inject_midi(note, velocity [, channel])  -- synthetic note-on

-- Diagnostics
daw.audio_info()
daw.list_audio_devices()
daw.open_audio_device(name)
daw.plugin_info(track)

-- Persistent state (survives hot reload)
daw.store.mykey = value
```

## Clip Grid

```
         Slot 1        Slot 2
Track 1  Bass A        Bass B
(Bass)   16-beat C     16-beat D
         groove        groove

Track 2  Melody A      Melody B
(Melody) 8-beat C maj  8-beat A min
         pentatonic    pentatonic
```

## Example Scripts

### `scripts/drunk_walk.lua`
Probabilistic follow-action sequencer. When a clip ends, randomly picks the next slot (70% flip, 30% stay) and launches it. Chains indefinitely.

```lua
daw.load_script("scripts/drunk_walk.lua")
-- Simulate clip endings without audio:
daw.trigger_follow(1, 1)
daw.trigger_follow(2, 1)
```

### `scripts/arrange.py`
Python arrangement script — writes a 16-bar bass line and melody to the timeline, then starts playback.

```lua
daw.run_python("scripts/arrange.py")
```

### `scripts/clip_launcher.lua`
Maps MIDI notes C3/D3 to clip slot launches.

```lua
daw.load_script("scripts/clip_launcher.lua")
```

## Known Issues

### Clip looping
`daw.clip(t,s).launch()` plays the clip once and stops. `LaunchHandle::setLooping()` is called with the clip's beat span (`getEndBeat() - getStartBeat()`) but clips still don't loop continuously.

**Root cause:** Not yet identified. Likely in how session-view clips interact with Tracktion's playback graph loop rendering. The clip's beat span may differ from the sequence content length in ClipSlot context.

**Next steps to investigate:**
- Print the beat span value at launch time to verify it's non-zero
- Check if `MidiClip::getLoopLengthBeats()` is a better source
- Try calling `lh->setLooping({})` with `std::nullopt` to see if Tracktion has a default loop mode
- Look at how Tracktion's own clip launcher examples handle looping

### VST3 plugins (e.g. Surge XT)
`daw.load_plugin(track, path)` successfully scans the VST3 (`findAllTypesForFile` returns correct description with `instrument=1`), but `ExternalPlugin::getLoadError()` returns the generic "couldn't be loaded" error and the plugin produces no audio.

`daw.load_4osc(track)` with Tracktion's built-in 4OSC synth **does** work and produces audio.

**Root cause hypothesis:** `ExternalPlugin::initialiseFully()` isn't being triggered through the normal playback graph init when a plugin is inserted, and calling it directly from the message thread may conflict with the VST3 plugin's own threading requirements.

**Next steps to investigate:**
- Load the plugin *before* calling `daw.play()` — forces graph init to include the new plugin
- Check whether `engine.getPluginManager().initialise()` is required first to register VST3 format handlers
- Inspect the actual JUCE error from `AudioPluginFormatManager::createPluginInstance`

### Shutdown segfault
Ctrl+C causes a segfault due to a race between audio device teardown and Lua/Python thread cleanup. Use `quit` at the REPL instead.

## Phases Completed

| Phase | Description |
|-------|-------------|
| 1 | Engine bootstrap (JUCE + Tracktion, console app, audio init) |
| 2 | Lua REPL + `daw` API (transport, clip launch, `daw.store`) |
| 3 | MIDI input (raw JUCE `MidiInput`, lock-free SPSC queue, `on_midi` callback) |
| 4 | Hot reload (file watch, bar-boundary sync, `daw.load_script`) |
| 5 | Python arrangement layer (pybind11 embed, `daw` Python module, timeline clips) |
| 6 | Follow actions (polling `LaunchHandle::PlayState`, `daw.on_end`, drunk walk) |

## Planned

- Phase 7: HTTP/WebSocket server for browser-based control
- Clip looping fix
- VST3 plugin loading fix
- MIDI output routing

## License

Tracktion Engine is GPL3 / commercial dual-licensed. Any derivative that distributes must comply with GPL3 or obtain a commercial license from Tracktion Software.
