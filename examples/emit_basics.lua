-- emit_basics.lua — pushes a handful of events to the /events stream so
-- they show up in the browser event log and any other SSE subscriber.
--
-- Run from the REPL:
--   daw.load_script("examples/emit_basics.lua")
-- Open http://127.0.0.1:8081/ first so you can watch them land.

print("[emit_basics] emitting a variety of payloads...")

-- bare event, no data
daw.emit("hello")

-- flat object
daw.emit("status", { healthy = true, note = "all good" })

-- nested object
daw.emit("user_action", {
    kind    = "test",
    payload = { x = 10, y = 20, from = "emit_basics" },
})

-- array (all keys are 1..N → serialized as a JSON array)
daw.emit("chord", { 60, 64, 67, 72 })

-- strings with characters that need JSON-escaping
daw.emit("quoted", { msg = [[he said "hi" and left\n]] })

print("[emit_basics] done")
