/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
R"REMOTE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#190807"><title>ProsperoTV Remote</title>
<style>
:root{color-scheme:dark;font:16px system-ui,sans-serif;background:#190807;color:#fff2df}
*{box-sizing:border-box}body{margin:0;padding:24px 20px max(24px,env(safe-area-inset-bottom))}
main{max-width:420px;margin:auto}header{display:flex;align-items:center;gap:14px;margin-bottom:24px}
.logo{background:#ff9445;color:#190807;border-radius:15px;padding:12px;font-weight:900;letter-spacing:-1px}
h1{font-size:23px;margin:0}header p{margin:3px 0 0;color:#d3b6a8;font-size:14px}
.card{background:#2b100d;border:1px solid #633428;border-radius:20px;padding:20px;margin:16px 0}
label{display:block;color:#e8b968;font-size:13px;font-weight:700;letter-spacing:.08em;margin-bottom:10px}
input,button{font:inherit;border:1px solid #81503e;border-radius:12px;color:inherit;min-height:52px}
input{background:#190807;padding:13px;width:100%;font-size:18px}button{background:#3e1812;padding:12px;cursor:pointer;touch-action:manipulation}
button:hover{background:#7d2412}button:active{transform:scale(.96)}button:focus-visible,input:focus-visible{outline:3px solid #fff2df;outline-offset:3px}
button:disabled{opacity:.5;cursor:wait}.primary{background:#ff9445;color:#190807;font-weight:750}.primary:hover{background:#ffc276}
.row{display:flex;gap:10px;margin-top:12px}.row>*{flex:1}.muted{font-size:13px;color:#d3b6a8;line-height:1.5}
#status{min-height:24px;color:#e8b968;font-size:14px;margin:0 2px}.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;max-width:310px;margin:22px auto}
.pad button{height:76px;font-size:26px}.pad .ok{font-size:18px}#up{grid-column:2}#left{grid-column:1}#down{grid-column:2}
fieldset{border:0;padding:0;margin:0;min-width:0}[hidden]{display:none!important}footer{text-align:center;color:#d3b6a8;font-size:12px;margin-top:20px}
</style></head><body><main>
<header><div class="logo" aria-hidden="true">TV</div><div><h1>ProsperoTV</h1><p>Your phone. Your remote.</p></div></header>
<p id="status" role="status" aria-live="polite">Pair with the code on your TV</p>
<form id="pair" class="card"><label for="pin">TV PAIRING CODE</label>
<input id="pin" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" autocomplete="off" required placeholder="6-digit code">
<div class="row"><button class="primary" type="submit">Connect to TV</button></div>
<p class="muted">Use the same Wi-Fi as your PS5. Find the code along the bottom of ProsperoTV.</p></form>
<fieldset id="remote" disabled hidden>
<div class="pad" aria-label="Directional controls">
<button id="up" data-key="up" aria-label="Up">↑</button>
<button id="left" data-key="left" aria-label="Left">←</button>
<button class="primary ok" data-key="enter" aria-label="Enter">OK</button>
<button data-key="right" aria-label="Right">→</button>
<button id="down" data-key="down" aria-label="Down">↓</button></div>
<div class="row"><button data-key="back">← Back</button><button data-key="favorite">☆ Favorite</button></div>
<div class="row"><button data-key="previous">‹ Section</button><button data-key="next">Section ›</button></div>
<form id="search" class="card"><label for="query">FIND A CHANNEL</label>
<input id="query" type="search" maxlength="78" enterkeyhint="search" autocomplete="off" placeholder="Type with your phone keyboard">
<div class="row"><button class="primary" type="submit">Search on TV</button><button id="clear" type="button">Clear</button></div>
<div class="row"><button type="button" data-key="search">Search &amp; filters on TV</button></div>
<p class="muted">Up to 39 characters. Search the current channel list; Back also stops playback.</p></form>
<button id="disconnect" type="button" style="width:100%">Disconnect phone</button>
</fieldset><footer>Local network remote · ProsperoTV</footer></main>
<script>
const $=id=>document.getElementById(id);
let pin='',paired=false,busy=false;
try{pin=sessionStorage.getItem('prosperotv-pin')||''}catch{}
function unpair(){paired=false;$('remote').disabled=true;$('remote').hidden=true;$('pair').hidden=false;try{sessionStorage.removeItem('prosperotv-pin')}catch{}}
async function request(path,body){
 const response=await fetch('/api/'+path,{method:body===undefined?'GET':'POST',headers:{'X-Remote-Pin':pin,'Content-Type':'text/plain;charset=UTF-8'},body,signal:AbortSignal.timeout(4000)});
 const text=await response.text();
 if(!response.ok){if(response.status===401)unpair();throw Error(text)}return text;
}
async function connect(){
 try{$('status').textContent=await request('status');paired=true;$('remote').disabled=false;$('remote').hidden=false;$('pair').hidden=true;try{sessionStorage.setItem('prosperotv-pin',pin)}catch{}}
 catch(error){$('status').textContent=error.message||'Cannot connect. Check the TV and Wi-Fi.'}
}
$('pair').onsubmit=event=>{event.preventDefault();pin=$('pin').value;connect()};
async function command(path,body){
 if(busy||!paired)return;busy=true;
 try{$('status').textContent=await request(path,body)}catch(error){$('status').textContent=error.message||'Connection lost. Check the TV and Wi-Fi.'}finally{busy=false}
}
document.querySelectorAll('[data-key]').forEach(button=>button.onclick=()=>command('key',button.dataset.key));
$('search').onsubmit=event=>{event.preventDefault();const text=$('query').value;if([...text].length>39){$('status').textContent='Use up to 39 characters.';return}command('search',text)};
$('clear').onclick=()=>{$('query').value='';command('search','')};
$('disconnect').onclick=()=>{pin='';unpair();$('status').textContent='Phone disconnected'};
document.addEventListener('keydown',event=>{
 if(event.target.matches('input,button')||event.ctrlKey||event.altKey||event.metaKey)return;
 const key={ArrowUp:'up',ArrowDown:'down',ArrowLeft:'left',ArrowRight:'right',Enter:'enter',Escape:'back',Backspace:'back'}[event.key];
 if(key&&paired){event.preventDefault();command('key',key)}
});
setInterval(async()=>{if(paired&&!busy){try{$('status').textContent=await request('status')}catch(error){$('status').textContent=error.message||'Connection lost. Check the TV and Wi-Fi.'}}},10000);
if(pin)connect();
</script></body></html>)REMOTE"
