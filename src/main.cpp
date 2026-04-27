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
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>

// cpp-httplib must be included after JuceHeader so its socket symbols don't
// collide with anything JUCE pulls in via X11/Cocoa headers.
#include <httplib.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "web_ui.h"

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
                std::cout << "[python] done: " << path << "\n" << std::flush;
            }
            catch (py::error_already_set& e)
            {
                std::cerr << "[python error] " << e.what() << "\n" << std::flush;
            }
        }

        py::finalize_interpreter();
    }

    std::mutex              mtx;
    std::queue<std::string> scriptQueue;
    juce::WaitableEvent     event { false };
};

//==============================================================================
// EventBroker — fan-out of script-emitted events to SSE subscribers.
// See PLAN.md ("Control surface design") for the design rationale:
// scripts opt into broadcasting via daw.emit(name, table); external clients
// subscribe via GET /events (Server-Sent Events) and render whatever they
// like. Keeps the engine free of state-schema baggage.
//==============================================================================
class EventBroker
{
public:
    struct Subscriber
    {
        std::mutex              mu;
        std::condition_variable cv;
        std::deque<std::string> queue;
        bool                    disconnected = false;
        static constexpr size_t kMaxQueue    = 1024;
    };

    std::shared_ptr<Subscriber> subscribe()
    {
        auto s = std::make_shared<Subscriber>();
        std::lock_guard<std::mutex> lock (mu);
        subs.push_back (s);
        return s;
    }

    void emit (std::string data)
    {
        std::lock_guard<std::mutex> lock (mu);
        subs.erase (std::remove_if (subs.begin(), subs.end(),
                                    [](auto& w) { return w.expired(); }),
                    subs.end());
        for (auto& w : subs)
        {
            if (auto s = w.lock())
            {
                std::lock_guard<std::mutex> sl (s->mu);
                if (s->queue.size() < Subscriber::kMaxQueue)
                    s->queue.push_back (data);
                // drop on slow clients rather than OOM
                s->cv.notify_one();
            }
        }
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock (mu);
        for (auto& w : subs)
            if (auto s = w.lock())
            {
                std::lock_guard<std::mutex> sl (s->mu);
                s->disconnected = true;
                s->cv.notify_all();
            }
    }

    // Auto-emit: the engine itself publishes built-in events (transport,
    // clip_launch, follow, script_load, midi_in, osc_in). Scripts toggle
    // with daw.auto_emit(bool) / daw.auto_emit_midi_in(bool) /
    // daw.auto_emit_osc_in(bool).
    //
    // midi_in is off by default because MIDI clock alone is 96 msgs/beat;
    // turning it on by default would flood the event log on most setups.
    // OSC defaults on — typical OSC sources (Tidal, hardware controllers)
    // emit a few messages per cycle, well within the log's capacity.
    std::atomic<bool> autoEmit{true};
    std::atomic<bool> autoEmitMidiIn{false};
    std::atomic<bool> autoEmitOscIn{true};

private:
    std::mutex                              mu;
    std::vector<std::weak_ptr<Subscriber>>  subs;
};

//==============================================================================
// OscBridge — owns a juce::OSCReceiver, queues incoming messages for
// the REPL thread to drain. Uses RealtimeCallback so we don't compete
// for the message thread; queueing is mutex-protected (OSC traffic is
// low-rate, no need for lock-free).
//==============================================================================
class OscBridge : private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
{
public:
    bool listen (int port)
    {
        stop();
        if (! receiver.connect (port))
            return false;
        receiver.addListener (this);
        listenPort = port;
        return true;
    }

    void stop()
    {
        receiver.removeListener (this);
        receiver.disconnect();
        listenPort = -1;
    }

    int getListenPort() const noexcept { return listenPort; }

    std::vector<juce::OSCMessage> drain()
    {
        std::lock_guard<std::mutex> lock (mu);
        std::vector<juce::OSCMessage> out;
        std::swap (out, queue);
        return out;
    }

    bool send (const std::string& host, int port,
               const juce::OSCMessage& msg)
    {
        juce::OSCSender sender;
        if (! sender.connect (juce::String (host), port)) return false;
        return sender.send (msg);
    }

private:
    void oscMessageReceived (const juce::OSCMessage& msg) override
    {
        std::lock_guard<std::mutex> lock (mu);
        if (queue.size() < 1024)
            queue.push_back (msg);
    }

    void oscBundleReceived (const juce::OSCBundle& b) override
    {
        for (auto& el : b)
            if (el.isMessage())
                oscMessageReceived (el.getMessage());
            else if (el.isBundle())
                oscBundleReceived (el.getBundle());
    }

    juce::OSCReceiver              receiver;
    std::mutex                     mu;
    std::vector<juce::OSCMessage>  queue;
    int                            listenPort = -1;
};

// Helper: emit a built-in event if auto-emit is on.
// dataJson should already be valid JSON (empty string = no data field).
static inline void emitAuto (EventBroker& b, const char* name,
                             const std::string& dataJson = {})
{
    if (! b.autoEmit.load()) return;
    std::ostringstream o;
    o << "{\"name\":\"" << name << "\"";
    if (! dataJson.empty()) o << ",\"data\":" << dataJson;
    o << "}";
    b.emit (o.str());
}

//==============================================================================
// Decode a MidiEvent and call on_midi(msg) if defined. Lua-thread only.
// Also emits a "midi_in" event when broker.autoEmitMidiIn is set — useful
// for watching MIDI traffic in the browser UI without writing a script.
//==============================================================================
static void dispatchMidiToLua (sol::state& lua, const MidiEvent& e,
                               EventBroker& broker)
{
    const uint8_t status  = e.data[0] & 0xF0;
    const uint8_t channel = (e.data[0] & 0x0F) + 1;

    const char* type = "other";
    int  note = -1, value = -1, cc = -1;
    if (status == 0x90 && e.size >= 3 && e.data[2] > 0)
        { type = "note_on";   note = e.data[1]; value = e.data[2]; }
    else if (status == 0x80 || (status == 0x90 && e.size >= 3 && e.data[2] == 0))
        { type = "note_off";  note = e.data[1]; value = e.data[2]; }
    else if (status == 0xB0 && e.size >= 3)
        { type = "cc";        cc   = e.data[1]; value = e.data[2]; }
    else if (status == 0xE0 && e.size >= 3)
        { type = "pitchbend"; value = (int) ((e.data[1] & 0x7F) | ((e.data[2] & 0x7F) << 7)) - 8192; }

    if (broker.autoEmitMidiIn.load() && broker.autoEmit.load())
    {
        std::ostringstream o;
        o << "{\"type\":\"" << type << "\",\"channel\":" << (int) channel;
        if (note  >= 0) o << ",\"note\":" << note;
        if (cc    >= 0) o << ",\"cc\":"   << cc;
        if (value >= 0 || std::string(type) == "pitchbend")
            o << ",\"value\":" << value;
        o << "}";
        broker.emit ("{\"name\":\"midi_in\",\"data\":" + o.str() + "}");
    }

    sol::object cb = lua["on_midi"];
    if (cb.get_type() != sol::type::function) return;

    double dispatchTime = juce::Time::getMillisecondCounterHiRes() * 0.001;

    sol::table msg = lua.create_table();
    msg["channel"]       = (int) channel;
    msg["receive_time"]  = e.receiveTimeSecs;
    msg["dispatch_time"] = dispatchTime;
    msg["latency_ms"]    = (dispatchTime - e.receiveTimeSecs) * 1000.0;
    msg["type"]          = type;
    if (note  >= 0) msg["note"]     = note;
    if (cc    >= 0) msg["cc"]       = cc;
    if (value >= 0 || std::string(type) == "pitchbend") msg["value"] = value;
    if (std::string(type) == "note_on" || std::string(type) == "note_off")
        msg["velocity"] = value;
    if (std::string(type) == "other") msg["status"] = (int) e.data[0];

    auto result = cb.as<sol::protected_function>()(msg);
    if (!result.valid())
    {
        sol::error err = result;
        std::cerr << "\n[on_midi error] " << err.what() << "\n" << std::flush;
    }
}

// Forward decl — defined below; needed by dispatchOscToLua for the osc_in
// auto-emit JSON serialization.
static void luaToJson (std::ostringstream& out, sol::object obj, int depth);

//==============================================================================
// Drain queued OSC messages and dispatch on_osc(address, args). Lua-thread
// only. Also emits an osc_in event per message when autoEmitOscIn is on.
//==============================================================================
static void dispatchOscToLua (sol::state& lua, OscBridge& osc, EventBroker& broker)
{
    auto msgs = osc.drain();
    if (msgs.empty()) return;

    sol::object cb = lua["on_osc"];
    const bool hasCallback = (cb.get_type() == sol::type::function);

    for (auto& msg : msgs)
    {
        const std::string addr = msg.getAddressPattern().toString().toStdString();

        // Build a numeric-indexed Lua table of args.
        sol::table args = lua.create_table();
        for (int i = 0; i < msg.size(); ++i)
        {
            const auto& a = msg[i];
            if      (a.isInt32())   args[i + 1] = a.getInt32();
            else if (a.isFloat32()) args[i + 1] = (double) a.getFloat32();
            else if (a.isString())  args[i + 1] = a.getString().toStdString();
            else                    args[i + 1] = sol::lua_nil;
        }

        if (broker.autoEmitOscIn.load() && broker.autoEmit.load())
        {
            std::ostringstream o;
            o << "{\"address\":\"";
            for (unsigned char c : addr)
                if (c == '"' || c == '\\') { o << '\\'; o << (char) c; }
                else                       { o << (char) c; }
            o << "\",\"args\":";
            luaToJson (o, args, 0);
            o << "}";
            broker.emit ("{\"name\":\"osc_in\",\"data\":" + o.str() + "}");
        }

        if (! hasCallback) continue;

        auto result = cb.as<sol::protected_function>() (addr, args);
        if (! result.valid())
        {
            sol::error err = result;
            std::cerr << "\n[on_osc error] " << err.what() << "\n" << std::flush;
        }
    }
}

//==============================================================================
// Minimal Lua-to-JSON serializer. Only what daw.emit needs: nil/bool/number/
// string/table. Tables are arrays iff all keys are integers 1..N with no gaps.
//==============================================================================
static void luaToJson (std::ostringstream& out, sol::object obj, int depth = 0)
{
    if (depth > 16) { out << "null"; return; }
    switch (obj.get_type())
    {
        case sol::type::nil:     out << "null"; break;
        case sol::type::boolean: out << (obj.as<bool>() ? "true" : "false"); break;
        case sol::type::number:
        {
            double d = obj.as<double>();
            if (std::isfinite (d)) out << d;
            else                   out << "null";
            break;
        }
        case sol::type::string:
        {
            out << '"';
            for (unsigned char c : obj.as<std::string>())
            {
                switch (c)
                {
                    case '"':  out << "\\\""; break;
                    case '\\': out << "\\\\"; break;
                    case '\n': out << "\\n";  break;
                    case '\r': out << "\\r";  break;
                    case '\t': out << "\\t";  break;
                    default:
                        if (c < 0x20)
                        {
                            char buf[8];
                            std::snprintf (buf, sizeof buf, "\\u%04x", (unsigned) c);
                            out << buf;
                        }
                        else
                        {
                            out << (char) c;
                        }
                }
            }
            out << '"';
            break;
        }
        case sol::type::table:
        {
            sol::table t = obj.as<sol::table>();
            int count = 0, maxIdx = 0;
            bool isArray = true;
            t.for_each ([&] (sol::object k, sol::object) {
                ++count;
                if (k.get_type() != sol::type::number) { isArray = false; return; }
                double n = k.as<double>();
                int    i = (int) n;
                if ((double) i != n || i < 1) { isArray = false; return; }
                if (i > maxIdx) maxIdx = i;
            });
            if (isArray && maxIdx != count) isArray = false;

            if (isArray)
            {
                out << '[';
                for (int i = 1; i <= maxIdx; ++i)
                {
                    if (i > 1) out << ',';
                    luaToJson (out, t[i], depth + 1);
                }
                out << ']';
            }
            else
            {
                out << '{';
                bool first = true;
                t.for_each ([&] (sol::object k, sol::object v) {
                    if (! first) out << ',';
                    first = false;
                    std::string key;
                    if (k.get_type() == sol::type::string)
                        key = k.as<std::string>();
                    else if (k.get_type() == sol::type::number)
                        key = std::to_string (k.as<double>());
                    out << '"';
                    for (unsigned char c : key)
                    {
                        if (c == '"' || c == '\\') out << '\\';
                        out << (char) c;
                    }
                    out << "\":";
                    luaToJson (out, v, depth + 1);
                });
                out << '}';
            }
            break;
        }
        default: out << "null"; break;
    }
}

//==============================================================================
// PluginEditorWindow — hosts a juce::AudioProcessorEditor in a DocumentWindow
// so VST plugin GUIs can be opened for debugging. Used by daw.show_editor.
//==============================================================================
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow (const juce::String& name,
                        juce::AudioProcessorEditor* editor,
                        std::function<void()> onCloseRequested)
        : juce::DocumentWindow (name, juce::Colours::darkgrey,
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseRequested))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);       // window now owns + deletes editor
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        // Defer deletion — we're inside the window's own event handler, so
        // we can't let the caller destroy us synchronously.
        if (onClose)
            juce::MessageManager::callAsync (onClose);
    }

private:
    std::function<void()> onClose;
};

using PluginEditorMap = std::map<int, std::unique_ptr<PluginEditorWindow>>;

//==============================================================================
// Scheduler — beat-based one-shot and repeating timers, fired on the REPL
// thread's poll loop (~10ms granularity). Delay/interval are specified in
// beats and converted to wall-clock ms at schedule time using current BPM;
// pending tasks do not re-scale if BPM changes mid-flight.
//
// Callbacks receive (count) — 1 on first call, 2 on second, etc. A callback
// returning boolean `false` cancels its own task; any other return (including
// nil) continues. Errors are logged but don't abort future tasks.
//==============================================================================
class Scheduler
{
public:
    int schedule (double delayBeats, double intervalBeats,
                  sol::protected_function fn, double bpm)
    {
        std::lock_guard<std::mutex> lock (mu);
        const int id     = ++nextId;
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double bms = 60000.0 / bpm;
        tasks.push_back ({ id, now + delayBeats * bms, intervalBeats * bms,
                           std::move (fn), 0 });
        return id;
    }

    bool cancel (int id)
    {
        std::lock_guard<std::mutex> lock (mu);
        auto it = std::remove_if (tasks.begin(), tasks.end(),
                                  [id](auto& t) { return t.id == id; });
        const bool found = (it != tasks.end());
        tasks.erase (it, tasks.end());
        return found;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock (mu);
        tasks.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock (mu);
        return tasks.size();
    }

    // Called from the REPL poll loop. Fires any ready tasks, drops one-shots.
    void tick()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();

        struct Fire { int id; int count; sol::protected_function fn; };
        std::vector<Fire> toFire;
        {
            std::lock_guard<std::mutex> lock (mu);
            for (auto it = tasks.begin(); it != tasks.end(); )
            {
                if (now < it->triggerTimeMs) { ++it; continue; }

                ++it->callCount;
                toFire.push_back ({ it->id, it->callCount, it->fn });

                if (it->intervalMs > 0)
                {
                    // Catch up if we fell behind (e.g. after a stall)
                    do { it->triggerTimeMs += it->intervalMs; }
                    while (now >= it->triggerTimeMs);
                    ++it;
                }
                else
                {
                    it = tasks.erase (it);
                }
            }
        }

        for (auto& f : toFire)
        {
            auto result = f.fn (f.count);
            if (! result.valid())
            {
                sol::error err = result;
                std::cerr << "\n[scheduler error] " << err.what()
                          << "\n" << std::flush;
                continue;
            }
            if (result.return_count() > 0)
            {
                sol::object first = result;
                if (first.get_type() == sol::type::boolean && first.as<bool>() == false)
                    cancel (f.id);
            }
        }
    }

private:
    struct ScheduledTask
    {
        int    id;
        double triggerTimeMs;    // wall-clock target
        double intervalMs;       // 0 = one-shot
        sol::protected_function fn;
        int    callCount;
    };

    mutable std::mutex         mu;
    std::vector<ScheduledTask> tasks;
    int                        nextId = 0;
};

// Hot-reload state for the (currently single) watched script. Owned by
// LuaRepl, passed by reference to registerDawApi so daw.unwatch() and
// daw.state() can read/clear it.
struct WatchState
{
    std::string path;
    juce::Time  mtime;
    bool        pending   = false;
    int         pollCount = 0;
};

// Look up the first plugin's automatable parameter on an AudioTrack by ID,
// falling back to a case-insensitive match on the display name.
static te::AutomatableParameter*
findFirstPluginParam (te::AudioTrack* at, const std::string& name)
{
    if (at == nullptr || at->pluginList.size() == 0) return nullptr;
    auto* plug = at->pluginList[0];
    const juce::String n (name);
    for (auto* p : plug->getAutomatableParameters())
        if (p->paramID == n) return p;
    for (auto* p : plug->getAutomatableParameters())
        if (p->getParameterName().equalsIgnoreCase (n)) return p;
    return nullptr;
}

//==============================================================================
// Register the `daw` table.
// onWatch(path) is called when load_script succeeds — lets LuaRepl set up watching.
//==============================================================================
static void registerDawApi (sol::state& lua,
                             te::Edit& edit,
                             MidiEventQueue& midiQueue,
                             std::unique_ptr<juce::MidiOutput>& midiOutput,
                             PluginEditorMap& pluginEditors,
                             EventBroker& eventBroker,
                             Scheduler& scheduler,
                             WatchState& watch,
                             OscBridge& osc,
                             std::function<void(const std::string&)> onWatch,
                             PythonHost& pythonHost)
{
    auto daw = lua.create_named_table ("daw");

    // --- transport ---

    daw.set_function ("play", [&edit, &eventBroker]() {
        juce::MessageManager::callAsync ([&edit, &eventBroker] {
            edit.getTransport().play (false);
            std::ostringstream o;
            o << "{\"playing\":true,\"position\":"
              << edit.getTransport().getPosition().inSeconds() << "}";
            emitAuto (eventBroker, "transport", o.str());
        });
    });

    daw.set_function ("stop", [&edit, &eventBroker]() {
        juce::MessageManager::callAsync ([&edit, &eventBroker] {
            edit.getTransport().stop (false, false);
            std::ostringstream o;
            o << "{\"playing\":false,\"position\":"
              << edit.getTransport().getPosition().inSeconds() << "}";
            emitAuto (eventBroker, "transport", o.str());
        });
    });

    daw.set_function ("position", [&edit]() -> double {
        return edit.getTransport().getPosition().inSeconds();
    });

    // daw.position_beats() — total beats since edit start (fractional)
    daw.set_function ("position_beats", [&edit]() -> double {
        auto pos = edit.getTransport().getPosition();
        return edit.tempoSequence.toBeats (pos).inBeats();
    });

    // daw.position_bars() → { bar, beat, numerator } (1-based for musical display)
    //   bar 1, beat 1.0   = start of edit
    //   bar 2, beat 1.0   = after one full bar
    //   bar 1, beat 3.5   = halfway between beat 3 and beat 4 of bar 1
    daw.set_function ("position_bars",
        [&edit](sol::this_state ts) -> sol::table {
            auto pos = edit.getTransport().getPosition();
            auto bb  = edit.tempoSequence.toBarsAndBeats (pos);
            sol::state_view lv (ts);
            sol::table t = lv.create_table();
            t["bar"]       = bb.bars + 1;
            t["beat"]      = bb.beats.inBeats() + 1.0;
            t["numerator"] = bb.numerator;
            return t;
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
    daw.set_function ("load_4osc", [&edit, &eventBroker](int trackIdx) -> bool {
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
            a->edit->getTransport().ensureContextAllocated (true);   // see load_plugin for rationale
            a->ok = true;
            return nullptr;
        }, &args);
        if (args.ok)
        {
            std::cout << "[4osc] loaded on track " << trackIdx << "\n" << std::flush;
            std::ostringstream o;
            o << "{\"track\":" << trackIdx
              << ",\"name\":\"4osc\",\"format\":\"tracktion\",\"ok\":true}";
            emitAuto (eventBroker, "plugin_load", o.str());
        }
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

    // daw.show_editor(trackIdx)
    // Open a window hosting the native GUI of the first external plugin on
    // the given track. Useful for debugging when running an otherwise
    // headless session. Only supports te::ExternalPlugin (VST2/VST3) —
    // Tracktion built-ins like 4osc use their own UI path.
    // Returns true on success.
    daw.set_function ("show_editor",
        [&edit, &pluginEditors, &eventBroker](int trackIdx) -> bool {
            struct Args {
                te::Edit*         edit;
                int               idx;
                PluginEditorMap*  wins;
                bool              ok;
                std::string       err;
            };
            Args args { &edit, trackIdx, &pluginEditors, false, {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a     = static_cast<Args*> (ctx);
                    auto  tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { a->err = "track index out of range"; return nullptr; }

                    auto* at = tracks[a->idx - 1];
                    for (auto* plug : at->pluginList)
                    {
                        auto* ep = dynamic_cast<te::ExternalPlugin*> (plug);
                        if (! ep) continue;
                        auto* api = ep->getAudioPluginInstance();
                        if (! api) continue;
                        auto* editor = api->createEditorIfNeeded();
                        if (! editor)
                            { a->err = "plugin has no editor"; return nullptr; }

                        // Replace any existing window for this track
                        a->wins->erase (a->idx);
                        int idx    = a->idx;
                        auto* wins = a->wins;
                        (*a->wins)[a->idx] = std::make_unique<PluginEditorWindow> (
                            plug->getName(), editor,
                            [wins, idx] { wins->erase (idx); });
                        a->ok = true;
                        return nullptr;
                    }
                    a->err = "no external plugin on this track";
                    return nullptr;
                }, &args);

            if (args.ok)
                std::cout << "[show_editor] opened for track " << trackIdx
                          << "\n" << std::flush;
            else
                std::cerr << "[show_editor error] " << args.err
                          << "\n" << std::flush;
            {
                std::ostringstream o;
                o << "{\"track\":" << trackIdx
                  << ",\"action\":\"open\",\"ok\":" << (args.ok ? "true" : "false");
                if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
                o << "}";
                emitAuto (eventBroker, "plugin_editor", o.str());
            }
            return args.ok;
        });

    // daw.close_editor(trackIdx)
    daw.set_function ("close_editor",
        [&pluginEditors, &eventBroker](int trackIdx) {
            struct Args { PluginEditorMap* wins; int idx; };
            Args args { &pluginEditors, trackIdx };
            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    a->wins->erase (a->idx);
                    return nullptr;
                }, &args);
            std::ostringstream o;
            o << "{\"track\":" << trackIdx << ",\"action\":\"close\"}";
            emitAuto (eventBroker, "plugin_editor", o.str());
        });

    daw.set_function ("open_audio_device", [&edit, &eventBroker](const std::string& name) {
        auto& adm = edit.engine.getDeviceManager().deviceManager;
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        adm.getAudioDeviceSetup (setup);
        setup.outputDeviceName = name;
        setup.sampleRate       = 48000.0;
        setup.bufferSize       = 512;
        setup.outputChannels.setRange (0, 2, true);
        auto err = adm.setAudioDeviceSetup (setup, true);
        if (err.isEmpty())
            std::cout << "[audio] opened: " << name << "\n" << std::flush;
        else
            std::cout << "[audio error] " << err << "\n" << std::flush;
        std::ostringstream o;
        o << "{\"name\":\"" << name << "\",\"ok\":" << (err.isEmpty() ? "true" : "false");
        if (! err.isEmpty()) o << ",\"error\":\"" << err.toStdString() << "\"";
        o << "}";
        emitAuto (eventBroker, "audio_device_open", o.str());
    });

    // daw.state() → JSON string snapshot of bpm, transport, tracks, plugins,
    // and clip slots. Returned as a string so /eval round-trips it verbatim
    // and the browser can JSON.parse it directly. Lua-as-query: scripts can
    // also call this and emit/log subsets.
    daw.set_function ("state", [&edit, &watch]() -> std::string {
        struct Args { te::Edit* edit; std::string out; std::string watching; };
        Args args { &edit, {}, watch.path };
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void* ctx) -> void* {
                auto* a = static_cast<Args*> (ctx);
                auto esc = [](const std::string& s) {
                    std::string r; r.reserve (s.size() + 2);
                    for (unsigned char c : s) {
                        switch (c) {
                            case '"':  r += "\\\""; break;
                            case '\\': r += "\\\\"; break;
                            case '\n': r += "\\n";  break;
                            case '\r': r += "\\r";  break;
                            case '\t': r += "\\t";  break;
                            default:
                                if (c < 0x20) {
                                    char buf[8];
                                    std::snprintf (buf, sizeof buf, "\\u%04x", (unsigned) c);
                                    r += buf;
                                } else r += (char) c;
                        }
                    }
                    return r;
                };
                std::ostringstream o;
                auto& tr = a->edit->getTransport();
                auto pos = tr.getPosition();
                auto bb  = a->edit->tempoSequence.toBarsAndBeats (pos);
                o << "{\"bpm\":" << a->edit->tempoSequence.getTempo (0)->getBpm()
                  << ",\"playing\":" << (tr.isPlaying() ? "true" : "false")
                  << ",\"position\":" << pos.inSeconds()
                  << ",\"position_beats\":" << a->edit->tempoSequence.toBeats (pos).inBeats()
                  << ",\"bar\":" << (bb.bars + 1)
                  << ",\"beat\":" << (bb.beats.inBeats() + 1.0)
                  << ",\"watching\":";
                if (a->watching.empty()) o << "null";
                else                     o << "\"" << esc (a->watching) << "\"";
                o << ",\"tracks\":[";
                auto tracks = te::getAudioTracks (*a->edit);
                for (int i = 0; i < (int) tracks.size(); ++i)
                {
                    if (i > 0) o << ",";
                    auto* at = tracks[i];
                    o << "{\"index\":" << (i + 1)
                      << ",\"name\":\"" << esc (at->getName().toStdString()) << "\""
                      << ",\"plugins\":[";
                    int pi = 0;
                    for (auto* plug : at->pluginList)
                    {
                        if (pi++ > 0) o << ",";
                        o << "{\"name\":\"" << esc (plug->getName().toStdString()) << "\""
                          << ",\"type\":\"" << esc (plug->getPluginType().toStdString()) << "\"";
                        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (plug))
                        {
                            o << ",\"format\":\"VST\",\"synth\":"
                              << (ep->isSynth() ? "true" : "false");
                            auto err = ep->getLoadError();
                            if (err.isNotEmpty())
                                o << ",\"error\":\"" << esc (err.toStdString()) << "\"";
                        }
                        else
                        {
                            o << ",\"format\":\"builtin\"";
                        }
                        o << "}";
                    }
                    o << "],\"clips\":[";
                    auto slots = at->getClipSlotList().getClipSlots();
                    for (int s = 0; s < (int) slots.size(); ++s)
                    {
                        if (s > 0) o << ",";
                        o << "{\"slot\":" << (s + 1);
                        if (auto* clip = slots[s]->getClip())
                        {
                            o << ",\"name\":\"" << esc (clip->getName().toStdString()) << "\"";
                            if (auto lh = clip->getLaunchHandle())
                            {
                                bool playing = lh->getPlayingStatus()
                                               == te::LaunchHandle::PlayState::playing;
                                o << ",\"playing\":" << (playing ? "true" : "false");
                            }
                        }
                        o << "}";
                    }
                    o << "]}";
                }
                o << "]}";
                a->out = o.str();
                return nullptr;
            }, &args);
        return args.out;
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

    // daw.bpm()       → read current BPM
    // daw.bpm(120)    → set BPM, returns the value as applied (or coerced).
    // Both forms run synchronously on the message thread so the returned
    // value is the live current BPM — useful at the REPL ("did it stick?")
    // and in scripts (no need to query separately).
    auto applyBpm = [&edit, &eventBroker](double bpm) -> double {
        struct Args { te::Edit* edit; double bpm; double after; };
        Args args { &edit, bpm, 0.0 };
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void* ctx) -> void* {
                auto* a = static_cast<Args*> (ctx);
                a->edit->tempoSequence.getTempo (0)->setBpm (a->bpm);
                a->after = a->edit->tempoSequence.getTempo (0)->getBpm();
                return nullptr;
            }, &args);
        std::ostringstream o;
        o << "{\"bpm\":" << args.after << "}";
        emitAuto (eventBroker, "bpm", o.str());
        return args.after;
    };

    daw.set_function ("bpm", sol::overload (
        [&edit]() -> double {
            return edit.tempoSequence.getTempo (0)->getBpm();
        },
        [applyBpm](double bpm) -> double { return applyBpm (bpm); }));

    // Kept for backward compat — daw.bpm(N) is the new idiom.
    daw.set_function ("set_bpm",
        [applyBpm](double bpm) -> double { return applyBpm (bpm); });

    // --- clips ---

    daw.set_function ("clip",
        [&edit, &eventBroker](sol::this_state ts, int trackIdx, int slotIdx) -> sol::object {
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

        t["launch"] = [&edit, &eventBroker, trackIdx, slotIdx]() {
            juce::MessageManager::callAsync ([&edit, &eventBroker, trackIdx, slotIdx] {
                auto tr = te::getAudioTracks (edit);
                if (trackIdx < 1 || trackIdx > tr.size()) return;
                auto sl = tr[trackIdx - 1]->getClipSlotList().getClipSlots();
                if (slotIdx < 1 || slotIdx > sl.size()) return;
                if (auto* c = sl[slotIdx - 1]->getClip())
                    if (auto lh = c->getLaunchHandle())
                    {
                        auto loopBeats = c->getLoopLengthBeats();
                        if (loopBeats <= te::BeatDuration{})
                            loopBeats = c->getEndBeat() - c->getStartBeat();
                        if (loopBeats > te::BeatDuration{})
                            lh->setLooping (loopBeats);
                        lh->play (getLaunchPosition (edit));

                        std::ostringstream o;
                        o << "{\"track\":" << trackIdx
                          << ",\"slot\":" << slotIdx
                          << ",\"name\":\"" << c->getName().toStdString() << "\"}";
                        emitAuto (eventBroker, "clip_launch", o.str());
                    }
            });
        };

        t["stop"] = [&edit, &eventBroker, trackIdx, slotIdx]() {
            juce::MessageManager::callAsync ([&edit, &eventBroker, trackIdx, slotIdx] {
                auto tr = te::getAudioTracks (edit);
                if (trackIdx < 1 || trackIdx > tr.size()) return;
                auto sl = tr[trackIdx - 1]->getClipSlotList().getClipSlots();
                if (slotIdx < 1 || slotIdx > sl.size()) return;
                if (auto* c = sl[slotIdx - 1]->getClip())
                    if (auto lh = c->getLaunchHandle())
                    {
                        lh->stop ({});
                        std::ostringstream o;
                        o << "{\"track\":" << trackIdx << ",\"slot\":" << slotIdx << "}";
                        emitAuto (eventBroker, "clip_stop", o.str());
                    }
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

    // daw.add_track() → 1-based index of the new track
    // Adds one audio track with the same number of clip slots as existing
    // tracks (or 2 if there are none), then rebuilds the audio graph so
    // injectLiveMidiMessage and clip launching work immediately.
    daw.set_function ("add_track", [&edit, &eventBroker]() -> int {
        struct Args { te::Edit* edit; int newIdx; };
        Args args { &edit, 0 };
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void* ctx) -> void* {
                auto* a = static_cast<Args*> (ctx);
                int slotsPerTrack = 2;
                {
                    auto existing = te::getAudioTracks (*a->edit);
                    if (! existing.isEmpty())
                        slotsPerTrack = existing[0]->getClipSlotList().getClipSlots().size();
                    a->edit->ensureNumberOfAudioTracks (existing.size() + 1);
                }
                auto tracks = te::getAudioTracks (*a->edit);
                a->newIdx = tracks.size();
                tracks[a->newIdx - 1]->getClipSlotList().ensureNumberOfSlots (slotsPerTrack);
                a->edit->getTransport().ensureContextAllocated (true);
                return nullptr;
            }, &args);

        std::ostringstream o;
        o << "{\"track\":" << args.newIdx << "}";
        emitAuto (eventBroker, "track_add", o.str());
        std::cout << "[track] added track " << args.newIdx << "\n" << std::flush;
        return args.newIdx;
    });

    // daw.create_clip(track, slot [, length_beats=4])
    // Inserts an empty MIDI clip into the slot. Errors out if the slot is
    // already occupied — clear the existing clip first if you want to
    // replace. Length is set from current BPM at call time.
    daw.set_function ("create_clip",
        [&edit, &eventBroker](int trackIdx, int slotIdx,
                              sol::optional<double> beatsOpt) -> bool {
            struct Args { te::Edit* edit; int trackIdx; int slotIdx;
                          double beats; bool ok; std::string err; };
            Args args { &edit, trackIdx, slotIdx, beatsOpt.value_or (4.0),
                        false, {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto tracks = te::getAudioTracks (*a->edit);
                    if (a->trackIdx < 1 || a->trackIdx > (int) tracks.size())
                        { a->err = "track out of range"; return nullptr; }
                    auto slots = tracks[a->trackIdx - 1]->getClipSlotList().getClipSlots();
                    if (a->slotIdx < 1 || a->slotIdx > (int) slots.size())
                        { a->err = "slot out of range"; return nullptr; }
                    auto* slot = slots[a->slotIdx - 1];
                    if (slot->getClip() != nullptr)
                        { a->err = "slot already has a clip"; return nullptr; }
                    auto end = a->edit->tempoSequence.toTime (
                                   te::BeatPosition::fromBeats (a->beats));
                    auto clip = te::insertMIDIClip (*slot,
                                                    te::TimeRange { te::TimePosition{}, end });
                    if (clip == nullptr)
                        { a->err = "insertMIDIClip failed"; return nullptr; }
                    a->ok = true;
                    return nullptr;
                }, &args);

            std::ostringstream o;
            o << "{\"track\":" << trackIdx << ",\"slot\":" << slotIdx
              << ",\"beats\":" << args.beats
              << ",\"ok\":" << (args.ok ? "true" : "false");
            if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
            o << "}";
            emitAuto (eventBroker, "clip_create", o.str());
            if (! args.ok)
                std::cerr << "[create_clip error] " << args.err << "\n" << std::flush;
            return args.ok;
        });

    // daw.add_note(track, slot, pitch, start_beat, length_beats [, vel=100])
    // Adds one MIDI note to the clip at (track, slot). Beats are within the
    // clip's local timeline (0 = start of clip). Velocity 1..127.
    daw.set_function ("add_note",
        [&edit](int trackIdx, int slotIdx, int pitch,
                double startBeat, double lengthBeats,
                sol::optional<int> velOpt) -> bool {
            struct Args { te::Edit* edit; int trackIdx; int slotIdx;
                          int pitch; double start; double length; int vel;
                          bool ok; std::string err; };
            Args args { &edit, trackIdx, slotIdx, pitch, startBeat, lengthBeats,
                        velOpt.value_or (100), false, {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto tracks = te::getAudioTracks (*a->edit);
                    if (a->trackIdx < 1 || a->trackIdx > (int) tracks.size())
                        { a->err = "track out of range"; return nullptr; }
                    auto slots = tracks[a->trackIdx - 1]->getClipSlotList().getClipSlots();
                    if (a->slotIdx < 1 || a->slotIdx > (int) slots.size())
                        { a->err = "slot out of range"; return nullptr; }
                    auto* clip = dynamic_cast<te::MidiClip*> (slots[a->slotIdx - 1]->getClip());
                    if (clip == nullptr)
                        { a->err = "no MIDI clip in slot — call create_clip first"; return nullptr; }
                    clip->getSequence().addNote (
                        a->pitch,
                        te::BeatPosition::fromBeats (a->start),
                        te::BeatDuration::fromBeats (a->length),
                        a->vel, 0, nullptr);
                    a->ok = true;
                    return nullptr;
                }, &args);

            if (! args.ok)
                std::cerr << "[add_note error] " << args.err << "\n" << std::flush;
            return args.ok;
        });

    // daw.clear_clip(track, slot) — remove the clip from a slot
    daw.set_function ("clear_clip",
        [&edit, &eventBroker](int trackIdx, int slotIdx) -> bool {
            struct Args { te::Edit* edit; int trackIdx; int slotIdx;
                          bool ok; std::string err; };
            Args args { &edit, trackIdx, slotIdx, false, {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto tracks = te::getAudioTracks (*a->edit);
                    if (a->trackIdx < 1 || a->trackIdx > (int) tracks.size())
                        { a->err = "track out of range"; return nullptr; }
                    auto slots = tracks[a->trackIdx - 1]->getClipSlotList().getClipSlots();
                    if (a->slotIdx < 1 || a->slotIdx > (int) slots.size())
                        { a->err = "slot out of range"; return nullptr; }
                    if (auto* clip = slots[a->slotIdx - 1]->getClip())
                        clip->removeFromParent();
                    a->ok = true;
                    return nullptr;
                }, &args);

            std::ostringstream o;
            o << "{\"track\":" << trackIdx << ",\"slot\":" << slotIdx
              << ",\"ok\":" << (args.ok ? "true" : "false");
            if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
            o << "}";
            emitAuto (eventBroker, "clip_clear", o.str());
            return args.ok;
        });

    // daw.reset() — wipe all session content back to a clean slate.
    //   - stops transport
    //   - cancels every scheduled task (daw.after / daw.every)
    //   - clears every on_end follow callback
    //   - removes every clip from every slot
    //   - unloads every plugin from every track
    //   - closes any open plugin editor windows
    // Preserved: track count, BPM, daw.store, MIDI input/output device
    // routing. (Clear those manually if you want a truly empty edit.)
    daw.set_function ("reset",
        [&edit, &eventBroker, &pluginEditors, &scheduler]
        (sol::this_state ts) {
            scheduler.clear();

            // Wipe the Lua-side follow callback table so callbacks from
            // the previous setup don't fire after the engine state changes.
            sol::state_view lv (ts);
            lv["daw"]["_follow"] = lv.create_table();

            struct Args { te::Edit* edit; PluginEditorMap* wins;
                          int trackCount; int pluginCount; int clipCount; };
            Args args { &edit, &pluginEditors, 0, 0, 0 };
            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    a->edit->getTransport().stop (false, false);
                    a->wins->clear();
                    auto tracks = te::getAudioTracks (*a->edit);
                    a->trackCount = tracks.size();
                    for (auto* at : tracks)
                    {
                        a->pluginCount += at->pluginList.size();
                        at->pluginList.clear();
                        for (auto* slot : at->getClipSlotList().getClipSlots())
                            if (auto* clip = slot->getClip())
                                { ++a->clipCount; clip->removeFromParent(); }
                    }
                    a->edit->getTransport().ensureContextAllocated (true);
                    return nullptr;
                }, &args);

            std::ostringstream o;
            o << "{\"tracks\":" << args.trackCount
              << ",\"plugins_removed\":" << args.pluginCount
              << ",\"clips_removed\":"   << args.clipCount << "}";
            emitAuto (eventBroker, "reset", o.str());
            std::cout << "[reset] cleared " << args.pluginCount
                      << " plugins and " << args.clipCount
                      << " clips across " << args.trackCount
                      << " tracks\n" << std::flush;
        });

    // --- MIDI ---

    // daw.inject_midi(note, vel [, channel=1])
    // Pushes a synthetic note-on into the MIDI queue, which fires the Lua
    // on_midi(msg) callback. Does NOT play any instrument — for that, use
    // daw.note_on / daw.note_off below.
    daw.set_function ("inject_midi", [&midiQueue](int note, int vel, sol::optional<int> ch) {
        MidiEvent e;
        e.data[0]         = uint8_t (0x90 | ((ch.value_or (1) - 1) & 0x0F));
        e.data[1]         = uint8_t (note & 0x7F);
        e.data[2]         = uint8_t (vel  & 0x7F);
        e.size            = 3;
        e.receiveTimeSecs = juce::Time::getMillisecondCounterHiRes() * 0.001;
        midiQueue.push (e);
    });

    // daw.note_on(track, note, vel [, channel=1])
    // daw.note_off(track, note [, channel=1])
    // Send a synthetic MIDI message directly to the instrument loaded on
    // the given track (1-based) via AudioTrack::injectLiveMidiMessage —
    // the same path Tracktion uses for live controller input.
    daw.set_function ("note_on",
        [&edit](int trackIdx, int note, int vel, sol::optional<int> ch) {
            int channel = ch.value_or (1);
            juce::MessageManager::callAsync ([&edit, trackIdx, note, vel, channel] {
                auto tr = te::getAudioTracks (edit);
                if (trackIdx < 1 || trackIdx > tr.size()) return;
                tr[trackIdx - 1]->injectLiveMidiMessage (
                    juce::MidiMessage::noteOn (channel, note, (juce::uint8) vel), {});
            });
        });

    daw.set_function ("note_off",
        [&edit](int trackIdx, int note, sol::optional<int> ch) {
            int channel = ch.value_or (1);
            juce::MessageManager::callAsync ([&edit, trackIdx, note, channel] {
                auto tr = te::getAudioTracks (edit);
                if (trackIdx < 1 || trackIdx > tr.size()) return;
                tr[trackIdx - 1]->injectLiveMidiMessage (
                    juce::MidiMessage::noteOff (channel, note), {});
            });
        });

    // --- MIDI output to external devices ---
    // daw.list_midi_outputs(), daw.open_midi_output(name),
    // daw.send_midi(note, vel [, channel])
    //   send_midi with vel == 0 sends note-off; otherwise note-on.
    // daw.send_midi_raw(b1 [, b2 [, b3]]) for arbitrary bytes (CC, PC, etc).

    // getAvailableDevices / openDevice both assert message-thread; wrap.
    daw.set_function ("list_midi_outputs", []() {
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void*) -> void* {
                for (auto& info : juce::MidiOutput::getAvailableDevices())
                    std::cout << "  " << info.name.toStdString() << "\n";
                return nullptr;
            }, nullptr);
    });

    daw.set_function ("open_midi_output",
        [&midiOutput, &eventBroker](const std::string& name) -> bool {
            struct Args { std::unique_ptr<juce::MidiOutput>* out; std::string name; bool ok; };
            Args args { &midiOutput, name, false };
            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    for (auto& info : juce::MidiOutput::getAvailableDevices())
                    {
                        if (info.name.containsIgnoreCase (juce::String (a->name)))
                        {
                            *a->out = juce::MidiOutput::openDevice (info.identifier);
                            if (*a->out)
                            {
                                (*a->out)->startBackgroundThread();
                                std::cout << "[midi out] opened: "
                                          << info.name.toStdString() << "\n";
                                a->ok = true;
                                return nullptr;
                            }
                        }
                    }
                    std::cerr << "[midi out] not found: " << a->name << "\n";
                    return nullptr;
                }, &args);
            std::ostringstream o;
            o << "{\"name\":\"" << name << "\",\"ok\":" << (args.ok ? "true" : "false") << "}";
            emitAuto (eventBroker, "midi_output_open", o.str());
            return args.ok;
        });

    daw.set_function ("send_midi",
        [&midiOutput](int note, int vel, sol::optional<int> ch) {
            if (! midiOutput) return;
            int channel = ch.value_or (1);
            auto msg = vel > 0
                ? juce::MidiMessage::noteOn  (channel, note, (juce::uint8) vel)
                : juce::MidiMessage::noteOff (channel, note);
            midiOutput->sendMessageNow (msg);
        });

    daw.set_function ("send_midi_raw",
        [&midiOutput](int b1, sol::optional<int> b2, sol::optional<int> b3) {
            if (! midiOutput) return;
            const juce::uint8 bytes[3] = {
                (juce::uint8) (b1 & 0xFF),
                (juce::uint8) (b2.value_or (0) & 0xFF),
                (juce::uint8) (b3.value_or (0) & 0xFF) };
            int size = b3 ? 3 : (b2 ? 2 : 1);
            midiOutput->sendMessageNow (juce::MidiMessage (bytes, size));
        });

    // --- event broadcast (SSE via GET /events) ---

    // daw.emit(name [, table])
    // Broadcasts a JSON event to all connected SSE clients. Scripts choose
    // what's worth publishing (clip launched, drunk-walk step, custom state).
    // Visualizers subscribe and render whatever they want.
    //
    //   daw.emit("clip_launch", { track = 1, slot = 2 })
    //   daw.emit("beat")
    daw.set_function ("emit",
        [&eventBroker](const std::string& name,
                       sol::optional<sol::table> data) {
            std::ostringstream out;
            out << "{\"name\":\"";
            for (unsigned char c : name)
            {
                if (c == '"' || c == '\\') out << '\\';
                out << (char) c;
            }
            out << "\"";
            if (data.has_value())
            {
                out << ",\"data\":";
                luaToJson (out, data.value());
            }
            out << "}";
            eventBroker.emit (out.str());
        });

    // Master + per-topic toggles for engine-driven events.
    daw.set_function ("auto_emit",
        [&eventBroker](bool on) { eventBroker.autoEmit.store (on); });
    daw.set_function ("auto_emit_midi_in",
        [&eventBroker](bool on) { eventBroker.autoEmitMidiIn.store (on); });
    daw.set_function ("auto_emit_osc_in",
        [&eventBroker](bool on) { eventBroker.autoEmitOscIn.store (on); });

    // --- OSC ---
    //
    //   daw.osc_listen(57120)             -- bind UDP, returns true on success
    //   daw.osc_stop()                    -- stop listening
    //   daw.osc_port()                    -- → port number, or nil if not listening
    //   function on_osc(addr, args) end   -- script-side callback per message
    //   daw.osc_send(host, port, addr, ...)  -- send a message
    //
    // args is a Lua array of int / float / string. ints map to OSC int32,
    // numbers with fractional parts to float32, strings to OSC string.
    // Other Lua types are skipped.

    daw.set_function ("osc_listen",
        [&osc, &eventBroker](int port) -> bool {
            bool ok = osc.listen (port);
            std::ostringstream o;
            o << "{\"port\":" << port << ",\"action\":\"listen\""
              << ",\"ok\":" << (ok ? "true" : "false") << "}";
            emitAuto (eventBroker, "osc", o.str());
            std::cout << (ok ? "[osc] listening on " : "[osc] failed to bind ")
                      << port << "\n" << std::flush;
            return ok;
        });

    daw.set_function ("osc_stop", [&osc, &eventBroker]() {
        int port = osc.getListenPort();
        osc.stop();
        std::ostringstream o;
        o << "{\"port\":" << port << ",\"action\":\"stop\"}";
        emitAuto (eventBroker, "osc", o.str());
    });

    daw.set_function ("osc_port",
        [&osc](sol::this_state ts) -> sol::object {
            sol::state_view lv (ts);
            int p = osc.getListenPort();
            return p > 0 ? sol::make_object (lv, p) : sol::object (sol::lua_nil);
        });

    daw.set_function ("osc_send",
        [&osc](const std::string& host, int port,
               const std::string& address,
               sol::variadic_args vargs) -> bool {
            juce::OSCMessage msg { juce::OSCAddressPattern (juce::String (address)) };
            for (auto a : vargs)
            {
                sol::object obj = a;
                switch (obj.get_type())
                {
                    case sol::type::number:
                    {
                        double d = obj.as<double>();
                        if (d == (double) (int) d)
                            msg.addInt32 ((juce::int32) d);
                        else
                            msg.addFloat32 ((float) d);
                        break;
                    }
                    case sol::type::string:
                        msg.addString (juce::String (obj.as<std::string>()));
                        break;
                    case sol::type::boolean:
                        msg.addInt32 (obj.as<bool>() ? 1 : 0);
                        break;
                    default: break;   // skip nil / table / etc.
                }
            }
            return osc.send (host, port, msg);
        });

    // --- scheduler ---
    // daw.after(beats, fn)     — fire once after `beats` from now
    // daw.every(beats, fn)     — fire every `beats`. Return false to stop.
    // daw.cancel(id)           — cancel a specific task
    // daw.cancel_all()         — cancel everything pending
    //
    // Beats convert to ms at schedule time using current BPM; pending
    // tasks don't rescale if BPM changes. Fires regardless of transport
    // state (so you can prototype without pressing play).
    daw.set_function ("after",
        [&edit, &scheduler](double beats, sol::protected_function fn) -> int {
            return scheduler.schedule (beats, 0.0, std::move (fn),
                                       edit.tempoSequence.getTempo (0)->getBpm());
        });
    daw.set_function ("every",
        [&edit, &scheduler](double beats, sol::protected_function fn) -> int {
            return scheduler.schedule (beats, beats, std::move (fn),
                                       edit.tempoSequence.getTempo (0)->getBpm());
        });
    daw.set_function ("cancel",
        [&scheduler](int id) -> bool { return scheduler.cancel (id); });
    daw.set_function ("cancel_all",
        [&scheduler]() { scheduler.clear(); });

    // --- native Tracktion MIDI input routing ---
    // Unlike the raw juce::MidiInput path (MidiEventQueue → on_midi), this
    // uses Tracktion's DeviceManager so MIDI flows straight into a track's
    // plugin chain with no Lua callback. Both paths can run in parallel;
    // expect duplicate delivery if a script routes the same physical device
    // through both.

    // daw.list_engine_midi_inputs() — Tracktion's view of MIDI input devices
    // daw.rescan_midi() — ask Tracktion to re-enumerate MIDI input/output
    // devices. Useful when a keyboard or controller is plugged in after
    // trakdaw started up; otherwise it won't show in list_engine_midi_inputs
    // until next launch.
    daw.set_function ("rescan_midi", [&edit, &eventBroker]() {
        struct Args { te::Edit* edit; int count; };
        Args args { &edit, 0 };
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void* ctx) -> void* {
                auto* a = static_cast<Args*> (ctx);
                a->edit->engine.getDeviceManager().rescanMidiDeviceList();
                a->count = (int) a->edit->engine.getDeviceManager().getMidiInDevices().size();
                return nullptr;
            }, &args);
        std::ostringstream o;
        o << "{\"midi_in_count\":" << args.count << "}";
        emitAuto (eventBroker, "midi_rescan", o.str());
        std::cout << "[midi] rescanned — " << args.count
                  << " input device(s) visible\n" << std::flush;
    });

    daw.set_function ("list_engine_midi_inputs", [&edit]() {
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void* ctx) -> void* {
                auto& e = *static_cast<te::Edit*> (ctx);
                for (auto& dev : e.engine.getDeviceManager().getMidiInDevices())
                    std::cout << "  " << dev->getName().toStdString()
                              << (dev->isEnabled() ? "  [enabled]" : "") << "\n";
                return nullptr;
            }, &edit);
    });

    // daw.assign_midi_input(deviceName, trackIdx)
    // Enables the named Tracktion MidiInputDevice and routes it to the
    // given track (1-based). Returns true on success.
    daw.set_function ("assign_midi_input",
        [&edit, &eventBroker](const std::string& name, int trackIdx) -> bool {
            struct Args { te::Edit* edit; std::string name; int idx;
                          bool ok; std::string err; };
            Args args { &edit, name, trackIdx, false, {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto  tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { a->err = "track index out of range"; return nullptr; }
                    auto* track = tracks[a->idx - 1];

                    std::shared_ptr<te::MidiInputDevice> device;
                    for (auto& d : a->edit->engine.getDeviceManager().getMidiInDevices())
                        if (d->getName().containsIgnoreCase (juce::String (a->name)))
                            { device = d; break; }
                    if (! device)
                        { a->err = "no matching MIDI input device"; return nullptr; }

                    device->setEnabled (true);
                    a->edit->getTransport().ensureContextAllocated();
                    auto* ctxPlay = a->edit->getCurrentPlaybackContext();
                    if (! ctxPlay)
                        { a->err = "no playback context"; return nullptr; }
                    auto* inst = ctxPlay->getInputFor (device.get());
                    if (! inst)
                        { a->err = "no input instance for device"; return nullptr; }

                    auto result = inst->setTarget (track->itemID, false, nullptr);
                    if (! result)
                        { a->err = result.error().toStdString(); return nullptr; }
                    a->ok = true;
                    return nullptr;
                }, &args);

            if (args.ok)
                std::cout << "[midi-in] " << name << " → track " << trackIdx
                          << "\n" << std::flush;
            else
                std::cerr << "[assign_midi_input error] " << args.err
                          << "\n" << std::flush;
            {
                std::ostringstream o;
                o << "{\"device\":\"" << name << "\""
                  << ",\"track\":" << trackIdx
                  << ",\"action\":\"assign\",\"ok\":" << (args.ok ? "true" : "false");
                if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
                o << "}";
                emitAuto (eventBroker, "midi_input_route", o.str());
            }
            return args.ok;
        });

    // daw.unassign_midi_input(deviceName, trackIdx)
    daw.set_function ("unassign_midi_input",
        [&edit, &eventBroker](const std::string& name, int trackIdx) -> bool {
            struct Args { te::Edit* edit; std::string name; int idx;
                          bool ok; std::string err; };
            Args args { &edit, name, trackIdx, false, {} };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto  tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { a->err = "track index out of range"; return nullptr; }
                    auto* track = tracks[a->idx - 1];

                    std::shared_ptr<te::MidiInputDevice> device;
                    for (auto& d : a->edit->engine.getDeviceManager().getMidiInDevices())
                        if (d->getName().containsIgnoreCase (juce::String (a->name)))
                            { device = d; break; }
                    if (! device)
                        { a->err = "no matching MIDI input device"; return nullptr; }

                    auto* ctxPlay = a->edit->getCurrentPlaybackContext();
                    if (! ctxPlay)
                        { a->err = "no playback context"; return nullptr; }
                    auto* inst = ctxPlay->getInputFor (device.get());
                    if (! inst)
                        { a->err = "no input instance for device"; return nullptr; }

                    auto r = inst->removeTarget (track->itemID, nullptr);
                    if (r.failed())
                        { a->err = r.getErrorMessage().toStdString(); return nullptr; }
                    a->ok = true;
                    return nullptr;
                }, &args);

            if (! args.ok)
                std::cerr << "[unassign_midi_input error] " << args.err
                          << "\n" << std::flush;
            {
                std::ostringstream o;
                o << "{\"device\":\"" << name << "\""
                  << ",\"track\":" << trackIdx
                  << ",\"action\":\"unassign\",\"ok\":" << (args.ok ? "true" : "false");
                if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
                o << "}";
                emitAuto (eventBroker, "midi_input_route", o.str());
            }
            return args.ok;
        });

    // --- scripting ---

    // daw.load_script(path) — execute a Lua file and start watching it for changes
    daw.set_function ("load_script",
        [&lua, &eventBroker, onWatch](const std::string& path) -> bool {
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
            std::ostringstream o;
            o << "{\"path\":\"" << absFile.getFullPathName().toStdString()
              << "\",\"trigger\":\"load\"}";
            emitAuto (eventBroker, "script_load", o.str());
            return true;
        });

    // daw.unwatch() — stop hot-reloading the currently watched script.
    // The script's effects on the engine stay in place; only the file-mtime
    // poll is cancelled.
    daw.set_function ("unwatch", [&watch, &eventBroker]() {
        if (watch.path.empty()) return false;
        std::ostringstream o;
        o << "{\"path\":\"" << watch.path << "\"}";
        emitAuto (eventBroker, "unwatch", o.str());
        std::cout << "[watch] stopped: "
                  << juce::File (watch.path).getFileName() << "\n" << std::flush;
        watch.path.clear();
        watch.pending = false;
        return true;
    });

    // daw.watching() → string path or nil
    daw.set_function ("watching",
        [&watch](sol::this_state ts) -> sol::object {
            sol::state_view lv (ts);
            return watch.path.empty()
                ? sol::object (sol::lua_nil)
                : sol::make_object (lv, watch.path);
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
    daw.set_function ("load_plugin",
        [&edit, &eventBroker](int trackIdx, const std::string& path) -> bool {
        struct Args {
            te::Edit*   edit;
            int         trackIdx;
            std::string path;
            bool        ok;
            std::string err;
            std::string name;      // filled in on success
            std::string format;
        };
        Args args { &edit, trackIdx, path, false, {}, {}, {} };

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
            // Force a graph rebuild so injectLiveMidiMessage actually reaches
            // the new plugin even when the transport is stopped. The older
            // stop/play dance was a no-op when the transport wasn't already
            // playing, which left the audio graph without the new plugin
            // and made daw.note_on silent.
            // Force a full audio-graph rebuild. restartPlayback() alone
            // no-ops when transport has never started (because it routes
            // through editHasChanged(), which bails if playbackContext is
            // null). ensureContextAllocated(true) both creates the context
            // and forces a node reallocation that includes the just-
            // inserted plugin — so injectLiveMidiMessage reaches it even
            // before the user presses play.
            a->edit->getTransport().ensureContextAllocated (true);
            a->name   = results[0]->name.toStdString();
            a->format = results[0]->pluginFormatName.toStdString();
            a->ok = true;
            return nullptr;
        }, &args);

        std::ostringstream o;
        o << "{\"track\":" << trackIdx
          << ",\"name\":\"" << args.name << "\""
          << ",\"format\":\"" << args.format << "\""
          << ",\"path\":\"" << path << "\""
          << ",\"ok\":" << (args.ok ? "true" : "false");
        if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
        o << "}";
        emitAuto (eventBroker, "plugin_load", o.str());

        if (args.ok)
            std::cout << "[load_plugin] loaded on track " << trackIdx << "\n" << std::flush;
        else
            std::cerr << "[load_plugin error] " << args.err << "\n" << std::flush;
        return args.ok;
    });

    // --- patch save / load ---
    //
    // Serializes / restores the entire plugin chain on a track as Tracktion
    // ValueTree XML. Captures the plugin's audio chunk plus any Tracktion-
    // level metadata (parameter values, modifier mappings, plugin order in
    // the rack). Files are plain XML — diff-friendly, scriptable to template.
    //
    // Round-trips within trakdaw / Waveform Pro. For interop with other
    // VST3 hosts, export a .vstpreset instead (separate function, TBD).

    daw.set_function ("save_patch",
        [&edit, &eventBroker](int trackIdx, const std::string& path) -> bool {
            struct Args { te::Edit* edit; int idx; std::string path;
                          bool ok; std::string err; int count; };
            Args args { &edit, trackIdx, path, false, {}, 0 };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto  tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { a->err = "track index out of range"; return nullptr; }
                    auto* at = tracks[a->idx - 1];

                    // Flush each plugin's live state into its ValueTree so
                    // the XML reflects the current parameter values.
                    for (auto* plug : at->pluginList)
                        plug->flushPluginStateToValueTree();
                    a->count = at->pluginList.size();

                    auto xml = at->pluginList.state.toXmlString();
                    juce::File file (a->path);
                    file.getParentDirectory().createDirectory();
                    if (! file.replaceWithText (xml))
                        { a->err = "failed to write file"; return nullptr; }
                    a->ok = true;
                    return nullptr;
                }, &args);

            std::ostringstream o;
            o << "{\"track\":" << trackIdx
              << ",\"path\":\"" << path << "\""
              << ",\"plugins\":" << args.count
              << ",\"action\":\"save\",\"ok\":" << (args.ok ? "true" : "false");
            if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
            o << "}";
            emitAuto (eventBroker, "patch", o.str());

            if (args.ok)
                std::cout << "[save_patch] " << args.count
                          << " plugin(s) → " << path << "\n" << std::flush;
            else
                std::cerr << "[save_patch error] " << args.err << "\n" << std::flush;
            return args.ok;
        });

    daw.set_function ("load_patch",
        [&edit, &eventBroker](int trackIdx, const std::string& path) -> bool {
            struct Args { te::Edit* edit; int idx; std::string path;
                          bool ok; std::string err; int count; };
            Args args { &edit, trackIdx, path, false, {}, 0 };

            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto  tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { a->err = "track index out of range"; return nullptr; }
                    auto* at = tracks[a->idx - 1];

                    juce::File file (a->path);
                    if (! file.existsAsFile())
                        { a->err = "file not found: " + a->path; return nullptr; }

                    auto xmlDoc = juce::parseXML (file);
                    if (xmlDoc == nullptr)
                        { a->err = "could not parse XML"; return nullptr; }

                    auto state = juce::ValueTree::fromXml (*xmlDoc);
                    if (! state.isValid())
                        { a->err = "invalid ValueTree in file"; return nullptr; }

                    // Register any external-plugin descriptions referenced
                    // by the patch so ExternalPlugin::findMatchingPlugin
                    // can resolve them at graph-init time. (Same trick as
                    // load_plugin — without it, plugins silently fail to
                    // load even when the host has them scanned.)
                    auto& pm = a->edit->engine.getPluginManager();
                    for (auto child : state)
                        if (child.hasProperty (te::IDs::uid))
                        {
                            // Tracktion stores enough in the saved state
                            // for the plugin loader to find the format;
                            // we don't need to do anything fancy here.
                        }

                    at->pluginList.addPluginsFrom (state, true, true);
                    a->count = at->pluginList.size();
                    a->edit->getTransport().ensureContextAllocated (true);
                    a->ok = true;
                    return nullptr;
                }, &args);

            std::ostringstream o;
            o << "{\"track\":" << trackIdx
              << ",\"path\":\"" << path << "\""
              << ",\"plugins\":" << args.count
              << ",\"action\":\"load\",\"ok\":" << (args.ok ? "true" : "false");
            if (! args.ok) o << ",\"error\":\"" << args.err << "\"";
            o << "}";
            emitAuto (eventBroker, "patch", o.str());

            if (args.ok)
                std::cout << "[load_patch] " << args.count
                          << " plugin(s) ← " << path << "\n" << std::flush;
            else
                std::cerr << "[load_patch error] " << args.err << "\n" << std::flush;
            return args.ok;
        });

    // --- plugin parameters ---
    //
    // Lookup is by paramID first (e.g. "filter1.cutoff"), falling back to
    // case-insensitive match on the display name. Operates on the first
    // plugin in the track's chain. (See findFirstPluginParam above.)

    // daw.list_params(track [, filter [, max=50]])
    // filter: case-insensitive substring on either paramID or display name.
    //   daw.list_params(1, "filter")        -- everything matching "filter"
    //   daw.list_params(1, "", 100)         -- first 100, no filter
    //   daw.list_params(1)                  -- first 50, no filter
    daw.set_function ("list_params",
        [&edit](int trackIdx, sol::optional<std::string> filterOpt,
                sol::optional<int> maxOpt) {
            struct Args { te::Edit* edit; int idx; int max; std::string filter; };
            Args args { &edit, trackIdx, maxOpt.value_or (50),
                        filterOpt.value_or (std::string{}) };
            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { std::cout << "(track not found)\n"; return nullptr; }
                    auto* at = tracks[a->idx - 1];
                    if (at->pluginList.size() == 0)
                        { std::cout << "(no plugin on track)\n"; return nullptr; }
                    auto* plug = at->pluginList[0];
                    auto params = plug->getAutomatableParameters();
                    juce::String filt (a->filter);
                    int matched = 0, shown = 0;
                    for (auto* p : params)
                    {
                        if (filt.isNotEmpty()
                            && ! p->paramID.containsIgnoreCase (filt)
                            && ! p->getParameterName().containsIgnoreCase (filt))
                            continue;
                        ++matched;
                        if (shown >= a->max) continue;
                        ++shown;
                        auto r = p->getValueRange();
                        std::cout << "  " << p->paramID.toStdString()
                                  << "  (" << p->getParameterName().toStdString() << ")"
                                  << "  cur=" << p->getCurrentValue()
                                  << "  range=[" << r.getStart() << ", " << r.getEnd() << "]\n";
                    }
                    std::cout << "Track " << a->idx << " — "
                              << plug->getName().toStdString()
                              << " — " << params.size() << " params total";
                    if (filt.isNotEmpty())
                        std::cout << ", " << matched << " matched \"" << a->filter << "\"";
                    if (shown < matched)
                        std::cout << " (showing first " << shown << ")";
                    std::cout << "\n";
                    return nullptr;
                }, &args);
        });

    // daw.get_param(track, name)  → number, or nil if not found
    daw.set_function ("get_param",
        [&edit](int trackIdx, const std::string& name,
                sol::this_state ts) -> sol::object {
            struct Args { te::Edit* edit; int idx; std::string name;
                          bool ok; float value; };
            Args args { &edit, trackIdx, name, false, 0.0f };
            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size()) return nullptr;
                    if (auto* p = findFirstPluginParam (tracks[a->idx - 1], a->name))
                        { a->value = p->getCurrentValue(); a->ok = true; }
                    return nullptr;
                }, &args);
            sol::state_view lv (ts);
            return args.ok ? sol::make_object (lv, args.value) : sol::object (sol::lua_nil);
        });

    // daw.set_param(track, name, value)  → bool
    daw.set_function ("set_param",
        [&edit](int trackIdx, const std::string& name, double value) -> bool {
            struct Args { te::Edit* edit; int idx; std::string name; float value;
                          bool ok; std::string err; };
            Args args { &edit, trackIdx, name, (float) value, false, {} };
            juce::MessageManager::getInstance()->callFunctionOnMessageThread (
                [](void* ctx) -> void* {
                    auto* a = static_cast<Args*> (ctx);
                    auto tracks = te::getAudioTracks (*a->edit);
                    if (a->idx < 1 || a->idx > (int) tracks.size())
                        { a->err = "track index out of range"; return nullptr; }
                    auto* p = findFirstPluginParam (tracks[a->idx - 1], a->name);
                    if (! p) { a->err = "param not found: " + a->name; return nullptr; }
                    p->setParameter (a->value, juce::sendNotification);
                    a->ok = true;
                    return nullptr;
                }, &args);
            if (! args.ok)
                std::cerr << "[set_param error] " << args.err << "\n" << std::flush;
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
            std::cerr << "\n[trigger_follow error] " << err.what() << "\n" << std::flush;
        }
    });
}

// readline's callback API requires C-style free functions, so we set this
// to point at the live LuaRepl just before installing the handler.
class LuaRepl;
static LuaRepl* s_replForReadline = nullptr;

static juce::String readlineHistoryPath()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                .getChildFile (".trakdaw_history").getFullPathName();
}

// readline calls this when a complete line is ready. line == nullptr means EOF.
static void readlineLineCallback (char* line);  // forward; defined after LuaRepl

//==============================================================================
// LuaRepl
//==============================================================================
class LuaRepl : private juce::Thread
{
public:
    LuaRepl (te::Edit& edit, MidiEventQueue& midiQueue,
             EventBroker& eventBroker, PythonHost& pythonHost)
        : juce::Thread ("lua-repl"), edit (edit), midiQueue (midiQueue),
          eventBroker (eventBroker)
    {
        lua.open_libraries (sol::lib::base, sol::lib::string,
                            sol::lib::table,  sol::lib::math,
                            sol::lib::io,     sol::lib::os);

        registerDawApi (lua, edit, midiQueue, midiOutput, pluginEditors,
            eventBroker, scheduler, watch, osc,
            [this](const std::string& path) {
                watch.path  = path;
                watch.mtime = juce::File (path).getLastModificationTime();
                watch.pending = false;
                std::cout << "[watch] monitoring: "
                          << juce::File (path).getFileName() << "\n" << std::flush;
            },
            pythonHost);
    }

    void start()    { startThread(); }
    void shutdown()
    {
        signalThreadShouldExit();
        stopThread (2000);
        osc.stop();
        // Plugin editor windows own live VST editor components — they must
        // be destroyed on the message thread before the plugins themselves.
        juce::MessageManager::getInstance()->callFunctionOnMessageThread (
            [](void* ctx) -> void* {
                static_cast<PluginEditorMap*> (ctx)->clear();
                return nullptr;
            }, &pluginEditors);
    }

    // Called from the readline line-completion callback (which is a C-style
    // free function and can't reach private members directly).
    void onReplLine (const std::string& line)
    {
        if (line == "quit" || line == "exit")
        {
            juce::MessageManager::callAsync ([] {
                juce::JUCEApplicationBase::getInstance()->quit();
            });
            replShouldExit = true;
            return;
        }
        if (! line.empty())
        {
            add_history (line.c_str());
            evalLine (line);
        }
    }

    void onReplEof()    // Ctrl+D
    {
        juce::MessageManager::callAsync ([] {
            juce::JUCEApplicationBase::getInstance()->quit();
        });
        replShouldExit = true;
    }

    // Queue a Lua chunk for execution on the REPL thread. Returns a future
    // that resolves to the formatted result (or error message). Safe to
    // call from any thread; the chunk runs on the REPL thread on its next
    // poll cycle (≤10ms).
    std::future<std::string> evalAsync (std::string code)
    {
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future  = promise->get_future();
        {
            std::lock_guard<std::mutex> lock (evalMutex);
            evalQueue.push ({ std::move (code), promise });
        }
        return future;
    }

private:
    // -----------------------------------------------------------------------
    // Format a sol::object into a string (mirror of printValue, for HTTP).
    // -----------------------------------------------------------------------
    static void formatValue (std::ostringstream& out, sol::object obj)
    {
        switch (obj.get_type())
        {
            case sol::type::nil:     out << "nil"; break;
            case sol::type::boolean: out << (obj.as<bool>() ? "true" : "false"); break;
            case sol::type::number:  out << obj.as<double>(); break;
            case sol::type::string:  out << obj.as<std::string>(); break;
            default:                 out << "(value)"; break;
        }
    }

    // -----------------------------------------------------------------------
    // Drain the eval queue. Called from the REPL poll loop.
    // -----------------------------------------------------------------------
    void drainEvalQueue()
    {
        std::queue<EvalRequest> local;
        {
            std::lock_guard<std::mutex> lock (evalMutex);
            std::swap (local, evalQueue);
        }
        while (! local.empty())
        {
            auto req = std::move (local.front());
            local.pop();

            std::ostringstream out;
            auto r = lua.safe_script ("return " + req.code, sol::script_pass_on_error);
            if (! r.valid())
                r = lua.safe_script (req.code, sol::script_pass_on_error);

            if (! r.valid())
            {
                sol::error err = r;
                out << "error: " << err.what();
            }
            else
            {
                for (int i = 0; i < (int) r.return_count(); ++i)
                {
                    if (i > 0) out << "\t";
                    formatValue (out, r[i]);
                }
            }
            req.promise->set_value (out.str());
        }
    }

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
            dispatchMidiToLua (lua, e, eventBroker);
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

        // Stale scheduler tasks from the previous script version would hold
        // dangling Lua closures — clear before running the reloaded script.
        scheduler.clear();

        auto result = lua.safe_script_file (watch.path, sol::script_pass_on_error);
        if (result.valid())
        {
            std::cout << "[hot-reload] reloaded: "
                      << juce::File (watch.path).getFileName() << "\n" << std::flush;
            std::ostringstream o;
            o << "{\"path\":\"" << watch.path << "\",\"trigger\":\"hot_reload\"}";
            emitAuto (eventBroker, "script_load", o.str());
        }
        else
        {
            sol::error err = result;
            std::cout << "[hot-reload] error: " << err.what() << "\n" << std::flush;
        }
    }

    // -----------------------------------------------------------------------
    // REPL loop
    // -----------------------------------------------------------------------
    void run() override
    {
        std::cout << "\ntrakdaw Lua REPL — readline + history (up arrow recalls)\n";
        std::cout << "  daw.play() / daw.stop()              transport\n";
        std::cout << "  daw.clip(t,s).launch()               launch clip (1-based)\n";
        std::cout << "  daw.note_on(t, note, vel)            play instrument on track t\n";
        std::cout << "  daw.load_script(\"path.lua\")          load + watch for changes\n";
        std::cout << "  daw.store.x = val                    persistent across reloads\n";
        std::cout << "  http://127.0.0.1:8081/               web UI\n";
        std::cout << "  quit / exit / Ctrl+D\n\n";

        // readline takes over stdin: history + line editing + reverse search.
        // Don't let it install signal handlers — JUCE's SIGINT handler should
        // win so Ctrl+C quits the app cleanly.
        rl_catch_signals = 0;
        rl_catch_sigwinch = 0;

        const auto histPath = readlineHistoryPath();
        read_history (histPath.toRawUTF8());            // best-effort; fine if missing
        stifle_history (1000);

        s_replForReadline = this;
        rl_callback_handler_install ("> ", &readlineLineCallback);

        while (! threadShouldExit() && ! replShouldExit)
        {
            fd_set fds;
            FD_ZERO (&fds);
            FD_SET (STDIN_FILENO, &fds);
            struct timeval tv { 0, 10'000 };   // 10ms

            int ready = ::select (STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);

            if (ready < 0 && errno == EINTR)
                continue;

            // Feed every available byte to readline. Each call may complete
            // a line and invoke our callback (which evaluates it). Pasted
            // multi-line blocks fall out of this loop naturally — readline
            // sees the whole buffer and fires the callback once per newline.
            while (ready > 0)
            {
                rl_callback_read_char();
                FD_ZERO (&fds); FD_SET (STDIN_FILENO, &fds);
                struct timeval zero { 0, 0 };
                ready = ::select (STDIN_FILENO + 1, &fds, nullptr, nullptr, &zero);
            }

            drainMidi();
            dispatchOscToLua (lua, osc, eventBroker);
            drainEvalQueue();
            checkFileForChanges();
            reloadIfReady();
            checkFollowActions();
            scheduler.tick();
        }

        rl_callback_handler_remove();
        write_history (histPath.toRawUTF8());
        s_replForReadline = nullptr;
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
                    std::cerr << "\n[on_end error] " << err.what() << "\n" << std::flush;
                }

                std::ostringstream o;
                o << "{\"track\":" << t << ",\"slot\":" << s
                  << ",\"name\":\"" << clip->getName().toStdString() << "\"}";
                emitAuto (eventBroker, "follow", o.str());
            }

            st.prevState = cur;
        });
    }

    // -----------------------------------------------------------------------
    struct EvalRequest
    {
        std::string code;
        std::shared_ptr<std::promise<std::string>> promise;
    };

    te::Edit&                          edit;
    MidiEventQueue&                    midiQueue;
    EventBroker&                       eventBroker;
    sol::state                         lua;
    Scheduler                          scheduler;
    bool                               replShouldExit = false;
    WatchState                         watch;
    OscBridge                          osc;
    std::unique_ptr<juce::MidiOutput>  midiOutput;
    PluginEditorMap                    pluginEditors;
    std::map<std::pair<int,int>, FollowState> followStates;
    std::mutex                         evalMutex;
    std::queue<EvalRequest>            evalQueue;
};

// Defined here (after LuaRepl is complete) because the callback dispatches
// onto a LuaRepl method.
static void readlineLineCallback (char* line)
{
    if (s_replForReadline == nullptr) { if (line) free (line); return; }
    if (line == nullptr)
    {
        s_replForReadline->onReplEof();
        return;
    }
    std::string s (line);
    free (line);
    s_replForReadline->onReplLine (s);
}

//==============================================================================
// HttpServer — exposes POST /eval which runs a Lua chunk on the REPL thread
// and returns its formatted result. Listens on its own thread.
//==============================================================================
class HttpServer : private juce::Thread
{
public:
    HttpServer (LuaRepl& repl, EventBroker& broker, int port)
        : juce::Thread ("http-server"), repl (repl), broker (broker), port (port) {}

    void start() { startThread(); }

    void shutdown()
    {
        signalThreadShouldExit();
        broker.shutdown();   // wake SSE content providers so they return false
        svr.stop();          // unblocks listen()
        stopThread (2000);
    }

private:
    void run() override
    {
        // GET / — built-in web UI (eval box + live event log)
        svr.Get ("/", [] (const httplib::Request&, httplib::Response& res) {
            res.set_content (kIndexHtml, "text/html; charset=utf-8");
        });

        svr.Post ("/eval", [this] (const httplib::Request& req,
                                   httplib::Response& res)
        {
            auto fut = repl.evalAsync (req.body);
            if (fut.wait_for (std::chrono::seconds (5))
                  == std::future_status::ready)
            {
                res.set_content (fut.get(), "text/plain");
            }
            else
            {
                res.status = 504;
                res.set_content ("eval timeout\n", "text/plain");
            }
        });

        // GET /events — Server-Sent Events stream.
        // Clients subscribe with `new EventSource("http://127.0.0.1:8081/events")`
        // in the browser, or `curl -N http://127.0.0.1:8081/events` on the CLI.
        // Server emits `data: {json}\n\n` frames whenever a script calls
        // daw.emit(). A `:heartbeat\n\n` comment is sent every 15s so proxies
        // and load balancers don't drop the idle connection.
        svr.Get ("/events", [this] (const httplib::Request&,
                                    httplib::Response& res)
        {
            auto sub = broker.subscribe();
            res.set_header ("Cache-Control", "no-cache");
            res.set_chunked_content_provider ("text/event-stream",
                [sub] (size_t, httplib::DataSink& sink) -> bool
                {
                    std::unique_lock<std::mutex> lk (sub->mu);
                    sub->cv.wait_for (lk, std::chrono::seconds (15),
                        [&]{ return ! sub->queue.empty() || sub->disconnected; });

                    if (sub->disconnected) return false;

                    if (sub->queue.empty())
                    {
                        lk.unlock();
                        static const char hb[] = ":heartbeat\n\n";
                        return sink.write (hb, sizeof hb - 1);
                    }

                    std::deque<std::string> local;
                    std::swap (local, sub->queue);
                    lk.unlock();

                    for (auto& msg : local)
                    {
                        std::string frame = "data: " + msg + "\n\n";
                        if (! sink.write (frame.data(), frame.size()))
                            return false;
                    }
                    return true;
                });
        });

        std::cout << "[http] listening on http://127.0.0.1:" << port
                  << "  (web UI at /, POST /eval, GET /events)\n" << std::flush;
        svr.listen ("127.0.0.1", port);
    }

    LuaRepl&         repl;
    EventBroker&     broker;
    int              port;
    httplib::Server  svr;
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
        edit->ensureNumberOfAudioTracks (3);
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

        // 7. Event broker (shared between REPL and HTTP server)
        eventBroker = std::make_unique<EventBroker>();

        // 8. Lua REPL
        repl = std::make_unique<LuaRepl> (*edit, midiQueue, *eventBroker, *pythonHost);
        repl->start();

        // 9. HTTP server (POST /eval, GET /events)
        httpServer = std::make_unique<HttpServer> (*repl, *eventBroker, 8081);
        httpServer->start();
    }

    void shutdown() override
    {
        if (httpServer)  { httpServer->shutdown();  httpServer.reset(); }
        if (repl)        { repl->shutdown();        repl.reset(); }
        eventBroker.reset();
        if (pythonHost)  { pythonHost->shutdown();  pythonHost.reset(); }
        g_pyEdit = nullptr;

        for (auto& mi : midiInputs) mi->stop();
        midiInputs.clear();
        midiHandler.reset();

        if (edit)
        {
            edit->getTransport().stop (false, false);

            // Unload plugins explicitly before destroying the Edit so any
            // async cleanup (VST3 host context release, X11 wrapper-window
            // teardown) gets a chance to run on the message thread while
            // it's still pumping. Otherwise on Linux JUCE's leak detector
            // catches a residual VST3HostContextHeadless / RunLoop /
            // SharedResourcePointer / AsyncUpdater on app exit.
            for (auto* at : te::getAudioTracks (*edit))
                at->pluginList.clear();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

            edit.reset();
        }
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
    std::unique_ptr<EventBroker>                  eventBroker;
    std::unique_ptr<LuaRepl>                      repl;
    std::unique_ptr<HttpServer>                   httpServer;
};

//==============================================================================
START_JUCE_APPLICATION (TrakdawApp)
