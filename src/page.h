// The settings web page (served at / on the bridge). Everything is filled in from /api/* with JavaScript.
#pragma once
static const char PAGE_HTML[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Keypad Bridge</title><style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1d2330;--mut:#667085;--acc:#2563eb;--bd:#d0d5dd;--warn:#b54708;--ok:#067647}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--card:#1b2027;--fg:#e6e8eb;--mut:#98a2b3;--acc:#60a5fa;--bd:#344054;--warn:#fdb022;--ok:#47cd89}}
body{font:15px system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:16px}
main{max-width:660px;margin:auto}.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:16px;margin-bottom:16px}
h1{font-size:20px;margin:4px 0 16px}h2{font-size:16px;margin:0 0 12px}
label{display:block;margin:10px 0 4px;color:var(--mut);font-size:13px}label.chk{color:inherit;font-size:15px}
input,select,textarea{width:100%;box-sizing:border-box;padding:8px;border:1px solid var(--bd);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
input[type=checkbox]{width:auto;margin-right:8px}
button{padding:8px 14px;border:0;border-radius:6px;background:var(--acc);color:#fff;font:inherit;cursor:pointer;margin-top:12px}
button.sec{background:transparent;color:var(--acc);border:1px solid var(--acc)}
small{color:var(--mut)}.msg{margin-left:8px}pre{white-space:pre-wrap;font-size:12px;max-height:240px;overflow:auto;margin:8px 0 0}
.row{display:flex;gap:8px;align-items:end}.row>*{flex:1}.row>.fit{flex:0 0 auto}
.lcd{font-family:monospace;background:#0b3d1b;color:#b7f7c5;padding:8px;border-radius:6px;white-space:pre;width:40ch;max-width:100%;overflow:hidden;line-height:1.4;box-sizing:content-box}
.banner{background:var(--acc);color:#fff;border-radius:10px;padding:14px 16px;margin-bottom:16px}
.ln{display:flex;gap:8px;align-items:start;padding:6px 0;border-top:1px solid var(--bd)}.ln b{width:84px;padding-top:8px;flex:0 0 auto}
.kv{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;margin-top:10px}.kv span:nth-child(odd){color:var(--mut)}
</style></head><body><main>
<h1>Tecom keypad &harr; Inception bridge</h1>
<div class=banner id=setup hidden><b>Welcome!</b> Step 1: pick your home Wi-Fi below, enter its password and press <b>Save Wi-Fi</b>.
The bridge restarts, joins your network and is then at <b id=hn2></b>.</div>

<div class=card><h2>Status</h2><div class=lcd id=lcd>&nbsp;</div><div class=kv id=kv></div>
<details><summary><small>Log</small></summary><pre id=log></pre></details></div>

<div class=card data-card=wifi><h2>Wi-Fi and device</h2>
<div class=row><div><label>Wi-Fi network</label><input name=wifi_ssid list=nets autocomplete=off><datalist id=nets></datalist></div>
<div class=fit><button type=button class=sec onclick=scan()>Scan</button></div></div>
<label>Wi-Fi password</label><input name=wifi_pass type=password data-secret=has_wifi_pass autocomplete=new-password>
<label>Device name (hostname)</label><input name=hostname placeholder=keypad-bridge>
<small>Letters, numbers and dashes. The page is then at http://<i>name</i>.local</small>
<label>Time zone (POSIX format)</label><input name=tz>
<small>Sydney/Melbourne/Canberra/Hobart: AEST-10AEDT,M10.1.0,M4.1.0/3 &middot; Brisbane: AEST-10 &middot; Adelaide: ACST-9:30ACDT,M10.1.0,M4.1.0/3 &middot; Perth: AWST-8</small>
<label>Time server</label><input name=ntp>
<label>Settings page password (optional, user name "admin")</label><input name=web_pass type=password data-secret=has_web_pass autocomplete=new-password>
<label class=chk><input type=checkbox name=clear_web_pass>Remove the page password</label>
<button type=button onclick="save('wifi')">Save Wi-Fi</button><small class=msg id=msg-wifi></small>
<br><small>Changing the network or name restarts the bridge.</small></div>

<div class=card data-card=inc><h2>Inception</h2>
<label>Inception address (IP or hostname)</label><input name=inception_host placeholder="192.168.1.50">
<label>API username</label><input name=username autocomplete=off>
<label>API password</label><input name=password type=password data-secret=has_password autocomplete=new-password>
<button type=button onclick="save('inc')">Save</button> <button type=button class=sec onclick=test()>Test connection</button>
<small class=msg id=msg-inc></small></div>

<div class=card data-card=screen><h2>Screen</h2>
<label>Idle message</label><input name=idle_message>
<label>Message when area 1 is armed</label><input name=armed_message>
<label>Message when a fault is present</label><input name=fault_message>
<small>Use | for the second line. Placeholders: {date} {time} {area} {state} {unsealed} {faults} {areas}.
{areas} lists each keypad area and its state, e.g. "1 Garage ARMED  2 Home off".</small>
<label class=chk><input type=checkbox name=show_custom>Include custom inputs (status flags) in {unsealed}</label>
<button type=button onclick="save('screen')">Save</button><small class=msg id=msg-screen></small></div>

<div class=card data-card=codes><h2>Codes</h2>
<label>PIN check</label><select name=pin_mode><option value=local>PIN codes listed below</option>
<option value=inception>Inception user PIN codes (checked through the API)</option></select>
<label>Accepted PIN codes (comma separated, for "listed below")</label><input name=local_pins type=password data-secret=has_local_pins autocomplete=new-password>
<label>12-key keypad (* and #)</label><select name=key_mode>
<option value=labels>Code then # = arm (ON), code then * = disarm (OFF)</option>
<option value=toggle>Code then # = arm/disarm toggle, * = clear</option></select>
<label>Door slots: code then #</label><select name=door_action>
<option value=Open>Open the door (momentary unlock)</option><option value=Unlock>Unlock the door (until locked)</option></select>
<small>On a door slot, code then * locks the door. Leave password and PIN fields blank to keep the saved values.</small><br>
<button type=button onclick="save('codes')">Save</button><small class=msg id=msg-codes></small></div>

<div class=card><h2>Keypad areas and doors</h2>
<small>On the keypad: slot number then #, then code then # (arm / open door) or * (disarm / lock door). A code on its own uses slot 1.</small>
<div id=slots><small>Waiting for Inception&hellip;</small></div>
<button type=button onclick=saveSlots()>Save keypad slots</button><small class=msg id=msg-slots></small></div>

<div class=card><h2>LEDs</h2>
<small>Pick what drives each LED: an area (solid = armed, flashing = alarm or entry/exit delay) or an input (lit while active, solid or flashing).
Use + to add more sources to one LED. Inputs on COMMS, RAS, DGP or BATTERY also show the fault message.</small>
<div id=leds><small>Waiting for Inception&hellip;</small></div>
<button type=button onclick=saveLeds()>Save LEDs</button><small class=msg id=msg-leds></small></div>

<div class=card><h2>LED test</h2>
<small>Lights one LED bit at a time (4 s each, 56 steps). The keypad screen shows which byte and bit is on.</small><br>
<button type=button onclick="ledtest('start')">Start LED test</button> <button type=button class=sec onclick="ledtest('stop')">Stop</button>
<label>Or set the 7 LED bytes directly (hex)</label><div class=row><input id=ledraw placeholder="00 20 00 01 00 00 00">
<div class=fit><button type=button class=sec onclick="ledtest('set')">Set</button></div></div></div>

<div class=card><h2>Firmware and backup</h2>
<label>Install new firmware (.bin from the build folder)</label><input type=file id=fw accept=.bin>
<button type=button onclick=upload()>Upload firmware</button><small class=msg id=msg-fw></small>
<label>Import settings from the PC bridge (paste bridge_config.json)</label><textarea id=imp rows=3></textarea>
<button type=button class=sec onclick=imp()>Import</button><small class=msg id=msg-imp></small><br>
<button type=button class=sec onclick=reboot()>Restart bridge</button></div>
</main><script>
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
const esc=t=>String(t).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;');
const say=(id,t,bad)=>{const e=$('#msg-'+id);e.textContent=t;e.style.color=bad?'var(--warn)':'var(--ok)'};
const api=async(p,body)=>{const r=await fetch(p,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 const t=await r.text();let j;try{j=JSON.parse(t)}catch(e){j={error:t||('HTTP '+r.status)}}if(!r.ok&&!j.error)j.error='HTTP '+r.status;return j};
let C={},E=null;
async function loadCfg(){C=await api('/api/config');
 $$('[name]').forEach(e=>{const k=e.name;if(e.type=='checkbox')e.checked=!!C[k];else if(e.dataset.secret){e.value='';e.placeholder=C[e.dataset.secret]?'saved - leave blank to keep':''}else if(k in C)e.value=C[k]});
 $('#hn2').textContent='http://'+(C.hostname||'keypad-bridge')+'.local'}
async function save(card){const b={};$$('[data-card='+card+'] [name]').forEach(e=>{b[e.name]=e.type=='checkbox'?e.checked:e.value});
 say(card,'Saving...');const j=await api('/api/config',b);if(j.error)return say(card,j.error,1);
 say(card,j.reboot?'Saved - restarting, reconnect in about 20 s':'Saved');loadCfg()}
async function test(){const b={};$$('[data-card=inc] [name]').forEach(e=>b[e.name]=e.value);say('inc','Testing...');
 const j=await api('/api/test',b);say('inc',j.msg||j.error,!j.ok)}
async function scan(){const d=$('#nets');d.innerHTML='';const j=await api('/api/scan');(j.nets||[]).forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.label=n.rssi+' dBm';d.appendChild(o)});
 say('wifi',(j.nets||[]).length+' networks found - click the network box to pick one')}
function srcSel(v){return '<select class=src style="margin:2px 0"><option value="">(nothing)</option><optgroup label="Areas (on = armed, flash = alarm)">'+
 E.areas.map(a=>'<option value="area:'+esc(a.ID)+'"'+(v=='area:'+a.ID?' selected':'')+'>'+esc(a.Name)+'</option>').join('')+'</optgroup><optgroup label="Inputs (lit while active)">'+
 E.inputs.map(i=>'<option value="input:'+esc(i.ID)+'"'+(v=='input:'+i.ID?' selected':'')+'>'+esc(i.Name)+'</option>').join('')+'</optgroup></select>'}
function addSrc(b){b.parentNode.querySelector('.srcs').insertAdjacentHTML('beforeend',srcSel(''))}
async function loadEnt(){E=await api('/api/entities');if(!E.loaded){setTimeout(loadEnt,4000);return}
 let k='';for(let n=1;n<=8;n++){const cur=(C.slots||{})[n]||'';k+='<div class=ln><b>Slot '+n+'</b><select data-slot='+n+'><option value="">(not used)</option><optgroup label="Areas (arm / disarm)">'+
  E.areas.map(a=>'<option value="area:'+esc(a.ID)+'"'+('area:'+a.ID==cur?' selected':'')+'>'+esc(a.Name)+'</option>').join('')+'</optgroup><optgroup label="Doors (open / lock)">'+
  E.doors.map(a=>'<option value="door:'+esc(a.ID)+'"'+('door:'+a.ID==cur?' selected':'')+'>'+esc(a.Name)+'</option>').join('')+'</optgroup></select></div>'}
 $('#slots').innerHTML=k;
 let h='';C.led_names.forEach(n=>{const c=(C.leds||{})[n]||{sources:[],mode:'on'};const label=n.startsWith('AREA')?'Area '+n.slice(4):n;
  h+='<div class=ln data-led="'+n+'"><b>'+label+'</b><div class=srcs style="flex:1">'+(c.sources.length?c.sources:['']).map(srcSel).join('')+'</div>'+
  '<select class=mode style="width:auto"><option value=on>Solid</option><option value=flash'+(c.mode=='flash'?' selected':'')+'>Flashing</option></select>'+
  '<button type=button class=sec style="margin-top:0" onclick="addSrc(this)">+</button></div>'});
 $('#leds').innerHTML=h}
async function saveSlots(){const s={};$$('[data-slot]').forEach(e=>{if(e.value)s[e.dataset.slot]=e.value});
 const j=await api('/api/config',{slots:s});say('slots',j.error||'Saved',j.error);loadCfg()}
async function saveLeds(){const l={};$$('[data-led]').forEach(r=>{const s=$$('[data-led="'+r.dataset.led+'"] .src').map(x=>x.value).filter(Boolean);
 if(s.length)l[r.dataset.led]={sources:s,mode:r.querySelector('.mode').value}});
 const j=await api('/api/config',{leds:l});say('leds',j.error||'Saved',j.error);await loadCfg();loadEnt()}
const ledtest=a=>api('/api/ledtest',{action:a,bytes:$('#ledraw').value});
async function upload(){const f=$('#fw').files[0];if(!f)return say('fw','Pick the firmware.bin file first',1);say('fw','Uploading...');
 const fd=new FormData();fd.append('fw',f);const r=await fetch('/update',{method:'POST',body:fd});const t=await r.text();say('fw',t,!r.ok)}
async function imp(){let o;try{o=JSON.parse($('#imp').value)}catch(e){return say('imp','That is not valid JSON',1)}
 const j=await api('/api/config',o);say('imp',j.error||(j.reboot?'Imported - restarting':'Imported'),j.error);$('#imp').value='';loadCfg().then(loadEnt)}
async function reboot(){if(confirm('Restart the bridge?'))api('/api/reboot',{})}
async function st(){try{const j=await api('/api/status');$('#lcd').textContent=j.screen||' ';$('#setup').hidden=!j.setup;
 const kv={'Keypad':j.keypad,'Inception':j.inception,'Wi-Fi':j.wifi,'Address':j.address,'Time':j.time,'Firmware':j.version,'Free memory':j.heap};
 $('#kv').innerHTML=Object.entries(kv).map(([k,v])=>'<span>'+k+'</span><span>'+esc(v)+'</span>').join('');
 $('#log').textContent=(j.log||[]).join('\n')}catch(e){}}
loadCfg().then(loadEnt);st();setInterval(st,2000);
</script></body></html>)rawliteral";
