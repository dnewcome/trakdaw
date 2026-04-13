# trakdaw

A fully scriptable, clip-based DAW built on [Tracktion Engine](https://github.com/Tracktion/tracktion_engine).

Write Lua or Python to generate MIDI, launch clips, define follow actions, and perform live — while audio plays without interruption.

---

## What this is

Most DAWs bolt scripting on as an afterthought. Ableton and Bitwig limit you to controller/MIDI remote scripts. Reaper's ReaScript is powerful but purely track-and-timeline. Waveform Pro (the DAW built on the same engine as this project) has a scripting feature, but it's a crippled JUCE-internal JavaScript used only for keyboard macros — "major parts of a project are currently inaccessible" via it.

trakdaw is built from the engine up to be scriptable. The script API is not an escape hatch — it's the primary interface.

## What you can do with it

```lua
-- React to MIDI input in real time
function on_midi(msg)
  if msg.type == "note_on" then
    daw.scene(msg.note % 8):trigger()
  end
end

-- Define follow actions in code
function on_clip_end(clip)
  clip.slot.next_filled():launch({ quantize = "bar" })
end
```

```python
# Generate a MIDI clip from a Python script
import daw
from music21 import chord

edit = daw.current_edit()
clip = edit.track("Chords").new_midi_clip(start_bar=1, length_bars=4)
for bar, pitches in enumerate([[60,64,67], [62,65,69], [60,64,67], [57,60,64]]):
    for p in pitches:
        clip.add_note(pitch=p, start_beat=bar*4, length=3.9)
clip.commit()
```

Changes to scripts on disk take effect on the next bar boundary — no stopping playback.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Script Environment                    │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │  Lua (RT)   │  │  Python (AT) │  │  JS/QuickJS   │  │
│  │  LuaJIT     │  │  CPython     │  │  (optional)   │  │
│  └──────┬──────┘  └──────┬───────┘  └───────┬───────┘  │
│         └────────────────┴──────────────────┘           │
│                     DAW Script API                       │
│  clip.launch(), scene.trigger(), midi.send(), ...        │
└────────────────────────┬────────────────────────────────┘
                         │  lock-free message queues
┌────────────────────────┴────────────────────────────────┐
│                   C++ Binding Layer                      │
│  sol2 (Lua), pybind11 (Python), quickjs-c (JS)          │
│  Thread-safe proxy objects, callback registration        │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────┴────────────────────────────────┐
│                  Tracktion Engine                        │
│  Engine · Edit · Track · ClipSlot · Scene · AudioGraph  │
│  MidiClip · AudioClip · Plugin rack · Render engine     │
└─────────────────────────────────────────────────────────┘
                         │
              JUCE audio/MIDI I/O layer
```

**Lua (LuaJIT)** handles real-time scripting — MIDI callbacks, follow actions, clip triggering. LuaJIT's incremental GC makes it safe near the audio thread.

**Python (CPython)** handles arrangement-time scripting — generative composition, batch operations, calling external APIs or AI models. Runs in its own thread; never touches the audio callback.

## Build stack

- C++17
- JUCE 7+ (CMake)
- Tracktion Engine (JUCE module, git submodule)
- LuaJIT 2.1 + sol2 v3
- pybind11 (Python support)
- CMake 3.22+

## Status

Early design phase. See [PLAN.md](PLAN.md) for the full architecture and roadmap.

## License

Tracktion Engine is GPL3 / commercial dual-licensed. Any derivative that distributes must comply with GPL3 or obtain a commercial license from Tracktion Software.
