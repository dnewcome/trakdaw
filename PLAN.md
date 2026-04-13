# trakdaw — Scriptable Clip-Based DAW on Tracktion Engine

## The Bet

Most DAWs offer scripting as an afterthought — Ableton and Bitwig restrict you to controller/MIDI remote scripts, Reaper's ReaScript is powerful but operates on a track/timeline model. Nothing gives you a **first-class scripting API over a clip-launcher architecture** where you can generate music, automate arrangements, and process MIDI in real time from a script.

Tracktion Engine (the open-source SDK behind Waveform Pro) already has:
- A full clip-launcher/session-view model (`ClipSlot`, `Scene`, `LaunchHandle`, `SceneList`) added in v3
- A timeline arrangement model (`Edit`, `Track`, `AudioClip`, `MidiClip`)
- A composable audio graph layer
- 115k lines of mature C++ DAW infrastructure

What it does **not** have is any scripting layer. That's what we build.

### Why not just script Waveform Pro?

Waveform Pro (Tracktion's own DAW built on this engine) does have a scripting feature — but it's not what you want:

- Uses **JUCE's internal JavaScript engine**, a crippled subset missing basic ES1 features
- Scoped to **keyboard macros and controller mappings only**
- "Major parts of a project are currently inaccessible" via their scripting API (their words)
- No programmatic access to clips, tracks, or the session view
- No real Python, Lua, or full JS — just a macro language that happens to look like JS

This is the same category as Ableton's controller scripts: you can wire up a MIDI controller or trigger menu actions, but you cannot generate clips, drive the clip launcher from code, or write follow actions as real programs. Building on the SDK gives you the entire object model as a first-class API rather than a narrow escape hatch.

---

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

---

## The Real-Time / Arrangement-Time Split

This is the most important architectural decision. "Scriptable" means different things on different threads:

### Arrangement-Time Scripting (AT)
Runs outside the audio thread. Safe for GC, allocations, blocking calls.
- Generate MIDI clips programmatically
- Create/delete/move clips on the timeline or in clip slots
- Set automation curves, parameter values, tempo maps
- Batch operations: transpose all clips in a scene, shuffle grid, generative composition
- Call external APIs, databases, AI models

**Any language works here.** Python has the richest ecosystem (music21, mido, numpy for generative music) and is the best choice.

### Real-Time Scripting (RT)
Runs on or in tight coordination with the audio thread. Must be deterministic — GC pauses are fatal.
- Respond to MIDI input with zero-copy logic
- Live clip-trigger decisions (e.g., "launch next clip when this MIDI note hits")
- Custom follow-action logic
- Parameter modulation synchronized to the audio clock

**Only Lua (LuaJIT) is viable here.** Its garbage collector is incremental and controllable; LuaJIT's JIT output is fast enough for audio-rate logic. Python's GIL makes it a non-starter on the audio thread. The pattern: Lua scripts run in the message-delivery path, not the DSP path. A lock-free ring buffer passes events from the audio callback to the Lua thread.

---

## Language Evaluation

| Language | RT viable | Ecosystem | Embed cost | Recommended use |
|----------|-----------|-----------|------------|-----------------|
| **LuaJIT** | Yes | Medium | Low (sol2) | Real-time event handling, follow actions |
| **Python** | No | Best | Medium (pybind11) | Arrangement generation, generative composition |
| **QuickJS** | Marginal | Good | Low | Optional: web-dev-friendly AT scripting |
| V8 | No | Best | Very high | Not worth it |

**Start with Lua only.** It covers both paths (RT with care, AT fine). Add Python as a second tier once the architecture is proven.

---

## What Tracktion Gives You vs. What You Build

### Tracktion Engine provides:
- `Edit` — the document: all tracks, clips, tempo, master plugins
- `AudioTrack` — a track with a clip slot list and timeline clips
- `ClipSlot` — a cell in the session grid; holds a clip or is empty
- `Scene` / `SceneList` — a row in the session grid; launching a scene fires all slots in that row
- `LaunchHandle` — returned when you launch a clip; lets you query state and stop it
- `MidiClip` — MIDI sequence with note list, quantization, looping
- `AudioClip` — reference to audio file with stretch/pitch
- `EditClip` — nested Edit inside a clip slot (deep recursion possible)
- Transport, tempo map, beat-quantized playback
- Full plugin (VST/AU) hosting via plugin rack
- Offline rendering to audio file

### You build:
- **Script API surface** — the `daw.*` namespace exposed to scripts
- **C++ binding layer** — thread-safe wrappers around Tracktion objects
- **Script host / REPL** — console where scripts are loaded, reloaded hot
- **Script-triggered clip launcher** — `clip.launch("A3")`, `scene.trigger(2)`
- **MIDI event scripting bridge** — route MIDI input to Lua callbacks
- **Live coding support** — file watcher + hot reload without stopping playback
- **Script-defined follow actions** — override Tracktion's built-in follow action logic
- **Arrangement generator API** — Python API to build Edits from scratch or transform them

---

## DAW Script API (proposed surface)

```lua
-- Lua: real-time follow action
function on_clip_end(clip)
  local next = clip.slot.next_filled()
  if next then
    next:launch({ quantize = "bar" })
  end
end

-- Lua: react to MIDI input
function on_midi(msg)
  if msg.type == "note_on" and msg.note == 60 then
    daw.scene(1):trigger()
  end
end
```

```python
# Python: generative arrangement
import daw
from music21 import stream, note

edit = daw.current_edit()
track = edit.track("Melody")
clip = track.new_midi_clip(start_bar=1, length_bars=4)
for pitch in [60, 62, 64, 65, 67]:
    clip.add_note(pitch=pitch, start_beat=0, length=0.5)
clip.commit()
```

---

## Phases

### Phase 1 — Engine bootstrap
- Minimal JUCE app that creates an Engine, loads an Edit, plays back audio
- No UI (headless or minimal JUCE Component)
- Confirm clip launcher APIs work: create ClipSlots, launch a clip, query LaunchHandle state
- Output: a working Tracktion host binary

### Phase 2 — Lua embedding
- Embed LuaJIT via sol2
- Expose a minimal `daw` Lua table: `daw.play()`, `daw.stop()`, `daw.clip(track, slot):launch()`
- REPL loop: read Lua from stdin, execute, print result
- Output: type commands, hear audio respond

### Phase 3 — MIDI scripting bridge
- Route MIDI input events to Lua `on_midi(msg)` callback via lock-free queue
- Benchmark: measure latency from MIDI event to Lua callback execution
- Output: play a MIDI keyboard, Lua script launches clips in response

### Phase 4 — Hot reload / live coding
- File watcher on script directory
- On change: reload script in-place without stopping playback
- Script sandbox: each reload gets a fresh Lua state; persistent state managed via `daw.store`
- Output: edit a `.lua` file, changes take effect on next bar boundary

### Phase 5 — Python arrangement layer
- Embed CPython via pybind11
- Expose `daw` Python module mirroring Lua API but richer (generators, iterators)
- Python runs in its own thread, communicates to audio thread via the same message queue
- Output: Python script generates a 16-bar MIDI arrangement, plays back immediately

### Phase 6 — Script-defined follow actions
- Override Tracktion's built-in follow action with a Lua function per clip slot
- Lua function receives clip state, returns next action
- Output: Lua-defined "drunk walk" through clip grid — each clip randomly picks a neighbor

### Phase 7 — Developer UX
- HTTP server in the app: POST `/eval` to run a script chunk
- WebSocket: subscribe to DAW events (clip started, beat tick, MIDI in)
- Optional: embed a code editor (Monaco via WebView, or just lean on external editors + the HTTP API)

---

## Prior Art

| Project | What it does right | What's missing |
|---------|-------------------|----------------|
| Reaper / ReaScript | Lua + Python + EEL over full DAW | No clip launcher, no live-coding story |
| Sonic Pi | Live coding with hot reload | Not a real DAW, no MIDI/audio clips |
| SuperCollider | Real-time DSP scripting | Steep learning curve, not clip-based |
| Max/MSP | Dataflow scripting | Expensive, no arrangement model |
| Bitwig controller API | MIDI scripting | No arrangement access, no clip generation |

**trakdaw** sits in the empty cell: clip-launcher + arrangement + real-time MIDI scripting + hot reload.

---

## Risks

- **Tracktion Engine licensing**: GPL3 means you must open-source derivatives, or pay for a commercial license. For a personal/research tool, GPL is fine. For a product, budget the commercial license.
- **JUCE dependency**: Tracktion is a JUCE module. JUCE is large. The build system is CMake + JUCE CMake API — non-trivial to set up.
- **Clip launcher maturity**: The session-view APIs in Tracktion Engine are newer than the timeline APIs. Expect rough edges; budget time to read source code.
- **Lua ↔ C++ thread safety**: sol2 is not thread-safe by default. All Lua-to-engine calls must go through a message queue or run on the message thread with a mutex. Getting this wrong causes subtle crashes.
- **Python GIL**: If Python runs in a thread, the GIL means only one Python operation at a time. Fine for arrangement scripting; never put Python near the audio callback.

---

## Build Stack

- **C++17** (Tracktion Engine requirement)
- **JUCE 7+** (CMake)
- **Tracktion Engine** (JUCE module, submodule)
- **LuaJIT 2.1** + **sol2 v3** (Lua binding)
- **pybind11** (Python binding, Phase 5+)
- **CMake 3.22+**
- Platform: macOS first (CoreAudio), Linux second (ALSA/JACK), Windows optional

---

## Open Questions

1. Should the script API be **synchronous** (script calls block until engine responds) or **fully async** (script calls return futures/promises)? Sync is simpler to start; async is more correct for real-time use.
2. Should Lua scripts run in the **audio thread** (dangerous but lowest latency) or a **dedicated script thread** (safer, ~1ms extra latency)? Answer: dedicated thread, always.
3. Is there value in a **visual grid UI** (like Ableton's session view) or should this be **headless-first**, designed to be driven entirely by scripts? Headless-first keeps scope tight.
4. Should follow actions be **per-clip** Lua functions or a **global event loop**? Per-clip is more composable.
