/*
    trakdaw — Phase 5: Python Arrangement Layer
    ---------------------------------------------
    Builds on Phase 4 (hot reload) and adds:
      - CPython embedded via pybind11
      - `daw` Python module: transport, Track, MidiClip arrangement API
      - PythonHost thread owns the interpreter; scripts queued and run serially
      - daw.run_python("path.py") from the Lua REPL triggers a Python script
      - Python arrangement writes timeline MIDI clips; Lua handles real-time
*/

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// pybind11 must come before JuceHeader to avoid macro conflicts
#include <pybind11/embed.h>
#include <pybind11/stl.h>
namespace py = pybind11;

#include <JuceHeader.h>

#include <sys/select.h>
#include <unistd.h>
#include <cmath>
#include <map>
#include <mutex>
#include <queue>

namespace te = tracktion;
using namespace tracktion::literals;

//==============================================================================
// MidiEvent — POD, safe to copy across threads
//==============================================================================
struct MidiEvent
{
    std::array<uint8_t, 4> data{};
    int    size            = 0;
    double receiveTimeSecs = 0.0;
};

//==============================================================================
// MidiEventQueue — lock-free SPSC
//   Producer: MIDI callback thread
//   Consumer: Lua REPL thread
//==============================================================================
class MidiEventQueue
{
public:
    static constexpr int kCapacity = 1024;

    bool push (const MidiEvent& e)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 == 0) return false;
        buf[s1] = e;
        fifo.finishedWrite (n1);
        return true;
    }

    bool pop (MidiEvent& e)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 == 0) return false;
        e = buf[s1];
        fifo.finishedRead (n1);
        return true;
    }

private:
    juce::AbstractFifo                   fifo { kCapacity };
    std::array<MidiEvent, kCapacity>     buf;
};

//==============================================================================
// MidiInputHandler — JUCE callback, runs on the OS MIDI thread
//==============================================================================
class MidiInputHandler : public juce::MidiInputCallback
{
public:
    explicit MidiInputHandler (MidiEventQueue& q) : queue (q) {}

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& msg) override
    {
        MidiEvent e;
        const uint8_t* raw = msg.getRawData();
        e.size = std::min ((int) msg.getRawDataSize(), 4);
        for (int i = 0; i < e.size; ++i)
            e.data[i] = raw[i];
        e.receiveTimeSecs = juce::Time::getMillisecondCounterHiRes() * 0.001;
        queue.push (e);
    }

private:
    MidiEventQueue& queue;
};

//==============================================================================
struct TrakdawBehaviour : public te::EngineBehaviour
{
    bool autoInitialiseDeviceManager() override { return false; }
};

//==============================================================================
static te::MonotonicBeat getLaunchPosition (te::Edit& edit)
{
    if (auto epc = edit.getTransport().getCurrentPlaybackContext())
        if (auto syncPoint = epc->getSyncPoint())
            return syncPoint->monotonicBeat;
    return {};
}

//==============================================================================
// Phase 5: Python arrangement layer
//==============================================================================

// Global context — set before the Python interpreter starts, read-only thereafter.
static te::Edit* g_pyEdit = nullptr;

// ---------------------------------------------------------------------------
// Proxy: a timeline MIDI clip owned by the Edit
// ---------------------------------------------------------------------------
struct PyMidiClip
{
    te::MidiClip* ptr = nullptr;   // raw pointer — valid for Edit lifetime

    void addNote (int pitch, double startBeat, double lenBeats,
                  int velocity = 100, int channel = 1)
    {
        if (!ptr) return;
        struct Args { PyMidiClip* self; int pitch; double sb, lb; int vel, ch; };
        Args args { this, pitch, startBeat, lenBeats, velocity, channel };
        py::gil_scoped_release release;
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [] (void* ctx) -> void*
            {
                auto* a  = static_cast<Args*>(ctx);
                auto* um = &g_pyEdit->getUndoManager();
                a->self->ptr->getSequence().addNote (
                    a->pitch,
                    te::BeatPosition::fromBeats (a->sb),
                    te::BeatDuration::fromBeats (a->lb),
                    a->vel, a->ch - 1, um);
                return nullptr;
            }, &args);
    }

    void clear()
    {
        if (!ptr) return;
        py::gil_scoped_release release;
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [] (void* ctx) -> void*
            {
                auto* self = static_cast<PyMidiClip*>(ctx);
                self->ptr->getSequence().clear (&g_pyEdit->getUndoManager());
                return nullptr;
            }, this);
    }
};

// ---------------------------------------------------------------------------
// Proxy: an audio track in the Edit
// ---------------------------------------------------------------------------
struct PyTrack
{
    int index = -1;   // 0-based

    std::string name() const
    {
        auto tr = te::getAudioTracks (*g_pyEdit);
        if (index < 0 || index >= tr.size()) return {};
        return tr[index]->getName().toStdString();
    }

    PyMidiClip newMidiClip (int startBar, int lengthBars)
    {
        PyMidiClip result;
        py::gil_scoped_release release;

        struct Args { int idx, sb, lb; PyMidiClip* out; };
        Args args { index, startBar, lengthBars, &result };

        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [] (void* ctx) -> void*
            {
                auto* a  = static_cast<Args*>(ctx);
                auto  tr = te::getAudioTracks (*g_pyEdit);
                if (a->idx < 0 || a->idx >= tr.size()) return nullptr;
                auto* at = tr[a->idx];
                auto* um = &g_pyEdit->getUndoManager();

                double startBeat = (a->sb - 1) * 4.0;
                double endBeat   = startBeat + a->lb * 4.0;
                auto   startTime = g_pyEdit->tempoSequence.toTime (
                                       te::BeatPosition::fromBeats (startBeat));
                auto   endTime   = g_pyEdit->tempoSequence.toTime (
                                       te::BeatPosition::fromBeats (endBeat));

                // insertMIDIClip takes a SelectionManager*, not UndoManager*
                auto clipPtr = at->insertMIDIClip (te::TimeRange (startTime, endTime), nullptr);
                a->out->ptr = clipPtr.get();
                return nullptr;
            }, &args);

        return result;
    }

    void clearClips()
    {
        py::gil_scoped_release release;
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [] (void* ctx) -> void*
            {
                auto* a  = static_cast<PyTrack*>(ctx);
                auto  tr = te::getAudioTracks (*g_pyEdit);
                if (a->index < 0 || a->index >= tr.size()) return nullptr;
                for (auto* clip : tr[a->index]->getClips())
                    clip->removeFromParent();
                return nullptr;
            }, this);
    }
};

// ---------------------------------------------------------------------------
// The embedded `daw` Python module
// Registered at static-init time; interpreter must start after g_pyEdit is set.
// ---------------------------------------------------------------------------
PYBIND11_EMBEDDED_MODULE (daw, m)
{
    m.doc() = "trakdaw Python arrangement API";

    // --- transport (fire-and-forget; Python doesn't need to block) ---
    m.def ("play",  [] { juce::MessageManager::callAsync ([] { g_pyEdit->getTransport().play (false); }); });
    m.def ("stop",  [] { juce::MessageManager::callAsync ([] { g_pyEdit->getTransport().stop (false, false); }); });
    m.def ("bpm",   [] -> double { return g_pyEdit->tempoSequence.getTempo (0)->getBpm(); });
    m.def ("set_bpm", [] (double bpm) {
        juce::MessageManager::callAsync ([bpm] { g_pyEdit->tempoSequence.getTempo (0)->setBpm (bpm); });
    });
    m.def ("position", [] -> double {
        return g_pyEdit->getTransport().getPosition().inSeconds();
    });
    m.def ("playing", [] -> bool { return g_pyEdit->getTransport().isPlaying(); });

    // --- track proxy ---
    py::class_<PyTrack> (m, "Track")
        .def_property_readonly ("name",    &PyTrack::name)
        .def ("new_midi_clip", &PyTrack::newMidiClip,
              py::arg ("start_bar") = 1, py::arg ("length_bars") = 4,
              "Create a new timeline MIDI clip (bars are 1-based, 4/4 assumed)")
        .def ("clear_clips", &PyTrack::clearClips,
              "Remove all timeline clips from this track");

    // --- midi clip proxy ---
    py::class_<PyMidiClip> (m, "MidiClip")
        .def ("add_note", &PyMidiClip::addNote,
              py::arg ("pitch"), py::arg ("start_beat"),
              py::arg ("length") = 1.0, py::arg ("velocity") = 100,
              py::arg ("channel") = 1,
              "Add a MIDI note (beats are 0-based within the clip)")
        .def ("clear", &PyMidiClip::clear, "Remove all notes from this clip");

    // --- edit access ---
    m.def ("tracks", [] {
        std::vector<PyTrack> result;
        auto tr = te::getAudioTracks (*g_pyEdit);
        for (int i = 0; i < (int) tr.size(); ++i)
            result.push_back ({ i });
        return result;
    }, "Return list of all audio tracks");

    m.def ("track", [] (py::object nameOrIdx) -> PyTrack {
        auto tr = te::getAudioTracks (*g_pyEdit);
        if (py::isinstance<py::int_> (nameOrIdx))
        {
            int idx = nameOrIdx.cast<int>() - 1;   // 1-based → 0-based
            if (idx >= 0 && idx < (int) tr.size()) return { idx };
        }
        else if (py::isinstance<py::str> (nameOrIdx))
        {
            auto name = nameOrIdx.cast<std::string>();
            for (int i = 0; i < (int) tr.size(); ++i)
                if (tr[i]->getName().toStdString() == name)
                    return { i };
        }
        throw py::index_error ("track not found: " + py::str (nameOrIdx).cast<std::string>());
    }, py::arg ("name_or_index"),
       "Get a track by 1-based index or name");
}

// ---------------------------------------------------------------------------
// PythonHost — owns the CPython interpreter, runs scripts serially
// ---------------------------------------------------------------------------
class PythonHost : private juce::Thread
{
public:
    PythonHost() : juce::Thread ("python-host") {}

    void start()    { startThread(); }
    void shutdown() { signalThreadShouldExit(); event.signal(); stopThread (5000); }

    // Thread-safe: enqueue a script path for execution
    void runScript (const std::string& path)
    {
        {
            std::lock_guard<std::mutex> lock (mtx);
            scriptQueue.push (path);
        }
        event.signal();
    }

private:
    void run() override
    {
        py::initialize_interpreter();

        // Force line-buffered stdout/stderr so print() appears immediately
        py::exec (R"(
import sys, io
if hasattr(sys.stdout, 'buffer'):
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, line_buffering=True)
if hasattr(sys.stderr, 'buffer'):
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, line_buffering=True)
)");

        while (!threadShouldExit())
        {
            event.wait (500);

            std::string path;
            {
                std::lock_guard<std::mutex> lock (mtx);
                if (!scriptQueue.empty())
                {
                    path = scriptQueue.front();
                    scriptQueue.pop();
                }
            }

            if (path.empty()) continue;

            std::cout << "[python] running: " << path << "\n";
            try
            {
                py::eval_file (path);
                py::exec ("import sys; sys.stdout.flush(); sys.stderr.flush()");
                std::cout << "[python] done: " << path << "\n> " << std::flush;
            }
            catch (py::error_already_set& e)
            {
                std::cerr << "[python error] " << e.what() << "\n> " << std::flush;
            }
        }

        py::finalize_interpreter();
    }

    std::mutex              mtx;
    std::queue<std::string> scriptQueue;
    juce::WaitableEvent     event { false };
};

//==============================================================================
// Decode a MidiEvent and call on_midi(msg) if defined. Lua-thread only.
//==============================================================================
static void dispatchMidiToLua (sol::state& lua, const MidiEvent& e)
{
    sol::object cb = lua["on_midi"];
    if (cb.get_type() != sol::type::function) return;

    const uint8_t status  = e.data[0] & 0xF0;
    const uint8_t channel = (e.data[0] & 0x0F) + 1;

    double dispatchTime = juce::Time::getMillisecondCounterHiRes() * 0.001;

    sol::table msg = lua.create_table();
    msg["channel"]       = (int) channel;
    msg["receive_time"]  = e.receiveTimeSecs;
    msg["dispatch_time"] = dispatchTime;
    msg["latency_ms"]    = (dispatchTime - e.receiveTimeSecs) * 1000.0;

    if (status == 0x90 && e.size >= 3 && e.data[2] > 0)
    {
        msg["type"]     = "note_on";
        msg["note"]     = (int) e.data[1];
        msg["velocity"] = (int) e.data[2];
    }
    else if (status == 0x80 || (status == 0x90 && e.size >= 3 && e.data[2] == 0))
    {
        msg["type"]     = "note_off";
        msg["note"]     = (int) e.data[1];
        msg["velocity"] = (int) e.data[2];
    }
    else if (status == 0xB0 && e.size >= 3)
    {
        msg["type"]  = "cc";
        msg["cc"]    = (int) e.data[1];
        msg["value"] = (int) e.data[2];
    }
    else if (status == 0xE0 && e.size >= 3)
    {
        msg["type"]  = "pitchbend";
        msg["value"] = (int) ((e.data[1] & 0x7F) | ((e.data[2] & 0x7F) << 7)) - 8192;
    }
    else
    {
        msg["type"]   = "other";
        msg["status"] = (int) e.data[0];
    }

    auto result = cb.as<sol::protected_function>()(msg);
    if (!result.valid())
    {
        sol::error err = result;
        std::cerr << "\n[on_midi error] " << err.what() << "\n> " << std::flush;
    }
}

//==============================================================================
// Register the `daw` table.
// onWatch(path) is called when load_script succeeds — lets LuaRepl set up watching.
//==============================================================================
static void registerDawApi (sol::state& lua,
                             te::Edit& edit,
                             MidiEventQueue& midiQueue,
                             std::function<void(const std::string&)> onWatch,
                             PythonHost& pythonHost)
{
    auto daw = lua.create_named_table ("daw");

    // --- transport ---

    daw.set_function ("play", [&edit]() {
        juce::MessageManager::callAsync ([&edit] { edit.getTransport().play (false); });
    });

    daw.set_function ("stop", [&edit]() {
        juce::MessageManager::callAsync ([&edit] { edit.getTransport().stop (false, false); });
    });

    daw.set_function ("position", [&edit]() -> double {
        return edit.getTransport().getPosition().inSeconds();
    });

    daw.set_function ("playing", [&edit]() -> bool {
        return edit.getTransport().isPlaying();
    });

    daw.set_function ("list_audio_devices", [&edit]() {
        auto& adm = edit.engine.getDeviceManager().deviceManager;
        for (auto* type : adm.getAvailableDeviceTypes())
        {
            std::cout << "Type: " << type->getTypeName() << "\n";
            for (auto& name : type->getDeviceNames (false))
                std::cout << "  out: " << name << "\n";
        }
    });

    // daw.load_4osc(trackIdx) — insert Tracktion's built-in 4-oscillator synth
    daw.set_function ("load_4osc", [&edit](int trackIdx) -> bool {
        struct Args { te::Edit* edit; int idx; bool ok; };
        Args args { &edit, trackIdx, false };
        juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* ctx) -> void*
        {
            auto* a      = static_cast<Args*> (ctx);
            auto  tracks = te::getAudioTracks (*a->edit);
            if (a->idx < 1 || a->idx > (int) tracks.size()) return nullptr;
            auto* at = tracks[a->idx - 1];
            juce::ValueTree vt (te::IDs::PLUGIN);
            vt.setProperty (te::IDs::type, te::FourOscPlugin::xmlTypeName, nullptr);
            at->pluginList.insertPlugin (vt, 0);
            a->ok = true;
            return nullptr;
        }, &args);
        if (args.ok)
            std::cout << "[4osc] loaded on track " << trackIdx << "\n> " << std::flush;
        return args.ok;
    });

    daw.set_function ("plugin_info", [&edit](int trackIdx) {
        struct Args { te::Edit* edit; int idx; };
        Args args { &edit, trackIdx };
        juce::MessageManager::getInstance()->callFunctionOnMessageThread ([](void* ctx) -> void*
        {
            auto* a = static_cast<Args*> (ctx);
            auto tracks = te::getAudioTracks (*a->edit);
            if (a->idx < 1 || a->idx > (int) tracks.size())
                { std::cout << "(track not found)\n"; return nullptr; }
            auto* at = tracks[a->idx - 1];
            std::cout << "Track: " << at->getName() << "\n";
            for (auto* plug : at->pluginList)
            {
                std::cout << "  Plugin: " << plug->getName() << "\n";
                if (auto* ep = dynamic_cast<te::ExternalPlugin*> (plug))
                {
                    auto err = ep->getLoadError();
                    std::cout << "    Load error: " << (err.isEmpty() ? "(none)" : err) << "\n";
                    std::cout << "    Synth:      " << (ep->isSynth() ? "yes" : "no") << "\n";
                }
            }
            return nullptr;
        }, &args);
    });

    daw.set_function ("open_audio_device", [&edit](const std::string& name) {
        auto& adm = edit.engine.getDeviceManager().deviceManager;
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        adm.getAudioDeviceSetup (setup);
        setup.outputDeviceName = name;
        setup.sampleRate       = 48000.0;
        setup.bufferSize       = 512;
        setup.outputChannels.setRange (0, 2, true);
        auto err = adm.setAudioDeviceSetup (setup, true);
        if (err.isEmpty())
            std::cout << "[audio] opened: " << name << "\n> " << std::flush;
        else
            std::cout << "[audio error] " << err << "\n> " << std::flush;
    });

    daw.set_function ("audio_info", [&edit]() {
        auto& dm  = edit.engine.getDeviceManager();
        auto* dev = dm.deviceManager.getCurrentAudioDevice();
        if (dev)
        {
            std::cout << "Audio device:  " << dev->getName() << "\n";
            std::cout << "Type:          " << dev->getTypeName() << "\n";
            std::cout << "Sample rate:   " << dev->getCurrentSampleRate() << "\n";
            std::cout << "Buffer size:   " << dev->getCurrentBufferSizeSamples() << "\n";
            std::cout << "Out channels:  " << dev->getActiveOutputChannels().countNumberOfSetBits() << "\n";
        }
        else
        {
            std::cout << "No audio device open\n";
        }
        std::cout << "Transport:     " << (edit.getTransport().isPlaying() ? "playing" : "stopped") << "\n";
        std::cout << "Position:      " << edit.getTransport().getPosition().inSeconds() << "s\n";
    });

    daw.set_function ("bpm", [&edit]() -> double {
        return edit.tempoSequence.getTempo (0)->getBpm();
    });

    daw.set_function ("set_bpm", [&edit](double bpm) {
        juce::MessageManager::callAsync ([&edit, bpm] {
            edit.tempoSequence.getTempo (0)->setBpm (bpm);
        });
    });

    // --- clips ---

    daw.set_function ("clip", [&edit](sol::this_state ts, int trackIdx, int slotIdx) -> sol::object {
        auto tracks = te::getAudioTracks (edit);
        if (trackIdx < 1 || trackIdx > tracks.size()) return sol::lua_nil;
        auto* at    = tracks[trackIdx - 1];
        auto  slots = at->getClipSlotList().getClipSlots();
        if (slotIdx < 1 || slotIdx > slots.size()) return sol::lua_nil;
        auto* clip  = slots[slotIdx - 1]->getClip();
        if (!clip) return sol::lua_nil;

        sol::state_view lv (ts);
        sol::table t = lv.create_table();
        t["name"] = clip->getName().toStdString();

        t["launch"] = [&edit, trackIdx, slotIdx]() {
            juce::MessageManager::callAsync ([&edit, trackIdx, slotIdx] {
                auto tr = te::getAudioTracks (edit);
                if (trackIdx < 1 || trackIdx > tr.size()) return;
                auto sl = tr[trackIdx - 1]->getClipSlotList().getClipSlots();
                if (slotIdx < 1 || slotIdx > sl.size()) return;
                if (auto* c = sl[slotIdx - 1]->getClip())
                    if (auto lh = c->getLaunchHandle())
                    {
                        // Prefer the clip's own loop range; fall back to the
                        // timeline length for freshly-inserted clips whose
                        // loopLengthBeats defaults to 0.
                        auto loopBeats = c->getLoopLengthBeats();
                        if (loopBeats <= te::BeatDuration{})
                            loopBeats = c->getEndBeat() - c->getStartBeat();
                        if (loopBeats > te::BeatDuration{})
                            lh->setLooping (loopBeats);
                        lh->play (getLaunchPosition (edit));
                    }
            });
        };

        t["stop"] = [&edit, trackIdx, slotIdx]() {
            juce::MessageManager::callAsync ([&edit, trackIdx, slotIdx] {
                auto tr = te::getAudioTracks (edit);
                if (trackIdx < 1 || trackIdx > tr.size()) return;
                auto sl = tr[trackIdx - 1]->getClipSlotList().getClipSlots();
                if (slotIdx < 1 || slotIdx > sl.size()) return;
                if (auto* c = sl[slotIdx - 1]->getClip())
                    if (auto lh = c->getLaunchHandle())
                        lh->stop ({});
            });
        };

        t["playing"] = [&edit, trackIdx, slotIdx]() -> bool {
            auto tr = te::getAudioTracks (edit);
            if (trackIdx < 1 || trackIdx > tr.size()) return false;
            auto sl = tr[trackIdx - 1]->getClipSlotList().getClipSlots();
            if (slotIdx < 1 || slotIdx > sl.size()) return false;
            if (auto* c = sl[slotIdx - 1]->getClip())
                if (auto lh = c->getLaunchHandle())
                    return lh->getPlayingStatus() == te::LaunchHandle::PlayState::playing;
            return false;
        };

        return t;
    });

    daw.set_function ("tracks", [&edit](sol::this_state ts) -> sol::table {
        sol::state_view lv (ts);
        sol::table t = lv.create_table();
        int i = 1;
        for (auto* at : te::getAudioTracks (edit))
            t[i++] = at->getName().toStdString();
        return t;
    });

    // --- MIDI ---

    // daw.inject_midi(note, vel [, channel=1])
    // Pushes a synthetic note-on into the MIDI queue.
    // Dispatched on the next drainMidi() call (≤10ms).
    daw.set_function ("inject_midi", [&midiQueue](int note, int vel, sol::optional<int> ch) {
        MidiEvent e;
        e.data[0]         = uint8_t (0x90 | ((ch.value_or (1) - 1) & 0x0F));
        e.data[1]         = uint8_t (note & 0x7F);
        e.data[2]         = uint8_t (vel  & 0x7F);
        e.size            = 3;
        e.receiveTimeSecs = juce::Time::getMillisecondCounterHiRes() * 0.001;
        midiQueue.push (e);
    });

    // --- scripting ---

    // daw.load_script(path) — execute a Lua file and start watching it for changes
    daw.set_function ("load_script", [&lua, onWatch](const std::string& path) -> bool {
        // Resolve to absolute so juce::File is happy when setting up the watcher
        auto absFile = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        auto result = lua.safe_script_file (absFile.getFullPathName().toStdString(),
                                            sol::script_pass_on_error);
        if (!result.valid())
        {
            sol::error err = result;
            std::cerr << "[load_script error] " << err.what() << "\n";
            return false;
        }
        onWatch (absFile.getFullPathName().toStdString());
        return true;
    });

    // --- persistent store ---

    // daw.store persists across hot reloads (the Lua state is not replaced).
    // Scripts should treat it as a simple key-value bag of primitives.
    // Pattern: daw.store.counter = (daw.store.counter or 0) + 1
    sol::object existing = daw["store"];
    if (existing.get_type() != sol::type::table)
        daw["store"] = lua.create_table();

    // --- Python bridge ---

    // daw.run_python("path.py")  — execute a Python arrangement script
    daw.set_function ("run_python", [&pythonHost](const std::string& path) {
        pythonHost.runScript (path);
    });

    // --- plugin loading ---

    // daw.load_plugin(trackIdx, path)
    // Load a VST3 plugin from `path` and insert it at the front of the
    // given track's plugin chain. trackIdx is 1-based.
    // Returns true on success, false + prints error on failure.
    daw.set_function ("load_plugin", [&edit](int trackIdx, const std::string& path) -> bool {
        struct Args { te::Edit* edit; int trackIdx; std::string path; bool ok; std::string err; };
        Args args { &edit, trackIdx, path, false, {} };

        juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* ctx) -> void*
        {
            auto* a      = static_cast<Args*> (ctx);
            auto  tracks = te::getAudioTracks (*a->edit);
            if (a->trackIdx < 1 || a->trackIdx > (int) tracks.size())
            {
                a->err = "track index out of range";
                return nullptr;
            }
            auto* at = tracks[a->trackIdx - 1];

            // Find a format that accepts this file
            auto& fmgr = a->edit->engine.getPluginManager().pluginFormatManager;
            juce::OwnedArray<juce::PluginDescription> results;
            for (auto* fmt : fmgr.getFormats())
            {
                if (fmt->fileMightContainThisPluginType (a->path))
                {
                    fmt->findAllTypesForFile (results, a->path);
                    if (! results.isEmpty()) break;
                }
            }

            if (results.isEmpty())
            {
                a->err = "no plugin found at: " + a->path;
                return nullptr;
            }

            std::cout << "[load_plugin] found " << results.size() << " plugin(s):\n";
            for (auto* d : results)
                std::cout << "  name=" << d->name
                          << "  fmt=" << d->pluginFormatName
                          << "  instrument=" << (int)d->isInstrument
                          << "  uid=" << d->uniqueId << "\n";

            // Tracktion's ExternalPlugin::findMatchingPlugin() searches
            // PluginManager::knownPluginList for a description matching the
            // inserted plugin — if it's not registered there, doFullInitialisation
            // silently bails and the plugin never loads. Register now so the
            // graph-build init path can resolve it.
            auto& pm = a->edit->engine.getPluginManager();
            pm.knownPluginList.addType (*results[0]);

            auto vt = te::ExternalPlugin::create (a->edit->engine, *results[0]);
            at->pluginList.insertPlugin (vt, 0);
            // Restart playback so Tracktion rebuilds the audio graph and initialises
            // the new plugin through the normal render path.
            bool wasPlaying = a->edit->getTransport().isPlaying();
            a->edit->getTransport().stop (false, false);
            if (wasPlaying)
                a->edit->getTransport().play (false);
            a->ok = true;
            return nullptr;
        }, &args);

        if (args.ok)
        {
            std::cout << "[load_plugin] loaded on track " << trackIdx << "\n> " << std::flush;
        }
        else
        {
            std::cerr << "[load_plugin error] " << args.err << "\n> " << std::flush;
        }
        return args.ok;
    });

    // --- follow actions ---

    // daw._follow  — internal table: "t:s" → callback function
    // Persists across hot reloads (same as daw.store).
    {
        sol::object existing = daw["_follow"];
        if (existing.get_type() != sol::type::table)
            daw["_follow"] = lua.create_table();
    }

    // daw.on_end(track, slot, fn)
    // Register fn to be called when clip at (track, slot) finishes playing.
    // fn receives: { track, slot, name }
    // Set fn = nil to deregister.
    daw.set_function ("on_end", [](sol::this_state ts, int t, int s, sol::object fn) {
        sol::state_view lv (ts);
        sol::table follow = lv["daw"]["_follow"];
        follow[std::to_string (t) + ":" + std::to_string (s)] = fn;
    });

    // daw.trigger_follow(track, slot)
    // Manually fire the on_end callback for a slot. Useful for testing without
    // a running audio engine (no real clip playback required).
    daw.set_function ("trigger_follow", [](sol::this_state ts, int t, int s) {
        sol::state_view lv (ts);
        sol::table follow = lv["daw"]["_follow"];
        sol::object fn = follow[std::to_string (t) + ":" + std::to_string (s)];
        if (fn.get_type() != sol::type::function) return;

        sol::table info = lv.create_table();
        info["track"] = t;
        info["slot"]  = s;
        info["name"]  = "(manually triggered)";

        auto result = fn.as<sol::protected_function>() (info);
        if (!result.valid())
        {
            sol::error err = result;
            std::cerr << "\n[trigger_follow error] " << err.what() << "\n> " << std::flush;
        }
    });
}

//==============================================================================
// LuaRepl
//==============================================================================
class LuaRepl : private juce::Thread
{
public:
    LuaRepl (te::Edit& edit, MidiEventQueue& midiQueue, PythonHost& pythonHost)
        : juce::Thread ("lua-repl"), edit (edit), midiQueue (midiQueue)
    {
        lua.open_libraries (sol::lib::base, sol::lib::string,
                            sol::lib::table,  sol::lib::math,
                            sol::lib::io,     sol::lib::os);

        registerDawApi (lua, edit, midiQueue,
            [this](const std::string& path) {
                watch.path  = path;
                watch.mtime = juce::File (path).getLastModificationTime();
                watch.pending = false;
                std::cout << "[watch] monitoring: "
                          << juce::File (path).getFileName() << "\n> " << std::flush;
            },
            pythonHost);
    }

    void start()    { startThread(); }
    void shutdown() { signalThreadShouldExit(); stopThread (2000); }

private:
    // -----------------------------------------------------------------------
    // Evaluate one line (expression first, then statement)
    // -----------------------------------------------------------------------
    void evalLine (const std::string& line)
    {
        auto result = lua.safe_script ("return " + line, sol::script_pass_on_error);
        if (!result.valid())
            result = lua.safe_script (line, sol::script_pass_on_error);

        if (!result.valid())
        {
            sol::error err = result;
            std::cout << "error: " << err.what() << "\n";
            return;
        }

        for (int i = 0; i < (int) result.return_count(); ++i)
            printValue (result[i]);
    }

    static void printValue (sol::object obj)
    {
        switch (obj.get_type())
        {
            case sol::type::nil:     break;
            case sol::type::boolean: std::cout << (obj.as<bool>() ? "true" : "false") << "\n"; break;
            case sol::type::number:  std::cout << obj.as<double>()      << "\n"; break;
            case sol::type::string:  std::cout << obj.as<std::string>() << "\n"; break;
            case sol::type::table:
            {
                sol::table t = obj.as<sol::table>();
                std::cout << "{\n";
                t.for_each ([](sol::object k, sol::object v) {
                    std::cout << "  [";
                    if (k.get_type() == sol::type::string) std::cout << k.as<std::string>();
                    else                                   std::cout << k.as<double>();
                    std::cout << "] = ";
                    if      (v.get_type() == sol::type::string)  std::cout << v.as<std::string>();
                    else if (v.get_type() == sol::type::number)  std::cout << v.as<double>();
                    else if (v.get_type() == sol::type::boolean) std::cout << (v.as<bool>() ? "true" : "false");
                    else                                         std::cout << "(function/table)";
                    std::cout << "\n";
                });
                std::cout << "}\n";
                break;
            }
            default: std::cout << "(value)\n"; break;
        }
    }

    // -----------------------------------------------------------------------
    // MIDI dispatch
    // -----------------------------------------------------------------------
    void drainMidi()
    {
        MidiEvent e;
        while (midiQueue.pop (e))
            dispatchMidiToLua (lua, e);
    }

    // -----------------------------------------------------------------------
    // Hot reload
    // -----------------------------------------------------------------------

    // True if we are at or within 20ms of a 4/4 bar boundary, or if stopped.
    bool isAtBarBoundary() const
    {
        if (!edit.getTransport().isPlaying()) return true;

        const double secs      = edit.getTransport().getPosition().inSeconds();
        const double bpm       = edit.tempoSequence.getTempo (0)->getBpm();
        const double beat      = secs * bpm / 60.0;
        const double barPos    = std::fmod (beat, 4.0);    // position within bar [0,4)
        const double threshold = 0.020 * (bpm / 60.0);    // 20ms in beats

        return barPos < threshold || barPos > 4.0 - threshold;
    }

    // Check file mtime every 500ms (every 50 × 10ms poll cycles).
    void checkFileForChanges()
    {
        if (watch.path.empty()) return;
        if (++watch.pollCount % 50 != 0) return;

        auto newMtime = juce::File (watch.path).getLastModificationTime();
        if (newMtime != watch.mtime)
        {
            watch.mtime   = newMtime;
            watch.pending = true;
            std::cout << "\n[hot-reload] change detected — waiting for bar boundary...\n> "
                      << std::flush;
        }
    }

    // Fire the reload when the timing is right.
    void reloadIfReady()
    {
        if (!watch.pending)        return;
        if (!isAtBarBoundary())    return;

        watch.pending = false;

        auto result = lua.safe_script_file (watch.path, sol::script_pass_on_error);
        if (result.valid())
        {
            std::cout << "[hot-reload] reloaded: "
                      << juce::File (watch.path).getFileName() << "\n> " << std::flush;
        }
        else
        {
            sol::error err = result;
            std::cout << "[hot-reload] error: " << err.what() << "\n> " << std::flush;
        }
    }

    // -----------------------------------------------------------------------
    // REPL loop
    // -----------------------------------------------------------------------
    void run() override
    {
        std::cout << "\ntrakdaw Lua REPL (Phase 4 — hot reload)\n";
        std::cout << "  daw.play() / daw.stop()              transport\n";
        std::cout << "  daw.clip(t,s).launch()               launch clip (1-based)\n";
        std::cout << "  daw.inject_midi(note, vel)           fake MIDI note-on\n";
        std::cout << "  daw.load_script(\"path.lua\")          load + watch for changes\n";
        std::cout << "  daw.store.x = val                    persistent across reloads\n";
        std::cout << "  function on_midi(m) ... end          MIDI callback\n";
        std::cout << "  quit / exit\n\n";

        std::cout << "> " << std::flush;

        std::string line;

        while (!threadShouldExit())
        {
            fd_set fds;
            FD_ZERO (&fds);
            FD_SET (STDIN_FILENO, &fds);
            struct timeval tv { 0, 10'000 };   // 10ms

            int ready = ::select (STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);

            // select() returns -1 with EINTR when a signal arrives (e.g.
            // SIGINT); just loop — JUCE's installed SIGINT handler will
            // request app quit on its next event-loop iteration.
            if (ready < 0 && errno == EINTR)
                continue;

            if (ready > 0)
            {
                if (!std::getline (std::cin, line))
                    break;

                if (line == "quit" || line == "exit")
                {
                    juce::MessageManager::callAsync ([] {
                        juce::JUCEApplicationBase::getInstance()->quit();
                    });
                    break;
                }

                if (!line.empty())
                    evalLine (line);

                std::cout << "> " << std::flush;
            }

            drainMidi();
            checkFileForChanges();
            reloadIfReady();
            checkFollowActions();
        }
    }

    // -----------------------------------------------------------------------
    // Follow action state per tracked clip slot
    // -----------------------------------------------------------------------
    struct FollowState
    {
        te::LaunchHandle::PlayState prevState = te::LaunchHandle::PlayState::stopped;
        bool everPlayed = false;
    };

    // Check all slots that have registered on_end callbacks.
    // Called every poll cycle (~10ms). Fires callback on playing→stopped transition.
    void checkFollowActions()
    {
        sol::object followObj = lua["daw"]["_follow"];
        if (followObj.get_type() != sol::type::table) return;
        sol::table follow = followObj.as<sol::table>();

        auto tracks = te::getAudioTracks (edit);

        follow.for_each ([&](sol::object key, sol::object val)
        {
            if (key.get_type() != sol::type::string) return;
            if (val.get_type() != sol::type::function) return;

            const std::string keyStr = key.as<std::string>();
            const auto colon = keyStr.find (':');
            if (colon == std::string::npos) return;
            int t = std::stoi (keyStr.substr (0, colon));
            int s = std::stoi (keyStr.substr (colon + 1));

            if (t < 1 || t > tracks.size()) return;
            auto slots = tracks[t - 1]->getClipSlotList().getClipSlots();
            if (s < 1 || s > slots.size()) return;
            auto* clip = slots[s - 1]->getClip();
            if (!clip) return;
            auto lh = clip->getLaunchHandle();
            if (!lh) return;

            auto& st  = followStates[{ t, s }];
            auto  cur = lh->getPlayingStatus();

            if (cur == te::LaunchHandle::PlayState::playing)
                st.everPlayed = true;

            if (st.everPlayed
                && st.prevState == te::LaunchHandle::PlayState::playing
                && cur          == te::LaunchHandle::PlayState::stopped)
            {
                // Clip just finished naturally — fire the callback
                st.everPlayed = false;

                sol::table info = lua.create_table();
                info["track"] = t;
                info["slot"]  = s;
                info["name"]  = clip->getName().toStdString();

                auto result = val.as<sol::protected_function>() (info);
                if (!result.valid())
                {
                    sol::error err = result;
                    std::cerr << "\n[on_end error] " << err.what() << "\n> " << std::flush;
                }
            }

            st.prevState = cur;
        });
    }

    // -----------------------------------------------------------------------
    struct WatchState
    {
        std::string path;
        juce::Time  mtime;
        bool        pending   = false;
        int         pollCount = 0;
    };

    te::Edit&       edit;
    MidiEventQueue& midiQueue;
    sol::state      lua;
    WatchState      watch;
    std::map<std::pair<int,int>, FollowState> followStates;
};

//==============================================================================
class TrakdawApp : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName()    override { return "trakdaw"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed()          override { return true; }

    void initialise (const juce::String&) override
    {
        // 1. Engine
        std::cout << "[trakdaw] Creating engine...\n";
        engine = std::make_unique<te::Engine> ("trakdaw",
                                               std::make_unique<te::UIBehaviour>(),
                                               std::make_unique<TrakdawBehaviour>());

        // 2. Devices
        auto& dm = engine->getDeviceManager();
        dm.initialise (0, 2);

        // Auto-open the first available output device (skips MIDI-only devices)
        {
            auto& adm = dm.deviceManager;
            if (adm.getCurrentAudioDevice() == nullptr)
            {
                for (auto* type : adm.getAvailableDeviceTypes())
                {
                    auto names = type->getDeviceNames (false);
                    for (auto& name : names)
                    {
                        if (name.containsIgnoreCase ("midi")) continue;
                        adm.setCurrentAudioDeviceType (type->getTypeName(), true);
                        juce::AudioDeviceManager::AudioDeviceSetup setup;
                        adm.getAudioDeviceSetup (setup);
                        setup.outputDeviceName = name;
                        setup.sampleRate       = 48000.0;
                        setup.bufferSize       = 512;
                        setup.outputChannels.setRange (0, 2, true);
                        auto err = adm.setAudioDeviceSetup (setup, true);
                        if (err.isEmpty())
                        {
                            std::cout << "[trakdaw] Audio device: " << name << "\n";
                            goto deviceOpened;
                        }
                    }
                }
                std::cout << "[trakdaw] WARNING: no audio output device could be opened\n";
                deviceOpened:;
            }
        }

        std::cout << "[trakdaw] Device manager initialised\n";

        // 3. MIDI inputs
        midiHandler = std::make_unique<MidiInputHandler> (midiQueue);
        for (auto& info : juce::MidiInput::getAvailableDevices())
        {
            auto mi = juce::MidiInput::openDevice (info.identifier, midiHandler.get());
            if (mi)
            {
                mi->start();
                std::cout << "[trakdaw] MIDI in: " << info.name << "\n";
                midiInputs.push_back (std::move (mi));
            }
        }
        if (midiInputs.empty())
            std::cout << "[trakdaw] No MIDI input devices found\n";

        // 4. Edit
        editFile = juce::File::createTempFile (".tracktionedit");
        edit = te::createEmptyEdit (*engine, editFile);
        edit->tempoSequence.getTempo (0)->setBpm (120.0);
        std::cout << "[trakdaw] Edit created @ "
                  << edit->tempoSequence.getTempo (0)->getBpm() << " BPM\n";

        // 5. Clip grid
        edit->ensureNumberOfAudioTracks (2);
        edit->getSceneList().ensureNumberOfScenes (2);
        auto tracks = te::getAudioTracks (*edit);
        for (auto* at : tracks)
            at->getClipSlotList().ensureNumberOfSlots (2);

        auto* um = &edit->getUndoManager();

        {
            auto* at = tracks[0];
            at->setName ("Bass");
            auto mc = te::insertMIDIClip (
                *at->getClipSlotList().getClipSlots()[0],
                edit->tempoSequence.toTime ({ 0_bp, 16_bp }));
            mc->setName ("Bass A");
            auto& seq = mc->getSequence();
            seq.addNote (36, 0_bp,  4_bd, 100, 0, um);
            seq.addNote (36, 4_bp,  4_bd, 80,  0, um);
            seq.addNote (43, 8_bp,  4_bd, 90,  0, um);
            seq.addNote (41, 12_bp, 4_bd, 85,  0, um);
        }
        {
            auto* at = tracks[1];
            at->setName ("Melody");
            auto mc = te::insertMIDIClip (
                *at->getClipSlotList().getClipSlots()[0],
                edit->tempoSequence.toTime ({ 0_bp, 8_bp }));
            mc->setName ("Melody A");
            auto& seq = mc->getSequence();
            seq.addNote (60, 0_bp, 2_bd, 90, 0, um);
            seq.addNote (62, 2_bp, 2_bd, 85, 0, um);
            seq.addNote (64, 4_bp, 2_bd, 90, 0, um);
            seq.addNote (67, 6_bp, 2_bd, 80, 0, um);
        }

        // Slot 2: alternate variations (Bass B, Melody B)
        {
            // Bass B — root + fifth walk, shifted up a whole tone (D)
            auto* at = tracks[0];
            auto mc = te::insertMIDIClip (
                *at->getClipSlotList().getClipSlots()[1],
                edit->tempoSequence.toTime ({ 0_bp, 16_bp }));
            mc->setName ("Bass B");
            auto& seq = mc->getSequence();
            seq.addNote (38, 0_bp,  4_bd, 100, 0, um);   // D2
            seq.addNote (38, 4_bp,  4_bd, 80,  0, um);
            seq.addNote (45, 8_bp,  4_bd, 90,  0, um);   // A2
            seq.addNote (43, 12_bp, 4_bd, 85,  0, um);   // G2
        }
        {
            // Melody B — minor pentatonic phrase (A minor)
            auto* at = tracks[1];
            auto mc = te::insertMIDIClip (
                *at->getClipSlotList().getClipSlots()[1],
                edit->tempoSequence.toTime ({ 0_bp, 8_bp }));
            mc->setName ("Melody B");
            auto& seq = mc->getSequence();
            seq.addNote (69, 0_bp, 2_bd, 90, 0, um);  // A4
            seq.addNote (67, 2_bp, 2_bd, 85, 0, um);  // G4
            seq.addNote (65, 4_bp, 2_bd, 85, 0, um);  // F4
            seq.addNote (62, 6_bp, 2_bd, 80, 0, um);  // D4
        }

        std::cout << "[trakdaw] Clip grid ready:\n";
        for (auto* at : te::getAudioTracks (*edit))
        {
            std::cout << "  [" << at->getName() << "]\n";
            for (auto* slot : at->getClipSlotList().getClipSlots())
            {
                if (auto* clip = slot->getClip())
                    std::cout << "    " << clip->getName() << "\n";
                else
                    std::cout << "    (empty)\n";
            }
        }

        // 6. Python host — set global context then start interpreter thread
        g_pyEdit = edit.get();
        pythonHost = std::make_unique<PythonHost>();
        pythonHost->start();
        std::cout << "[trakdaw] Python host started\n";

        // 7. Lua REPL
        repl = std::make_unique<LuaRepl> (*edit, midiQueue, *pythonHost);
        repl->start();
    }

    void shutdown() override
    {
        if (repl)        { repl->shutdown();        repl.reset(); }
        if (pythonHost)  { pythonHost->shutdown();  pythonHost.reset(); }
        g_pyEdit = nullptr;

        for (auto& mi : midiInputs) mi->stop();
        midiInputs.clear();
        midiHandler.reset();

        if (edit) { edit->getTransport().stop (false, false); edit.reset(); }
        if (engine)
        {
            engine->getTemporaryFileManager().getTempDirectory().deleteRecursively();
            engine.reset();
        }
    }

    void systemRequestedQuit()                         override { quit(); }
    void anotherInstanceStarted (const juce::String&)  override {}
    void suspended()                                   override {}
    void resumed()                                     override {}
    void unhandledException (const std::exception*,
                             const juce::String&, int) override {}

private:
    std::unique_ptr<te::Engine>                   engine;
    std::unique_ptr<te::Edit>                     edit;
    juce::File                                    editFile;
    MidiEventQueue                                midiQueue;
    std::unique_ptr<MidiInputHandler>             midiHandler;
    std::vector<std::unique_ptr<juce::MidiInput>> midiInputs;
    std::unique_ptr<PythonHost>                   pythonHost;
    std::unique_ptr<LuaRepl>                      repl;
};

//==============================================================================
START_JUCE_APPLICATION (TrakdawApp)
