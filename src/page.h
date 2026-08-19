#pragma once

#include <Arduino.h>

// The two pages, served from flash. Self-contained by choice: they are opened from a phone
// standing in front of a shutter, to check whether the bridge or the motor is the problem,
// and a CDN round trip is the one thing that would make that hang.
//
// **The split is deliberate.** The operation page does what Home Assistant does — up, stop,
// down, and what state we believe a shutter is in — and needs no password, because a page
// that asks for one before it will tell you whether a shutter is shut is a page nobody
// opens. Settings is the admin surface: pairing, adding and removing remotes. The browser
// asks for a password the moment that page is opened.
//
// Controls is a sibling of settings, not a section of it: same password, different job —
// this one is used walking around a house with a phone. Behind the password because the
// addresses on it are the credentials of real motors.

static const char PAGE_CSS[] PROGMEM = R"CSS(
:root{--bg:#0e1116;--card:#171c24;--line:#262d38;--fg:#e6eaf0;--dim:#8b95a5;
--accent:#4da3ff;--ok:#3ddc84;--warn:#f0b429;--bad:#ff6b6b}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);
font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
padding-bottom:calc(16px + env(safe-area-inset-bottom))}
main{max-width:520px;margin:0 auto}
h1{font-size:15px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim);
margin:0 0 14px;font-weight:600;display:flex;align-items:center;gap:10px}
h1 a{margin-left:auto;font-size:12px;letter-spacing:.06em;text-transform:none;
color:var(--accent);text-decoration:none}
h2{font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:var(--dim);
margin:0 0 10px;font-weight:600}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;
padding:14px;margin-bottom:12px}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:4px 14px}
.grid div{display:flex;justify-content:space-between;gap:8px;
border-bottom:1px solid var(--line);padding:5px 0;font-size:14px}
.grid span{color:var(--dim)}
.hdr{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.hdr b{font-size:17px}
.badge{margin-left:auto;font-size:12px;color:var(--dim);border:1px solid var(--line);
border-radius:999px;padding:2px 10px;white-space:nowrap}
.badge.open{color:var(--ok);border-color:var(--ok)}
.badge.closed{color:var(--accent);border-color:var(--accent)}
.badge.blocked{color:var(--warn);border-color:var(--warn)}
.badge.opening,.badge.closing{color:var(--fg);border-color:var(--fg);
animation:pulse 1.4s ease-in-out infinite}
@keyframes pulse{50%{opacity:.45}}
.row{display:flex;gap:8px}
input{flex:1;min-height:46px;border-radius:11px;border:1px solid var(--line);
background:#1e242e;color:var(--fg);padding:0 12px;font-size:14px}
button{flex:1;min-height:46px;border:1px solid var(--line);border-radius:11px;
background:#1e242e;color:var(--fg);font-size:15px;font-weight:600;cursor:pointer;
-webkit-tap-highlight-color:transparent;transition:transform .08s,background .15s}
button:active{transform:scale(.97)}
button:disabled{opacity:.4;cursor:not-allowed}
button.pri{background:var(--accent);border-color:var(--accent);color:#04121f}
button.prog{flex:0 0 84px;font-size:12px;font-weight:600;color:var(--warn);
border-color:#3a3320;background:#1d1a12}
button.danger{flex:0 0 92px;font-size:12px;color:var(--bad);border-color:#3a2020;
background:#1d1212}
.meta{color:var(--dim);font-size:12px;margin-top:8px;
font-family:ui-monospace,Menlo,Consolas,monospace}
#log{margin:0;padding:10px;background:#0b0e13;border:1px solid var(--line);
border-radius:10px;max-height:40vh;overflow:auto;font:11px/1.5 ui-monospace,Menlo,
Consolas,monospace;color:var(--dim);white-space:pre-wrap;word-break:break-word}
footer{color:var(--dim);font-size:12px;text-align:center;margin-top:18px}
footer a{color:var(--accent);text-decoration:none}
.stale{opacity:.45}
.warn{color:var(--warn);font-size:13px;margin:0 0 10px}
h2.sect{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);
margin:20px 0 8px;display:flex;align-items:center;gap:8px}
h2.sect .badge{font-weight:400}

/* One bordered container with dividers, so it reads as a list rather than a run of cards. */
.list{border:1px solid var(--line);border-radius:12px;background:var(--card);overflow:hidden}
.list:empty{display:none}
.item+.item{border-top:1px solid var(--line)}
.row2{display:flex;align-items:center;gap:12px;padding:13px 14px;cursor:pointer;
-webkit-tap-highlight-color:transparent}
.row2:active{background:#1b212b}
.chev{flex:0 0 10px;color:var(--dim);font-size:12px;line-height:1;
transition:transform .12s ease}
.item.open>.row2>.chev{transform:rotate(90deg);color:var(--accent)}
.title{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
font-size:15px}
.sub{flex:0 0 auto;color:var(--dim);font-size:12px;
font-variant-numeric:tabular-nums;font-family:ui-monospace,Menlo,Consolas,monospace}

/* An open row is lifted out of the list: accent edge, its own ground, and the editor on a
   panel of its own so the boundary is unmistakable. */
.item.open{background:#151b24;border-left:3px solid var(--accent)}
.item.open>.row2{padding-left:11px}
.edit{padding:14px;background:#0f141b;border-top:1px solid var(--line)}
.edit label.f{display:block;font-size:11px;text-transform:uppercase;letter-spacing:.06em;
color:var(--dim);margin:0 0 6px}
.edit input[type=text]{width:100%}
.count{text-align:right;font-size:11px;color:var(--dim);margin-top:4px;
font-variant-numeric:tabular-nums}
.empty{color:var(--dim);font-size:13px;padding:14px;border:1px dashed var(--line);
border-radius:12px;text-align:center}
button.link{background:none;border:none;color:var(--accent);font-size:12px;padding:0;
cursor:pointer;flex:0 0 auto;width:auto}
.ticks{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
)CSS";

// --- operation ---------------------------------------------------------------------------
//
// What Home Assistant exposes and nothing more: up, stop, down, and the state we inferred
// from what we sent. Nothing here can create, destroy or re-address anything, which is why
// it needs no password.
static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>somfy-remote</title>
<link rel="stylesheet" href="/style.css"></head><body><main>
<h1>somfy-remote <a href="/settings">settings</a></h1>

<div class="card">
  <div class="grid">
    <div><span>Host</span><b id="host">&mdash;</b></div>
    <div><span>IP</span><b id="ip">&mdash;</b></div>
    <div><span>Signal</span><b id="rssi">&mdash;</b></div>
    <div><span>Uptime</span><b id="uptime">&mdash;</b></div>
    <div><span>Heap</span><b id="heap">&mdash;</b></div>
    <div><span>Queued</span><b id="pending">&mdash;</b></div>
  </div>
</div>

<div class="card" id="unconfigured" style="display:none">
  <h2>Not configured</h2>
  <p class="warn">This board has no configuration yet. It is waiting for a retained config
  document from Home Assistant, and controls nothing until one arrives.</p>
</div>

<div id="remotes"></div>

<div class="card">
  <h2>Console</h2>
  <pre id="log">&hellip;</pre>
</div>

<footer><a href="/status">status</a> &middot; <a href="/log">log</a> &middot;
<a href="/errors">faults</a> &middot; <a href="/settings">settings</a></footer>
</main>
<script>
const $ = id => document.getElementById(id);
let built = 0;

function hms(s){const h=s/3600|0,m=(s/60|0)%60;return h?`${h}h ${m}m`:`${m}m ${s%60}s`}

// Rebuilt only when the number of remotes changes, so a poll does not throw away the
// button somebody is in the middle of pressing.
function build(n){
  $('remotes').innerHTML = Array.from({length:n},(_,i)=>`
    <div class="card">
      <div class="hdr"><b id="n${i}">Remote ${i}</b><span class="badge" id="p${i}">&mdash;</span></div>
      <div class="row">
        <button class="pri" id="u${i}" onclick="send(${i},'Up')">Up</button>
        <button id="m${i}" onclick="send(${i},'My')">My</button>
        <button class="pri" id="d${i}" onclick="send(${i},'Down')">Down</button>
      </div>
    </div>`).join('');
  built = n;
}

function render(s){
  $('host').textContent = s.host;
  $('ip').textContent = s.ip;
  $('rssi').textContent = s.rssi + ' dBm';
  $('uptime').textContent = hms(s.uptime);
  $('heap').textContent = (s.heap/1024).toFixed(1) + ' KB';
  $('pending').textContent = s.pending;
  $('unconfigured').style.display = s.configured ? 'none' : '';
  if (s.remotes.length !== built) build(s.remotes.length);
  for (const r of s.remotes){
    // The name comes from Home Assistant. The index stays visible because it is what
    // everything here — topics, entity ids, rolling codes — is actually keyed on.
    $('n' + r.n).textContent = r.name ? `${r.name} (${r.n})` : `Remote ${r.n}`;
    const b = $('p' + r.n);
    const where = r.pct < 0 ? r.position : `${r.position} ${r.pct}%`;
    b.textContent = r.ready ? where : (r.enabled ? 'not operational' : 'removed');
    b.className = 'badge ' + (r.ready ? (r.position === 'unknown' ? '' : r.position)
                                      : 'blocked');
    // A shutter the firmware refuses to drive gets dead buttons rather than live ones
    // that silently do nothing.
    for (const p of ['u','m','d']) $(p + r.n).disabled = !r.ready;
  }
  document.body.classList.remove('stale');
}

async function poll(){
  try { render(await (await fetch('/api/state')).json()); }
  catch(e) { document.body.classList.add('stale'); }
}

async function send(n, command){
  try {
    const r = await fetch(`/api/send?remote=${n}&command=${command}`, {method:'POST'});
    if (!r.ok) { alert(await r.text()); return; }
    render(await r.json());
  } catch(e) { document.body.classList.add('stale'); }
}

async function loadLog(){
  try { $('log').textContent = await (await fetch('/log')).text(); }
  catch(e) {}
}

poll(); loadLog();
setInterval(poll, 2000);
setInterval(loadLog, 10000);
</script></body></html>
)HTML";

// --- settings ----------------------------------------------------------------------------
//
// Everything that changes what the bridge *is*, rather than what it is doing. The browser
// has already asked for the password by the time this is served.
static const char SETTINGS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>somfy-remote &middot; settings</title>
<link rel="stylesheet" href="/style.css"></head><body><main>
<h1>settings <a href="/">operation</a></h1>

<div class="card">
  <h2>Bridge</h2>
  <div class="grid">
    <div><span>Remotes</span><b id="count">&mdash;</b></div>
    <div><span>Config epoch</span><b id="epoch">&mdash;</b></div>
    <div><span>Store</span><b id="store">&mdash;</b></div>
    <div><span>Free slots</span><b id="free">&mdash;</b></div>
  </div>
  <p class="meta" id="note"></p>
</div>

<div class="card">
  <h2>Add a remote</h2>
  <p class="warn">Added remotes start <b>not operational</b>, so nothing can transmit for
  one until it has been paired and switched on deliberately.</p>
  <div class="row">
    <input id="addr" maxlength="8" placeholder="address, optional (e.g. 0x000000)">
    <button style="flex:0 0 92px" onclick="addRemote()">Add</button>
  </div>
</div>

<div id="remotes"></div>

<footer><a href="/">operation</a> &middot; <a href="/controls">controls</a> &middot;
<a href="/status">status</a> &middot; <a href="/errors">faults</a></footer>
</main>
<script>
const $ = id => document.getElementById(id);
let built = 0;

function build(n){
  $('remotes').innerHTML = Array.from({length:n},(_,i)=>`
    <div class="card">
      <div class="hdr"><b id="n${i}">Remote ${i}</b><span class="badge" id="p${i}">&mdash;</span></div>
      <div class="meta" id="c${i}"></div>
      <div class="row" style="margin-top:10px">
        <label class="f" for="t${i}">Travel</label>
        <input type="number" id="t${i}" min="1" max="255" step="1" style="width:5.5em"
               onchange="setTravel(${i})">
        <span class="meta">seconds, fully open to fully shut</span>
      </div>
      <div class="row" style="margin-top:10px">
        <button id="o${i}" onclick="setOperational(${i})">&mdash;</button>
        <button class="prog" onclick="prog(${i})">Prog</button>
        <button class="danger" id="e${i}" onclick="setEnabled(${i})">&mdash;</button>
      </div>
    </div>`).join('');
  built = n;
}

function render(s){
  $('count').textContent = s.remotes.length;
  $('epoch').textContent = s.epoch;
  $('store').textContent = 'sector ' + s.store;
  $('free').textContent = s.free;
  $('note').textContent = s.degraded ? 'STORE DEGRADED — see faults' : '';
  if (s.remotes.length !== built) build(s.remotes.length);
  for (const r of s.remotes){
    $('n' + r.n).textContent = r.name ? `${r.name} (${r.n})` : `Remote ${r.n}`;
    const b = $('p' + r.n);
    b.textContent = !r.enabled ? 'removed'
                  : (r.operational ? 'operational' : 'not operational');
    b.className = 'badge ' + (r.enabled && r.operational ? 'open' : 'blocked');
    $('c' + r.n).textContent = `next code ${r.code}`;
    // Never over a value being typed: a two-second poll would eat every keystroke.
    const t = $('t' + r.n);
    if (document.activeElement !== t) t.value = r.travel;
    // The buttons say what they will do, not what the state is.
    const op = $('o' + r.n);
    op.textContent = r.operational ? 'Set not operational' : 'Set operational';
    op.className = r.operational ? '' : 'pri';
    const en = $('e' + r.n);
    en.textContent = r.enabled ? 'Remove' : 'Restore';
    en.className = r.enabled ? 'danger' : 'prog';
  }
}

async function poll(){
  try { last = await (await fetch('/api/state')).json(); render(last); } catch(e) {}
}

// How long the motor takes end to end. Everything the device reports about where a shutter
// is comes from this number and a clock, so a wrong one is a cover that reads wrong.
async function setTravel(n){
  const el = $('t' + n);
  const r = await fetch(`/api/remote/flags?remote=${n}&travel=${encodeURIComponent(el.value)}`,
                        {method:'POST'});
  if (!r.ok) { alert('NOT saved: ' + (await r.text())); poll(); return; }
  el.blur();
  poll();
}

// Prog lives here rather than on the operation page because it is the one press that
// changes something permanent: held down, it enrols or drops this emulated remote at a
// motor.
async function prog(n){
  if (!confirm(`Send Prog to remote ${n}? This pairs or unpairs a motor.`)) return;
  const r = await fetch(`/api/prog?remote=${n}`, {method:'POST'});
  alert(await r.text());
  poll();
}

async function addRemote(){
  if (!confirm('Add a remote?')) return;
  const a = encodeURIComponent($('addr').value.trim());
  const r = await fetch(`/api/remote/add?address=${a}`, {method:'POST'});
  alert(await r.text());
  $('addr').value = '';
  poll();
}

// Not operational means the firmware refuses to transmit for this remote at all — from
// Home Assistant, from this page, from anywhere. It is for a shutter that is known not to
// work, so the rule is enforced rather than remembered.
let last = null;
async function setOperational(n){
  const r0 = last && last.remotes.find(r => r.n === n);
  const on = r0 && r0.operational ? '0' : '1';
  if (on === '1' && !confirm(`Allow remote ${n} to be driven again?`)) return;
  const r = await fetch(`/api/remote/flags?remote=${n}&operational=${on}`, {method:'POST'});
  alert(await r.text());
  poll();
}

// Removing keeps the index and the rolling code; only the Home Assistant entities go.
// Restoring brings them back with the counter where it left off, which is why the index is
// never reused for anything else.
async function setEnabled(n){
  const r0 = last && last.remotes.find(r => r.n === n);
  const on = r0 && r0.enabled ? '0' : '1';
  if (on === '0' && !confirm(`Remove remote ${n}? Its Home Assistant entities go away. `
      + `The rolling code is kept, so restoring it resumes where it left off.`)) return;
  const r = await fetch(`/api/remote/flags?remote=${n}&enabled=${on}`, {method:'POST'});
  alert(await r.text());
  poll();
}

poll();
setInterval(poll, 3000);
</script></body></html>
)HTML";

// Press a control anywhere in the house and its row arrives at the top within a second.
// No per-control mode to arm, so nothing can be pointed at the wrong thing; the two sort
// orders support naming as you walk, or walking first and naming the list afterwards.
static const char CONTROLS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>somfy-remote &middot; controls</title>
<link rel="stylesheet" href="/style.css"></head><body><main>
<h1>controls <a href="/">operation</a></h1>

<div class="card">
  <div class="hdr"><b id="learnTitle">Add a control</b><span class="badge" id="state">&mdash;</span></div>
  <p class="meta" id="learnHelp">Press the button below, then press the control you want to
  add. Anything already in the lists is recognised without this.</p>
  <div class="row" style="margin-top:10px">
    <button id="learnBtn" onclick="learn()">Learn a new control</button>
  </div>
  <p class="meta" id="rx"></p>
</div>

<h2 class="sect">Not named yet <span class="badge" id="n">0</span>
  <button class="link" id="sortbtn" onclick="flip()">newest first</button></h2>
<div id="unknown" class="list"></div>
<p class="empty" id="empty">Nothing waiting. Use <b>Learn a new control</b> to add one.</p>

<h2 class="sect">Named <span class="badge" id="kn">0</span></h2>
<div id="known" class="list"></div>
<p class="empty" id="emptyKnown">None yet.</p>

<footer><a href="/settings">settings</a> &middot; <a href="/status">status</a> &middot;
<a href="/errors">faults</a></footer>
</main>
<script>
const $ = id => document.getElementById(id);
const CMD = {1:'My',2:'Up',3:'My+Up',4:'Down',5:'My+Down',6:'Up+Down',8:'Prog',9:'Sun',10:'Flag'};
const NAME_MAX = 39;

let names = [], newest = true, open = null, last = null, sig = '';

const esc = t => String(t).replace(/[&<>"']/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

const ago = ms => ms < 0 ? 'never'
  : ms < 2000 ? 'just now'
  : ms < 60000 ? Math.round(ms/1000) + 's ago'
  : ms < 3600000 ? Math.round(ms/60000) + 'm ago' : Math.round(ms/3600000) + 'h ago';

function editor(a, name, d, unknown){
  return `<div class="edit" onclick="event.stopPropagation()">
    <label class="f" for="nm${a}">Name</label>
    <input type="text" id="nm${a}" maxlength="${NAME_MAX}" placeholder="e.g. Gallery remote, right"
           value="${esc(name)}" oninput="counter(${a})">
    <div class="count" id="cc${a}"></div>
    <label class="f" style="margin-top:12px">Shutters it drives</label>
    <div class="ticks">${names.map((n,i)=>`<label class="tick"><input type="checkbox"
      data-a="${a}" value="${i}" ${d>>i & 1 ? 'checked' : ''}>${esc(n||('Remote '+i))}</label>`).join('')}</div>
    <div class="row" style="margin-top:14px">
      <button onclick="save(${a})">Save</button>
      <button class="danger" onclick="${unknown?'drop':'forget'}(${a})">${unknown?'Ignore':'Forget'}</button>
      <button class="link" style="margin-left:auto" onclick="shut()">Cancel</button>
    </div></div>`;
}

function counter(a){
  const el = $('nm'+a); if (!el) return;
  $('cc'+a).textContent = el.value.length + '/' + NAME_MAX;
}

function row(a, title, sub, body){
  return `<div class="item${open===a?' open':''}">
    <div class="row2" onclick="pick(${a})">
      <span class="chev">&#9656;</span>
      <span class="title">${title}</span>
      <span class="sub">${sub}</span>
    </div>${open===a ? body : ''}</div>`;
}

function draw(s){
  if (!s) return;
  last = s;

  $('state').textContent = s.learning ? s.learnLeft + 's' : (s.listening ? 'listening' : 'backing off');
  $('state').className = 'badge ' + (s.learning ? 'ok' : '');
  $('learnBtn').textContent = s.learning ? 'Waiting for a press…' : 'Learn a new control';
  $('learnBtn').disabled = s.learning;
  $('rx').textContent = `${s.frames} frames · ${s.presses} presses`
    + (s.ignored ? ` · ${s.ignored} from controls not on the list` : '')
    + (s.muted ? ' · MUTED' : '');

  const u = s.heard.slice().sort((x,y)=> newest ? x.last - y.last : y.first - x.first);
  const k = s.known.slice().sort((x,y)=> (x.last < 0 ? 1e12 : x.last) - (y.last < 0 ? 1e12 : y.last));
  $('n').textContent = u.length;
  $('kn').textContent = k.length;
  $('empty').style.display = u.length ? 'none' : '';
  $('emptyKnown').style.display = k.length ? 'none' : '';

  const next = JSON.stringify([u.map(c=>[c.a,c.n]), k.map(c=>[c.a,c.name,c.d]), open, s.learning]);
  if (next === sig) { refreshAges(u, k); return; }
  sig = next;

  $('unknown').innerHTML = u.map(c => row(c.a,
      `${CMD[c.cmd] || ('0x'+c.cmd.toString(16))} <span style="color:var(--dim)">&middot; ${c.n}&times;</span>`,
      `<span id="ag${c.a}">${ago(c.last)}</span>`,
      editor(c.a, '', 0, true))).join('');

  $('known').innerHTML = k.map(c => row(c.a, esc(c.name),
      `<span id="ag${c.a}">${ago(c.last)}</span>`,
      editor(c.a, c.name, c.d, false))).join('');

  if (open !== null) counter(open);
}

function refreshAges(u, k){
  for (const c of u.concat(k)) { const e = $('ag'+c.a); if (e) e.textContent = ago(c.last); }
  $('state').textContent = last.learning ? last.learnLeft + 's'
    : (last.listening ? 'listening' : 'backing off');
}

function pick(a){ open = (open === a) ? null : a; sig = ''; draw(last); }
function shut(){ open = null; sig = ''; draw(last); }
function flip(){ newest = !newest; $('sortbtn').textContent = newest ? 'newest first' : 'walk order'; sig=''; draw(last); }

async function learn(){ await fetch('/api/control/learn',{method:'POST'}); poll(); }

async function save(a){
  const name = $('nm'+a).value.trim();
  if (!name) { alert('give it a name first'); return; }
  const d = [...document.querySelectorAll(`input[data-a="${a}"]:checked`)].map(e=>e.value).join(',');
  const r = await fetch(`/api/control/save?address=${a}&name=${encodeURIComponent(name)}&drives=${d}`,
                        {method:'POST'});
  if (!r.ok) { alert('NOT saved: ' + (await r.text())); return; }
  shut(); poll();
}

async function forget(a){
  if (!confirm('Forget this control?')) return;
  const r = await fetch('/api/control/forget?address='+a,{method:'POST'});
  if (!r.ok) alert('NOT forgotten: ' + (await r.text()));
  shut(); poll();
}

async function drop(a){ await fetch('/api/control/ignore?address='+a,{method:'POST'}); shut(); poll(); }

async function poll(){
  try { const s = await (await fetch('/api/heard')).json(); names = s.names; draw(s); } catch (e) {}
}
poll();
setInterval(poll, 2000);   // served on the same thread as everything else
</script></body></html>
)HTML";
