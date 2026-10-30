/*
This file is part of picoFace.

picoFace is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with picoFace. If
not, see <https://www.gnu.org/licenses/>.
*/

// The page the browser gets from the file transfer (webfiles.h): the SD card's folders, a file
// downloaded by clicking it, files uploaded by choosing them or dropping them on the page.
// All of it is here (no other files, nothing from the internet). The title at the top is
// PicoMenu's own, as the NMI menu draws it (its font is Unscii 8, public domain), on the
// Spectrum's bright blue: a 124 x 14 picture, drawn twice the size. All the page's text is in
// that font too (webfont.inc, made from the menu's own font by tools/make_webfont.py: square
// pixels, 8 to the em), at 16px, twice its size, so it is as sharp as the title.

#include "webfiles.h"

namespace web {

const char PAGE[] = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PicoMenu</title>
<style>
@font-face{font-family:PicoMenu;src:url(data:font/woff;base64,)PAGE"
#include "webfont.inc"
R"PAGE() format('woff')}
:root{--bg:#f4f4f2;--fg:#1d1d1b;--dim:#6b6b66;--line:#d9d9d4;--card:#fff;--acc:#c8102e;--acc2:#0067b1}
@media (prefers-color-scheme:dark){:root{--bg:#121212;--fg:#ececea;--dim:#9a9a95;--line:#2c2c2a;--card:#1c1c1b;--acc:#ff5a5f;--acc2:#5fa8ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/24px PicoMenu,monospace;font-synthesis:none;-webkit-font-smoothing:none;-moz-osx-font-smoothing:grayscale;font-smooth:never}
header{padding:12px 16px;background:#0000ff}header img{display:block;width:248px;height:28px;image-rendering:pixelated;image-rendering:crisp-edges}
main{max-width:860px;margin:0 auto;padding:12px 16px 40px}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0 12px}
.crumbs{flex:1 1 auto;min-width:0;overflow-wrap:anywhere}.crumbs a{color:var(--acc2);text-decoration:none}
button,label.btn{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:6px;padding:6px 12px;cursor:pointer}
button:hover,label.btn:hover{border-color:var(--dim)}
table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden}
td{padding:8px 10px;border-top:1px solid var(--line);vertical-align:middle}tr:first-child td{border-top:0}
td.n{width:100%;overflow-wrap:anywhere}td.s{text-align:right;color:var(--dim);white-space:nowrap;font-variant-numeric:tabular-nums}
td.a{white-space:nowrap}td.a button{padding:2px 8px}
a.f{color:var(--fg);text-decoration:none}a.d{color:var(--acc2);text-decoration:none}
.hid{opacity:.55}#msg{min-height:1.4em;color:var(--dim);margin:6px 0}
#drop{display:none;position:fixed;inset:0;background:rgba(0,103,177,.15);border:3px dashed var(--acc2);z-index:9}
body.over #drop{display:block}
@media (max-width:600px){table,tbody{display:block}tr{display:flex;flex-wrap:wrap;align-items:center;border-top:1px solid var(--line)}tr:first-child{border-top:0}
td{border-top:0;padding:6px 8px}td.a button{padding:2px 5px}main{padding-left:10px;padding-right:10px}td.n{flex:1 1 100%;padding-bottom:0}td.s{flex:1 1 auto;text-align:left}td.a{flex:0 0 auto}}
</style></head><body>
<header><img alt="PicoMenu" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAHwAAAAOCAMAAADaKohGAAAAD1BMVEX/AAAAAP///wAA/wAA//+pXAbNAAAAgElEQVR42tWUQQ6AIAwEQf3/m00ta7dNjXIwwB60MZkOgWIpd+qVjaJf9pbacrRIbRwqzxuHCqS8y3C5b6MNUAPKYKWEs+Uzi4WvLedNj3I+uEwuHeaU88D1y+WZDZzRPLJzyWtIrzxeNb/tfvDWkz9ftS/yl2mfQv7375X5ofIT8boK/XCmFiMAAAAASUVORK5CYII="></header>
<main>
<div class="bar"><div class="crumbs" id="crumbs"></div>
<label class="btn">Upload files<input type="file" id="pick" multiple hidden></label>
<button id="mk">New folder</button></div>
<div id="msg">Drop files on the page to put them in this folder.</div>
<table id="list"></table>
</main><div id="drop"></div>
<script>
let cur='/',items=[];
const $=id=>document.getElementById(id);
const enc=encodeURIComponent;
const join=(a,b)=>a=='/'?'/'+b:a+'/'+b;
const say=t=>$('msg').textContent=t;
function size(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB'}
async function load(p){
 const r=await fetch('/api/list?p='+enc(p));
 if(!r.ok){say(await r.text());return}
 const j=await r.json();cur=j.path;items=j.items;
 items.sort((a,b)=>b.d-a.d||a.n.toLowerCase().localeCompare(b.n.toLowerCase()));
 const parts=cur.split('/').filter(x=>x);let c='<a href="#" data-p="/">SD:</a>',acc='';
 for(const x of parts){acc+='/'+x;c+=' / <a href="#" data-p="'+esc(acc)+'">'+esc(x)+'</a>'}
 $('crumbs').innerHTML=c;
 let h='';
 if(cur!='/')h+='<tr><td class="n" colspan="3"><a class="d" href="#" data-p="'+esc(cur.replace(/\/[^/]*$/,'')||'/')+'">.. up</a></td></tr>';
 for(const it of items){const p=join(cur,it.n);
  h+='<tr class="'+(it.h?'hid':'')+'"><td class="n">'+(it.d?'<a class="d" href="#" data-p="'+esc(p)+'">'+esc(it.n)+'/</a>':'<a class="f" href="/api/get?p='+enc(p)+'" download>'+esc(it.n)+'</a>')+
  '</td><td class="s">'+(it.d?'':size(it.s))+'</td><td class="a"><button data-r="'+esc(it.n)+'">Rename</button> <button data-e="'+esc(it.n)+'">Erase</button></td></tr>'}
 if(!items.length)h+='<tr><td class="n" style="color:var(--dim)">Empty</td></tr>';
 $('list').innerHTML=h}
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
document.addEventListener('click',async e=>{const t=e.target;
 if(t.dataset.p){e.preventDefault();load(t.dataset.p);return}
 if(t.dataset.e){if(!confirm('Erase '+t.dataset.e+'?'))return;const r=await fetch('/api/del?p='+enc(join(cur,t.dataset.e)),{method:'POST'});say(await r.text());load(cur)}
 if(t.dataset.r){const n=prompt('New name',t.dataset.r);if(!n||n==t.dataset.r)return;const r=await fetch('/api/ren?p='+enc(join(cur,t.dataset.r))+'&to='+enc(n),{method:'POST'});say(await r.text());load(cur)}});
$('mk').onclick=async()=>{const n=prompt('Folder name');if(!n)return;const r=await fetch('/api/mkdir?p='+enc(join(cur,n)),{method:'POST'});say(await r.text());load(cur)};
async function upload(files){
 let done=0;
 for(const f of files){
  let o='';if(items.some(i=>i.n.toLowerCase()==f.name.toLowerCase())){if(!confirm(f.name+' is there already. Replace it?'))continue;o='&o=1'}
  say('Sending '+f.name+' ('+size(f.size)+')...');
  const r=await fetch('/api/put?p='+enc(join(cur,f.name))+o,{method:'PUT',body:f});
  if(!r.ok){say(f.name+': '+await r.text());await load(cur);return}done++}
 say(done+' file'+(done==1?'':'s')+' sent');load(cur)}
$('pick').onchange=e=>{upload([...e.target.files]);e.target.value=''};
let depth=0;
addEventListener('dragenter',e=>{e.preventDefault();depth++;document.body.classList.add('over')});
addEventListener('dragleave',e=>{if(--depth<=0){depth=0;document.body.classList.remove('over')}});
addEventListener('dragover',e=>e.preventDefault());
addEventListener('drop',e=>{e.preventDefault();depth=0;document.body.classList.remove('over');upload([...e.dataTransfer.files].filter(f=>f.size||f.type))});
load('/');
</script></body></html>
)PAGE";

}   // namespace web
