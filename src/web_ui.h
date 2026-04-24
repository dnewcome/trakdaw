// Embedded index page served at GET /. Minimal by design: eval textarea,
// live event log. Lives here so main.cpp stays readable; edit as HTML.
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
             letter-spacing:3px; font-size:11px; text-transform:uppercase; }
  main     { display:grid; grid-template-columns: 1fr 1fr; gap:16px; padding:16px; height: calc(100vh - 60px); box-sizing: border-box; }
  section  { background:#12151d; border:1px solid #1f2430; display:flex; flex-direction:column; min-height: 0; }
  h2       { margin:0; padding:8px 12px; font-size:10px; letter-spacing:2px; color:#64748b;
             text-transform:uppercase; border-bottom:1px solid #1f2430; }
  textarea { flex:1; min-height: 100px; background:#0b0d12; color:#a5f3fc; border:none; padding:10px;
             font:inherit; resize:none; outline:none; }
  #result  { background:#0b0d12; color:#fde68a; padding:10px; white-space:pre-wrap;
             border-top:1px solid #1f2430; min-height: 24px; max-height: 180px; overflow:auto; }
  .bar     { padding:8px 10px; border-top:1px solid #1f2430; display:flex; gap:8px; align-items:center; }
  button   { background:#0369a1; color:#fff; border:0; padding:6px 14px; font:inherit; cursor:pointer; }
  button:hover { background:#0284c7; }
  .hint    { color:#475569; font-size:11px; }
  #events  { flex:1; overflow:auto; padding:6px 10px; background:#0b0d12; }
  .row     { padding:2px 0; border-bottom:1px solid #11141b; }
  .ts      { color:#475569; margin-right:8px; }
  .name    { color:#86efac; }
  .data    { color:#94a3b8; margin-left:8px; }
  .sys     { color:#475569; font-style:italic; }
  .err     { color:#f87171; }
  .ctl     { display:flex; gap:8px; padding:6px 10px; border-top:1px solid #1f2430; align-items:center; }
  .ctl input { flex:1; background:#0b0d12; color:#d8dee9; border:1px solid #1f2430; padding:4px 8px; font:inherit; }
</style>
</head>
<body>
<header>trakdaw</header>
<main>
  <section>
    <h2>eval (post /eval)</h2>
    <textarea id="code" spellcheck="false" placeholder="daw.bpm()">daw.bpm()</textarea>
    <div class="bar">
      <button id="run">run</button>
      <span class="hint">ctrl/cmd + enter</span>
    </div>
    <pre id="result"></pre>
  </section>
  <section>
    <h2>events (get /events)</h2>
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

  async function run() {
    try {
      const r = await fetch('/eval', { method:'POST', body: $('code').value });
      $('result').textContent = await r.text();
    } catch (e) { $('result').textContent = 'error: ' + e.message; }
  }
  $('run').onclick = run;
  $('code').addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); run(); }
  });

  const logEl = $('events');
  const filterEl = $('filter');
  function append(html, cls='') {
    const d = document.createElement('div');
    d.className = 'row' + (cls ? ' ' + cls : '');
    d.innerHTML = html;
    if (filterEl.value && !d.textContent.includes(filterEl.value)) d.style.display='none';
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
  }
  $('clear').onclick = () => { logEl.innerHTML=''; };
  filterEl.oninput = () => {
    const q = filterEl.value;
    for (const row of logEl.children)
      row.style.display = (!q || row.textContent.includes(q)) ? '' : 'none';
  };

  function connect() {
    const es = new EventSource('/events');
    es.onopen  = () => append('connected to /events', 'sys');
    es.onerror = () => append('/events disconnected — reconnecting…', 'err');
    es.onmessage = e => {
      let obj; try { obj = JSON.parse(e.data); } catch { append(esc(e.data)); return; }
      const ts = new Date().toLocaleTimeString();
      const d  = obj.data !== undefined ? '<span class="data">'+esc(JSON.stringify(obj.data))+'</span>' : '';
      append('<span class="ts">'+ts+'</span><span class="name">'+esc(obj.name)+'</span>'+d);
    };
  }
  connect();
})();
</script>
</body>
</html>
)HTML";
