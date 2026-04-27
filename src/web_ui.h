// Embedded index page served at GET /. Minimal by design: live clip grid,
// Lua eval box, event log. Lives here so main.cpp stays readable; edit as HTML.
#pragma once

static constexpr const char* kIndexHtml = R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>trakdaw</title>
<style>
  html,body { margin:0; padding:0; background:#0b0d12; color:#d8dee9;
              font-family: ui-monospace, Menlo, Consolas, monospace; font-size:13px; }
  header   { padding:14px 20px; border-bottom:1px solid #1f2430; color:#7dd3fc;
             letter-spacing:3px; font-size:11px; text-transform:uppercase;
             display:flex; justify-content:space-between; align-items:center; }
  header .meta { color:#475569; font-size:11px; letter-spacing:1px; }
  main     { display:grid; grid-template-columns: 1fr 1fr;
             grid-template-rows: auto fit-content(220px) 1fr;
             gap:16px; padding:16px;
             height: calc(100vh - 46px); box-sizing: border-box; }
  #tracks-panel { min-height: 0; }
  section  { background:#12151d; border:1px solid #1f2430; display:flex;
             flex-direction:column; min-height:0; overflow:hidden; }
  #grid-panel { grid-column: 1 / -1; }
  h2       { margin:0; padding:8px 12px; font-size:10px; letter-spacing:2px;
             color:#64748b; text-transform:uppercase;
             border-bottom:1px solid #1f2430;
             display:flex; justify-content:space-between; }
  h2 .sub  { color:#334155; letter-spacing:1px; }

  #grid    { display:grid; gap:6px; padding:12px; }
  .cell    { background:#0b0d12; border:1px solid #1f2430; padding:10px;
             min-height:54px; font-size:11px; color:#475569;
             transition: background 120ms, border-color 120ms, color 120ms;
             display:flex; flex-direction:column; justify-content:space-between; }
  .cell .name { color:#64748b; font-size:12px; word-break: break-all; }
  .cell .addr { opacity:0.6; font-size:10px; letter-spacing:1px; }
  .cell.on { background:#052e25; border-color:#10b981; color:#a7f3d0; }
  .cell.on .name { color:#d1fae5; }
  .cell.flash { background:#7c2d12 !important; border-color:#fb923c !important; }

  #tracks  { padding:10px; overflow:auto; }
  .track   { display:flex; gap:14px; padding:8px 0; border-bottom:1px solid #11141b;
             align-items:center; flex-wrap:wrap; }
  .track:last-child { border-bottom:none; }
  .track-idx { color:#7dd3fc; font-size:11px; letter-spacing:2px; min-width:34px; }
  .track-name { color:#d8dee9; font-size:13px; min-width:80px; }
  .pchain  { display:flex; gap:6px; flex-wrap:wrap; }
  .plug    { background:#12222e; border:1px solid #1e3a4d; padding:3px 8px;
             font-size:11px; color:#a5f3fc; border-radius:2px; }
  .plug.builtin { background:#1f2430; color:#94a3b8; border-color:#1f2430; }
  .plug.error { background:#451a1a; border-color:#7f1d1d; color:#fca5a5; }
  .plug-format { color:#475569; margin-left:6px; font-size:10px; }
  .empty   { color:#475569; font-style:italic; font-size:11px; }
  .clipdot { width:8px; height:8px; border-radius:50%; background:#1f2430;
             display:inline-block; margin-right:3px; }
  .clipdot.has  { background:#475569; }
  .clipdot.on   { background:#10b981; box-shadow:0 0 6px #10b981; }

  textarea { flex:1; min-height:80px; background:#0b0d12; color:#a5f3fc;
             border:none; padding:10px; font:inherit; resize:none; outline:none; }
  #result  { background:#0b0d12; color:#fde68a; padding:10px; white-space:pre-wrap;
             border-top:1px solid #1f2430; min-height:24px; max-height:180px; overflow:auto; }
  .bar     { padding:8px 10px; border-top:1px solid #1f2430;
             display:flex; gap:8px; align-items:center; }
  button   { background:#0369a1; color:#fff; border:0; padding:6px 14px;
             font:inherit; cursor:pointer; }
  button:hover { background:#0284c7; }
  .hint    { color:#475569; font-size:11px; }

  #events  { flex:1; overflow:auto; padding:6px 10px; background:#0b0d12; }
  .row     { padding:2px 0; border-bottom:1px solid #11141b; }
  .ts      { color:#475569; margin-right:8px; }
  .name    { color:#86efac; }
  .data    { color:#94a3b8; margin-left:8px; }
  .sys     { color:#475569; font-style:italic; }
  .err     { color:#f87171; }
  .ctl     { display:flex; gap:8px; padding:6px 10px;
             border-top:1px solid #1f2430; align-items:center; }
  .ctl input { flex:1; background:#0b0d12; color:#d8dee9;
               border:1px solid #1f2430; padding:4px 8px; font:inherit; }
</style>
</head>
<body>
<header>
  <span>trakdaw</span>
  <span class="meta"><span id="transport">stopped</span> · bpm <span id="bpm">—</span><span id="watching"></span></span>
</header>
<main>
  <section id="grid-panel">
    <h2>clip grid <span class="sub" id="grid-dims">—</span></h2>
    <div id="grid"></div>
  </section>
  <section id="tracks-panel" style="grid-column: 1 / -1;">
    <h2>tracks <span class="sub" id="tracks-meta">—</span></h2>
    <div id="tracks"></div>
  </section>
  <section>
    <h2>eval <span class="sub">post /eval</span></h2>
    <textarea id="code" spellcheck="false" placeholder="daw.bpm()">daw.bpm()</textarea>
    <div class="bar">
      <button id="run">run</button>
      <span class="hint">ctrl/cmd + enter — selection only if any, else whole buffer</span>
    </div>
    <pre id="result"></pre>
  </section>
  <section>
    <h2>events <span class="sub">get /events</span></h2>
    <div id="events"></div>
    <div class="ctl">
      <input id="filter" placeholder="filter by name substring…">
      <button id="clear">clear</button>
    </div>
  </section>
</main>
<script>
(() => {
  const $ = id => document.getElementById(id);
  const esc = s => String(s).replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));

  // --- eval ---
  // Send the current selection if there is one, otherwise the whole buffer.
  // Lets you keep a scratch sheet of snippets in the textarea and fire any
  // chunk by selecting it.
  async function run() {
    const ta = $('code');
    const sel = ta.value.substring(ta.selectionStart, ta.selectionEnd);
    const code = sel.length > 0 ? sel : ta.value;
    try {
      const r = await fetch('/eval', { method:'POST', body: code });
      $('result').textContent = await r.text();
    } catch (e) { $('result').textContent = 'error: ' + e.message; }
  }
  $('run').onclick = run;
  $('code').addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); run(); }
  });

  // --- event log ---
  const logEl = $('events'), filterEl = $('filter');
  function append(html, cls='') {
    const d = document.createElement('div');
    d.className = 'row' + (cls ? ' ' + cls : '');
    d.innerHTML = html;
    if (filterEl.value && !d.textContent.includes(filterEl.value)) d.style.display='none';
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
    while (logEl.children.length > 500) logEl.removeChild(logEl.firstChild);
  }
  $('clear').onclick = () => { logEl.innerHTML=''; };
  filterEl.oninput = () => {
    const q = filterEl.value;
    for (const row of logEl.children)
      row.style.display = (!q || row.textContent.includes(q)) ? '' : 'none';
  };

  // --- clip grid ---
  // Default 2x2 (matches current trakdaw edit); grows if events arrive with
  // higher track/slot indices. Lua-as-query means we don't fetch initial
  // state — cells start blank and populate as events flow.
  const grid = { tracks:2, slots:2, cells:{} };

  function key(t,s) { return t + ':' + s; }
  function cellEl(t,s) { return document.getElementById('cell-' + key(t,s)); }

  function rebuildGrid() {
    const el = $('grid');
    el.style.gridTemplateColumns = 'repeat(' + grid.slots + ', 1fr)';
    el.innerHTML = '';
    for (let t = 1; t <= grid.tracks; t++) {
      for (let s = 1; s <= grid.slots; s++) {
        const d = document.createElement('div');
        d.className = 'cell';
        d.id = 'cell-' + key(t,s);
        el.appendChild(d);
        updateCell(t, s);
      }
    }
    $('grid-dims').textContent = grid.tracks + ' × ' + grid.slots;
  }

  function updateCell(t, s) {
    const d = cellEl(t, s); if (!d) return;
    const c = grid.cells[key(t,s)] || {};
    d.classList.toggle('on', !!c.playing);
    d.innerHTML = '<div class="name">' + esc(c.name || '—') + '</div>'
                + '<div class="addr">t' + t + ' · s' + s + '</div>';
  }

  function handleClipEvent(name, data) {
    if (!data || !data.track || !data.slot) return;
    const t = data.track, s = data.slot, k = key(t,s);
    let grew = false;
    if (t > grid.tracks) { grid.tracks = t; grew = true; }
    if (s > grid.slots)  { grid.slots  = s; grew = true; }
    grid.cells[k] = grid.cells[k] || {};
    if (name === 'clip_launch') {
      grid.cells[k].playing = true;
      if (data.name) grid.cells[k].name = data.name;
    } else if (name === 'clip_stop') {
      grid.cells[k].playing = false;
    }
    if (grew) rebuildGrid();
    else      updateCell(t, s);

    if (name === 'follow') {
      const d = cellEl(t, s);
      if (d) { d.classList.add('flash'); setTimeout(() => d.classList.remove('flash'), 400); }
    }
  }

  rebuildGrid();

  // --- SSE connection ---
  // Events that imply track/plugin/clip/transport state may have changed
  // — when one fires, refresh the tracks panel.
  const REFRESH_EVENTS = new Set([
    'plugin_load', 'plugin_editor', 'transport', 'bpm', 'track_add',
    'midi_input_route', 'patch', 'script_load',
    'clip_launch', 'clip_stop', 'follow',
    'clip_create', 'clip_clear', 'reset', 'unwatch',
  ]);

  function connect() {
    const es = new EventSource('/events');
    es.onopen  = () => append('connected to /events', 'sys');
    es.onerror = () => append('/events disconnected — reconnecting…', 'err');
    es.onmessage = e => {
      let obj; try { obj = JSON.parse(e.data); } catch { append(esc(e.data)); return; }
      if (obj.name === 'clip_launch' || obj.name === 'clip_stop' || obj.name === 'follow')
        handleClipEvent(obj.name, obj.data);
      if (obj.name === 'transport' && obj.data)
        $('transport').textContent = obj.data.playing ? 'playing' : 'stopped';
      if (REFRESH_EVENTS.has(obj.name)) refreshState();
      const ts = new Date().toLocaleTimeString();
      const d  = obj.data !== undefined ? '<span class="data">'+esc(JSON.stringify(obj.data))+'</span>' : '';
      append('<span class="ts">'+ts+'</span><span class="name">'+esc(obj.name)+'</span>'+d);
    };
  }
  connect();

  // --- tracks panel: refresh from daw.state() on load and on relevant events ---
  // Lua-as-query: the engine doesn't push a state schema; we ask for one.
  async function refreshState() {
    try {
      const r = await fetch('/eval', { method:'POST', body:'return daw.state()' });
      const txt = await r.text();
      const st = JSON.parse(txt);
      renderTracks(st);
      $('bpm').textContent = String(Math.round(st.bpm));
      $('transport').textContent = st.playing ? 'playing' : 'stopped';
      const w = $('watching');
      if (st.watching) {
        const name = st.watching.replace(/^.*[\\/]/, '');
        w.textContent = ' · watching ' + name;
        w.title = st.watching;
      } else {
        w.textContent = '';
        w.title = '';
      }
      $('tracks-meta').textContent =
        st.tracks.length + ' tracks · bar ' + st.bar +
        ' · beat ' + st.beat.toFixed(2);

      // Sync the clip grid's bounds to the authoritative state. The grid
      // also auto-grows from individual clip events, but track_add doesn't
      // emit a per-clip event, so without this the grid stays at 3 rows
      // after daw.add_track().
      const tcount = st.tracks.length;
      const scount = (st.tracks[0] && st.tracks[0].clips.length) || grid.slots;
      let grew = false;
      if (tcount > grid.tracks) { grid.tracks = tcount; grew = true; }
      if (scount > grid.slots)  { grid.slots  = scount; grew = true; }
      if (grew) rebuildGrid();
    } catch (e) { /* server may not be ready yet on first paint */ }
  }

  function renderTracks(st) {
    const root = $('tracks');
    root.innerHTML = '';
    for (const t of st.tracks) {
      const row = document.createElement('div');
      row.className = 'track';
      const idx = '<span class="track-idx">T' + t.index + '</span>';
      const nm  = '<span class="track-name">' + esc(t.name || '') + '</span>';
      let chain = '<div class="pchain">';
      if (!t.plugins.length) {
        chain += '<span class="empty">no plugin</span>';
      } else {
        for (const p of t.plugins) {
          const cls = p.error ? 'plug error'
                    : (p.format === 'builtin' ? 'plug builtin' : 'plug');
          chain += '<span class="' + cls + '">' + esc(p.name)
                +  '<span class="plug-format">' + esc(p.format) + '</span>'
                +  '</span>';
        }
      }
      chain += '</div>';
      let dots = '<div class="pchain">';
      for (const c of t.clips) {
        const cls = c.playing ? 'clipdot on' : (c.name ? 'clipdot has' : 'clipdot');
        dots += '<span class="' + cls + '" title="slot ' + c.slot
              + (c.name ? ': ' + esc(c.name) : '') + '"></span>';
      }
      dots += '</div>';
      row.innerHTML = idx + nm + chain + dots;
      root.appendChild(row);
    }
  }

  refreshState();
})();
</script>
</body>
</html>
)HTML";
