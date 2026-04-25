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
daw.position_beats()   -- total beats since start (fractional)
daw.position_bars()    -- { bar=N, beat=N.f, numerator=N }  -- 1-based for musical display
daw.bpm()
daw.set_bpm(140)

-- Clip slots (1-based)
daw.clip(track, slot).launch()
daw.clip(track, slot).stop()
daw.clip(track, slot).playing()  -- bool
daw.clip(track, slot).name       -- string

-- Plugins
daw.load_4osc(track)                          -- Tracktion built-in synth
daw.load_plugin(track, "/path/to/foo.vst3")   -- external VST3 (verified with Surge XT)
daw.save_patch(track, "patches/my.xml")       -- save full plugin chain on track
daw.load_patch(track, "patches/my.xml")       -- restore plugin chain (replaces existing)
daw.list_params(track [, filter [, max=50]])  -- print id, name, value, range; filter is substring
daw.get_param(track, "filter1.cutoff")        -- → number, or nil if not found
daw.set_param(track, "filter1.cutoff", 0.7)   -- → bool

-- Scripts
daw.load_script("scripts/foo.lua")   -- load + watch for hot reload
daw.run_python("scripts/foo.py")     -- run Python arrangement script

-- Follow actions
daw.on_end(track, slot, function(info) ... end)  -- callback when clip finishes
daw.trigger_follow(track, slot)                  -- manually fire callback (testing)

-- MIDI
daw.inject_midi(note, velocity [, channel])    -- fires Lua on_midi callback
daw.note_on(track, note, velocity [, channel]) -- play instrument on track
daw.note_off(track, note [, channel])          -- stop instrument note

-- MIDI output to external devices
daw.list_midi_outputs()
daw.open_midi_output(name)
daw.send_midi(note, velocity [, channel])      -- vel 0 → note-off
daw.send_midi_raw(b1 [, b2 [, b3]])            -- CC / PC / arbitrary bytes

-- Plugin editor windows (useful for debugging a headless session)
daw.show_editor(track)   -- open native GUI for first VST on this track
daw.close_editor(track)

-- Scheduler (beat-based, fires on the REPL poll thread)
daw.after(beats, fn)             -- one-shot timer
daw.every(beats, fn)             -- repeating timer; fn returns false to stop
daw.cancel(id)                   -- cancel by id returned from after/every
daw.cancel_all()                 -- cancel everything pending

-- Remote eval + event stream (HTTP server on 127.0.0.1:8081)
--   http://127.0.0.1:8081/                               -- built-in web UI
--   curl -d 'daw.bpm()' http://127.0.0.1:8081/eval       -- run Lua, get result
--   curl -N http://127.0.0.1:8081/events                 -- subscribe to events
daw.emit(name [, table])        -- broadcast JSON to all /events subscribers
daw.auto_emit(bool)             -- master switch for engine-published events
daw.auto_emit_midi_in(bool)     -- gate the (chatty) midi_in stream; off by default
-- Auto-emitted: transport, clip_launch, clip_stop, follow, script_load, midi_in

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

## Phases Completed

| Phase | Description |
|-------|-------------|
| 1 | Engine bootstrap (JUCE + Tracktion, console app, audio init) |
| 2 | Lua REPL + `daw` API (transport, clip launch, `daw.store`) |
| 3 | MIDI input (raw JUCE `MidiInput`, lock-free SPSC queue, `on_midi` callback) |
| 4 | Hot reload (file watch, bar-boundary sync, `daw.load_script`) |
| 5 | Python arrangement layer (pybind11 embed, `daw` Python module, timeline clips) |
| 6 | Follow actions (polling `LaunchHandle::PlayState`, `daw.on_end`, drunk walk) |
| 7 | HTTP `/eval` + SSE `/events`, web UI with live clip grid, MIDI output, native MIDI input routing, VST plugin editor windows, beat-based scheduler, patch save/load, parameter API |

## Planned

- WebSocket transport for `/eval` (currently HTTP POST; SSE handles push)
- Plugin editor support for Tracktion built-ins (currently `show_editor` is VST-only)
- `.vstpreset` export for cross-DAW patch interop

## License

Tracktion Engine is GPL3 / commercial dual-licensed. Any derivative that distributes must comply with GPL3 or obtain a commercial license from Tracktion Software.
