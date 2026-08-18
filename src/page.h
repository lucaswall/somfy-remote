#pragma once

#include <Arduino.h>

// The whole UI, served from flash. Self-contained by choice: it is opened from a phone
// standing in front of a shutter, to check whether the bridge or the motor is the problem,
// and a CDN round trip is the one thing that would make it hang there.
static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>somfy-remote</title>
<style>
:root{--bg:#0e1116;--card:#171c24;--line:#262d38;--fg:#e6eaf0;--dim:#8b95a5;
--accent:#4da3ff;--ok:#3ddc84;--warn:#f0b429}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);
font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
padding-bottom:calc(16px + env(safe-area-inset-bottom))}
main{max-width:520px;margin:0 auto}
h1{font-size:15px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim);
margin:0 0 14px;font-weight:600}
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
border-radius:999px;padding:2px 10px}
.badge.open{color:var(--ok);border-color:var(--ok)}
.badge.closed{color:var(--accent);border-color:var(--accent)}
.row{display:flex;gap:8px}
button{flex:1;min-height:46px;border:1px solid var(--line);border-radius:11px;
background:#1e242e;color:var(--fg);font-size:15px;font-weight:600;cursor:pointer;
-webkit-tap-highlight-color:transparent;transition:transform .08s,background .15s}
button:active{transform:scale(.97)}
button.pri{background:var(--accent);border-color:var(--accent);color:#04121f}
button.prog{flex:0 0 68px;font-size:12px;font-weight:600;color:var(--warn);
border-color:#3a3320;background:#1d1a12}
#log{margin:0;padding:10px;background:#0b0e13;border:1px solid var(--line);
border-radius:10px;max-height:40vh;overflow:auto;font:11px/1.5 ui-monospace,Menlo,
Consolas,monospace;color:var(--dim);white-space:pre-wrap;word-break:break-word}
footer{color:var(--dim);font-size:12px;text-align:center;margin-top:18px}
footer a{color:var(--accent);text-decoration:none}
.stale{opacity:.45}
</style></head><body><main>
<h1>somfy-remote</h1>

<div class="card" id="statusCard">
  <div class="grid">
    <div><span>Host</span><b id="host">—</b></div>
    <div><span>IP</span><b id="ip">—</b></div>
    <div><span>Signal</span><b id="rssi">—</b></div>
    <div><span>Uptime</span><b id="uptime">—</b></div>
    <div><span>Heap</span><b id="heap">—</b></div>
    <div><span>Queued</span><b id="pending">—</b></div>
  </div>
</div>

<div id="remotes"></div>

<div class="card">
  <h2>Console</h2>
  <pre id="log">…</pre>
</div>

<footer><a href="/status">status</a> · <a href="/log">log</a> ·
<a href="/errors">faults</a></footer>
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
      <div class="hdr"><b>Remote ${i}</b><span class="badge" id="p${i}">—</span></div>
      <div class="row">
        <button class="pri" onclick="send(${i},'Up')">Up</button>
        <button onclick="send(${i},'My')">My</button>
        <button class="pri" onclick="send(${i},'Down')">Down</button>
        <button class="prog" onclick="prog(${i})">Prog</button>
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
  if (s.remotes.length !== built) build(s.remotes.length);
  for (const r of s.remotes){
    const b = $('p' + r.n);
    b.textContent = r.position;
    b.className = 'badge ' + (r.position === 'unknown' ? '' : r.position);
  }
  document.body.classList.remove('stale');
}

async function poll(){
  try { render(await (await fetch('/api/state')).json()); }
  catch(e) { document.body.classList.add('stale'); }
}

async function send(n, command){
  try {
    render(await (await fetch(`/api/send?remote=${n}&command=${command}`,
                              {method:'POST'})).json());
  } catch(e) { document.body.classList.add('stale'); }
}

// Prog pairs or unpairs this emulated remote with whatever motor is listening in
// programming mode. It is the one button here that changes something permanent.
function prog(n){
  if (confirm(`Send Prog to remote ${n}? This pairs or unpairs a motor.`)) send(n, 'Prog');
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
