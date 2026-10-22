// The web page (web.h serves it at /): the PC's controls and a file manager with drag and drop. Self-contained.
#pragma once

static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tab5 386</title>
<style>
:root{--bg:#11141c;--card:#1a1f2b;--line:#2a3142;--ink:#e8e4da;--dim:#8b93a7;--accent:#5ae2c7;--warn:#f0a868;--bad:#f07878;--hover:#222838}
@media (prefers-color-scheme:light){:root{--bg:#f3f1ec;--card:#fff;--line:#dedad0;--ink:#1d2230;--dim:#6b7285;--accent:#118f78;--warn:#b86a12;--bad:#c23b3b;--hover:#f0ede6}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:980px;margin:0 auto;padding:20px 16px 60px}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:16px}
h1{font-size:22px;margin:0;font-weight:650;letter-spacing:.2px}
.pill{padding:3px 10px;border-radius:999px;font-size:13px;border:1px solid var(--line);color:var(--dim)}
.pill.run{color:var(--accent);border-color:var(--accent)}.pill.pause{color:var(--warn);border-color:var(--warn)}
.grow{flex:1}
button{font:inherit;color:var(--ink);background:var(--card);border:1px solid var(--line);border-radius:10px;padding:7px 14px;cursor:pointer}
button:hover{background:var(--hover)}button.primary{background:var(--accent);color:#0b1712;border-color:var(--accent);font-weight:600}
button.danger{color:var(--bad)}button:disabled{opacity:.45;cursor:default}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin-bottom:16px}
.row{display:flex;gap:18px;align-items:center;flex-wrap:wrap}
label.slider{display:flex;gap:10px;align-items:center;color:var(--dim);min-width:240px;flex:1}
input[type=range]{flex:1;accent-color:var(--accent)}
.banner{border-color:var(--warn);display:none}.banner.show{display:block}
.banner b{color:var(--warn)}
.tabs{display:flex;gap:6px;margin-bottom:10px;flex-wrap:wrap}
.tabs button{border-radius:999px}.tabs button.on{border-color:var(--accent);color:var(--accent)}
.crumbs{display:flex;gap:4px;align-items:center;flex-wrap:wrap;color:var(--dim);margin-bottom:8px}
.crumbs a{color:var(--ink);cursor:pointer;text-decoration:none;padding:2px 6px;border-radius:6px}.crumbs a:hover{background:var(--hover)}
table{width:100%;border-collapse:collapse}
td{padding:7px 8px;border-top:1px solid var(--line);white-space:nowrap}
td.name{width:100%;white-space:normal;word-break:break-all}
td.num{text-align:right;color:var(--dim);font-variant-numeric:tabular-nums}
tr.dir td.name a{color:var(--accent);font-weight:550}
td.name a{color:var(--ink);text-decoration:none;cursor:pointer}
.act button{padding:3px 9px;font-size:13px}
.empty{color:var(--dim);padding:18px 8px}
#drop{position:fixed;inset:0;background:color-mix(in srgb,var(--bg) 80%,transparent);border:3px dashed var(--accent);display:none;align-items:center;justify-content:center;font-size:22px;color:var(--accent);z-index:9}
#drop.show{display:flex}
#prog{display:none}#prog.show{display:block}
.bar{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:6px}.bar i{display:block;height:100%;background:var(--accent);width:0}
.dim{color:var(--dim)}.small{font-size:13px}
.tools{display:flex;gap:8px;flex-wrap:wrap;margin-left:auto}
#editor{position:fixed;inset:0;background:color-mix(in srgb,var(--bg) 85%,transparent);display:none;z-index:8;padding:16px}
#editor.show{display:flex;align-items:center;justify-content:center}
#editor .card{width:min(900px,100%);height:min(720px,100%);display:flex;flex-direction:column;gap:10px;margin:0}
#edtext{flex:1;width:100%;resize:none;font:13px/1.45 ui-monospace,Menlo,Consolas,monospace;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:10px;tab-size:8}
</style></head><body><main>
<header><h1>Tab5 386</h1><span id="state" class="pill">…</span><span class="grow"></span>
<button id="pausebtn"></button><button class="danger" id="resetbtn">Reset PC</button></header>

<div class="card banner" id="stopped"><b>The PC is stopped.</b> Its disk images can be deleted or replaced now, and
<code>tiny386.ini</code> changed. <b>Start PC</b> restarts the Tab5, which reads <code>tiny386.ini</code> again.</div>

<div class="card banner" id="banner"><b>The PC is paused</b> while its disk <span id="opendisk"></span> is open here.
<span id="bannermore"></span><div class="row" style="margin-top:10px"><button class="primary" id="donebtn">Done</button>
<span class="dim small" id="donehint"></span></div></div>

<div class="card row">
<label class="slider">Volume <input type="range" id="vol" min="0" max="255"></label>
<label class="slider">Brightness <input type="range" id="bri" min="8" max="255"></label></div>

<div class="card">
<div class="row" style="margin-bottom:10px"><div class="tabs" id="tabs"></div>
<div class="tools"><button id="mkdirbtn">New folder</button><button id="upfiles">Upload files</button><button id="updir">Upload folder</button></div></div>
<div class="crumbs" id="crumbs"></div>
<div id="prog"><span id="progtext" class="small dim"></span><div class="bar"><i id="progbar"></i></div></div>
<table id="list"></table>
<p class="dim small" id="hint">Drop files or folders anywhere on this page to copy them here.</p>
</div>
<input type="file" id="fileinput" multiple hidden><input type="file" id="dirinput" webkitdirectory hidden>
</main><div id="editor"><div class="card"><div class="row"><b id="edname"></b><span class="dim small" id="edinfo"></span>
<span class="grow"></span><button id="edcancel">Cancel</button><button class="primary" id="edsave">Save</button></div>
<textarea id="edtext" spellcheck="false" autocomplete="off"></textarea></div></div><div id="drop">Drop to copy into <span id="droppath"></span></div>
<script>
const $=id=>document.getElementById(id);
let st={}, vol='', path='/', busy=false, dragging=0, sliding=null, restarting=false;
const api=(m,u)=>fetch(u,{method:m}).then(async r=>{const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||r.statusText);return j});
const q=(o)=>Object.entries(o).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&');
const fmt=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB';
const isHd=v=>v.startsWith('hd');
const letter=v=>{const i=(st.disks||[]).findIndex(d=>d.vol==v);return i<0?'':String.fromCharCode(67+i)+':'};
const volName=v=>v=='sd'?'SD card':(letter(v)+' ('+v+')');
const dos83=n=>/^[^ .]{1,8}(\.[^ .]{1,3})?$/.test(n);

async function status(){
  try{st=await api('GET','/api/status')}catch(e){$('state').textContent=restarting?'restarting…':'offline';return}
  if(restarting&&st.pc!='stopped'){restarting=false;list();return}
  const s=$('state');s.textContent=restarting?'starting':st.pc;s.className='pill '+(st.pc=='running'?'run':'pause');
  const stopped=st.pc=='stopped';$('stopped').classList.toggle('show',stopped);
  $('pausebtn').textContent=stopped?'Start PC':st.pc=='paused'?'Resume':'Pause';$('pausebtn').disabled=!!st.open||restarting;
  $('pausebtn').className=stopped?'primary':'';$('resetbtn').style.display=stopped?'none':'';
  if(sliding!='vol')$('vol').value=st.volume;if(sliding!='bri')$('bri').value=st.brightness;
  const b=$('banner');b.classList.toggle('show',!!st.open);
  if(st.open){$('opendisk').textContent=volName(st.open);
    $('bannermore').textContent=st.changed?'When you are done, it restarts, so DOS reads the disk afresh.':'Nothing has been changed yet: it will simply continue.';
    $('donehint').textContent='(or by itself '+st.idle_close_s+' s after the last file operation)';}
  const vols=(st.disks||[]).map(d=>d.vol).concat(['sd']);
  if(!vols.includes(vol))vol=st.open||'sd';   // the card first: opening a PC disk pauses the PC
  $('tabs').innerHTML='';vols.forEach(v=>{const t=document.createElement('button');t.textContent=volName(v);t.className=v==vol?'on':'';
    if(isHd(v)&&st.open!=v)t.title='Opening the PC\u2019s disk pauses the PC until you are done';
    t.onclick=()=>{vol=v;path='/';list()};$('tabs').appendChild(t)});
}
async function list(){
  const tb=$('list');
  $('crumbs').innerHTML='';const parts=path.split('/').filter(x=>x);let acc='';
  const crumb=(txt,p)=>{const a=document.createElement('a');a.textContent=txt;a.onclick=()=>{path=p;list()};$('crumbs').appendChild(a)};
  crumb(volName(vol),'/');parts.forEach(x=>{acc+='/'+x;$('crumbs').append(' / ');crumb(x,acc)});
  $('droppath').textContent=volName(vol)+path;
  $('hint').textContent=isHd(vol)?'Drop files or folders anywhere on this page to copy them here. Opening the PC’s disk pauses the PC; DOS sees names longer than 8.3 shortened (LONGNA~1.TXT).':'Drop files or folders anywhere on this page to copy them to the SD card (disk images, CD-ROM .iso files, tiny386.ini).';
  let items;
  try{items=await api('GET','/api/ls?'+q({vol,path}))}catch(e){tb.innerHTML='<tr><td class="empty">'+e.message+'</td></tr>';status();return}
  items.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));
  tb.innerHTML='';
  if(path!='/'){const tr=tb.insertRow();tr.className='dir';const c=tr.insertCell();c.className='name';c.colSpan=4;
    const a=document.createElement('a');a.textContent='← ..';a.onclick=()=>{path=path.slice(0,path.lastIndexOf('/'))||'/';list()};c.appendChild(a)}
  if(!items.length){tb.insertAdjacentHTML('beforeend','<tr><td class="empty" colspan="4">Empty</td></tr>')}
  for(const it of items){
    const p=(path=='/'?'':path)+'/'+it.n, tr=tb.insertRow();tr.className=it.d?'dir':'';
    const c=tr.insertCell();c.className='name';const a=document.createElement('a');
    a.textContent=(it.d?'📁 ':'')+it.n+(it.busy?'  (in use by the PC)':'');
    if(it.d)a.onclick=()=>{path=p;list()};else if(!it.busy){a.href='/api/file?'+q({vol,path:p});a.download=it.n}
    c.appendChild(a);
    tr.insertCell().outerHTML='<td class="num">'+(it.d?'':fmt(it.s))+'</td>';
    const ac=tr.insertCell();ac.className='act';
    if(!it.d&&!it.busy&&isText(it.n)&&it.s<=EDIT_MAX){const ed=document.createElement('button');ed.textContent='Edit';
      ed.onclick=()=>edit(p,it.n);ac.append(ed,' ')}
    const del=document.createElement('button');del.textContent='Delete';del.className='danger';
    del.onclick=async()=>{
      if(it.busy){if(!confirm('The PC has '+it.n+' open. Stop the PC and delete it?\n\nLike switching the PC off: anything unsaved in DOS is lost. '+
          'It stays stopped until you start it again (Start PC), so you can delete or replace its other files and change tiny386.ini first.'))return}
      else if(!confirm('Delete '+it.n+(it.d?' and everything in it':'')+'?'))return;
      try{if(it.busy)await api('POST','/api/pc?do=stop');await api('DELETE','/api/file?'+q({vol,path:p}))}catch(e){alert(e.message)}
      list();status()};
    ac.appendChild(del)
  }
  status();
}
const CHUNK=256<<10;   // the Tab5 takes files in chunks of up to 256 KB (web.h)
function putChunk(p,offset,blob,done,size){return new Promise((res,rej)=>{const x=new XMLHttpRequest();x.open('PUT','/api/file?'+q(offset?{vol,path:p,offset}:{vol,path:p,offset,size}));
  x.upload.onprogress=e=>done(e.loaded);x.onload=()=>x.status<300?res():rej(new Error((JSON.parse(x.responseText||'{}').error)||x.statusText));
  x.onerror=()=>rej(new Error('network error'));x.send(blob)})}
async function put(p,file,done){let off=0;do{const end=Math.min(off+CHUNK,file.size);await putChunk(p,off,file.slice(off,end),l=>done(off+l),file.size);off=end}while(off<file.size)}
async function upload(files){   // [{rel, file}]
  if(busy||!files.length)return;busy=true;
  const total=files.reduce((s,f)=>s+f.file.size,0);let sent=0;$('prog').classList.add('show');
  const long=isHd(vol)?files.filter(f=>f.rel.split('/').some(n=>!dos83(n))).length:0;
  try{for(const [i,f] of files.entries()){
    const p=(path=='/'?'':path)+'/'+f.rel;
    $('progtext').textContent='Copying '+(i+1)+' of '+files.length+': '+f.rel;
    await put(p,f.file,l=>{$('progbar').style.width=(100*(sent+l)/Math.max(total,1))+'%'});sent+=f.file.size}
    $('progtext').textContent='Copied '+files.length+' file'+(files.length>1?'s':'')+' ('+fmt(total)+')'+(long?'. '+long+' with names DOS will see shortened.':'.');
  }catch(e){$('progtext').textContent='Stopped: '+e.message}
  busy=false;list();
}
async function entries(item,prefix,out){   // walk a dropped folder
  if(item.isFile){await new Promise(r=>item.file(f=>{out.push({rel:prefix+f.name,file:f});r()},r))}
  else if(item.isDirectory){const rd=item.createReader();let batch;
    do{batch=await new Promise(r=>rd.readEntries(r,()=>r([])));for(const e of batch)await entries(e,prefix+item.name+'/',out)}while(batch.length)}
}
addEventListener('dragenter',e=>{e.preventDefault();dragging++;$('drop').classList.add('show')});
addEventListener('dragleave',e=>{if(--dragging<=0){dragging=0;$('drop').classList.remove('show')}});
addEventListener('dragover',e=>e.preventDefault());
addEventListener('drop',async e=>{e.preventDefault();dragging=0;$('drop').classList.remove('show');
  const out=[],its=[...e.dataTransfer.items].map(i=>i.webkitGetAsEntry&&i.webkitGetAsEntry()).filter(x=>x);
  if(its.length){for(const it of its)await entries(it,'',out)}else{for(const f of e.dataTransfer.files)out.push({rel:f.name,file:f})}
  upload(out)});
$('upfiles').onclick=()=>$('fileinput').click();$('updir').onclick=()=>$('dirinput').click();
$('fileinput').onchange=e=>{upload([...e.target.files].map(f=>({rel:f.name,file:f})));e.target.value=''};
$('dirinput').onchange=e=>{upload([...e.target.files].map(f=>({rel:f.webkitRelativePath||f.name,file:f})));e.target.value=''};
$('mkdirbtn').onclick=async()=>{const n=prompt('New folder name'+(isHd(vol)?' (DOS: up to 8 letters)':''));if(!n)return;
  try{await api('POST','/api/mkdir?'+q({vol,path:(path=='/'?'':path)+'/'+n}))}catch(e){alert(e.message)}list()};
$('pausebtn').onclick=async()=>{if(st.pc=='stopped'){restart();return}
  await api('POST','/api/pc?do='+(st.pc=='paused'?'resume':'pause')).catch(e=>alert(e.message));status()};
async function restart(){   // the whole Tab5: it reads tiny386.ini again (back in ~10 s; WiFi reconnects)
  restarting=true;$('pausebtn').disabled=true;$('state').textContent='restarting…';
  await api('POST','/api/pc?do=restart').catch(()=>{})}
// The text editor (tiny386.ini, DOS's batch and config files): UTF-8 if the file is, else one byte per character
// (DOS's code page), and the file's own line ends (CR LF for DOS) kept.
const EDIT_MAX=CHUNK, isText=n=>/\.(ini|txt|bat|cfg|conf|md|log|lst|nfo)$/i.test(n)||/^(fd)?config\.sys$/i.test(n);
let edPath='', edCrlf=false, edUtf8=true;
async function edit(p,name){
  let bytes;try{const r=await fetch('/api/file?'+q({vol,path:p}));if(!r.ok)throw new Error((await r.json().catch(()=>({}))).error||r.statusText);
    bytes=new Uint8Array(await r.arrayBuffer())}catch(e){alert(e.message);return}
  let text;try{text=new TextDecoder('utf-8',{fatal:true}).decode(bytes);edUtf8=true}catch(e){text=Array.from(bytes,b=>String.fromCharCode(b)).join('');edUtf8=false}
  edCrlf=text.includes('\r\n');edPath=p;
  $('edname').textContent=name;$('edinfo').textContent=volName(vol)+' · '+(edCrlf?'CR LF':'LF')+(edUtf8?'':' · one byte per character');
  $('edtext').value=text.replace(/\r\n/g,'\n');$('editor').classList.add('show');$('edtext').focus()}
const edClose=()=>{$('editor').classList.remove('show');edPath=''};
$('edcancel').onclick=edClose;
addEventListener('keydown',e=>{if(e.key=='Escape'&&edPath)edClose()});
$('edsave').onclick=async()=>{
  let text=$('edtext').value;if(edCrlf)text=text.replace(/\n/g,'\r\n');
  const bytes=edUtf8?new TextEncoder().encode(text):Uint8Array.from(text,c=>c.charCodeAt(0)&255);
  if(bytes.length>CHUNK){alert('Too long to save here (up to 256 KB)');return}
  try{await putChunk(edPath,0,new Blob([bytes]),()=>{},bytes.length)}catch(e){alert('Not saved: '+e.message);return}
  const ini=vol=='sd'&&/^\/tiny386\.ini$/i.test(edPath);edClose();list();
  if(ini&&st.pc!='stopped'&&confirm('Saved. Restart the Tab5 now, so the PC starts with the new tiny386.ini?\n\nLike switching the PC off: anything unsaved in DOS is lost.'))restart()};
$('resetbtn').onclick=async()=>{if(!confirm('Reset the PC? Anything unsaved in DOS is lost.'))return;await api('POST','/api/pc?do=reset').catch(e=>alert(e.message));status()};
$('donebtn').onclick=async()=>{await api('POST','/api/pc?do=done').catch(e=>alert(e.message));status()};
for(const [id,key] of [['vol','volume'],['bri','brightness']]){const el=$(id);
  el.oninput=()=>{sliding=id;clearTimeout(el.t);el.t=setTimeout(()=>api('POST','/api/set?'+key+'='+el.value),120)};
  el.onchange=()=>{sliding=null;api('POST','/api/set?'+key+'='+el.value)}}
status().then(list);setInterval(()=>{if(!busy)status()},2000);
</script></body></html>)HTML";
