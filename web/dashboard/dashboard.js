/* ===== Version ===== */
var UI_VERSION="5C.2";
console.log("[squeek] dashboard.js loaded, v"+UI_VERSION);
document.addEventListener("DOMContentLoaded",function(){var v=document.getElementById("uiVer");if(v)v.textContent="v"+UI_VERSION;});

/* ===== i18n ===== */
var L={
en:{tabMap:"Map",tabPlay:"Play",tabSeq:"Seq",tabConfig:"Config",battery:"Battery",peers:"peers",
noNodes:"No nodes yet",buildMap:"Build Map",scanning:"Scanning...",needNodes:"Need 2+ nodes",
mode:"Mode",modeOff:"Off",modeTravel:"Travel",modeRandom:"Random",modeSeq:"Seq",
modeSched:"Sched",travel:"Travel Order",nearest:"Nearest",axis:"Axis",random:"Random",tones:"Tones",
start:"Play",stop:"Stop",addStep:"Add Step",clear:"Clear",save:"Save",load:"Load",node:"Node",tone:"Tone",
delay:"Delay ms",settings:"Settings",wifi:"WiFi",connected:"Connected",notConnected:"Not connected",
storage:"Storage",reboot:"Reboot",confirmReboot:"Reboot device?",version:"Version",uptime:"Uptime",
sequence:"Sequence",noTones:"No tones"},
fr:{tabMap:"Carte",tabPlay:"Jouer",tabSeq:"Séq",tabConfig:"Config",battery:"Batterie",peers:"pairs",
noNodes:"Aucun nœud",buildMap:"Construire",scanning:"Scan en cours…",needNodes:"2+ nœuds requis",
mode:"Mode",modeOff:"Arrêt",modeTravel:"Trajet",modeRandom:"Aléatoire",modeSeq:"Séq",
modeSched:"Planif",travel:"Ordre trajet",nearest:"Proche",axis:"Axe",random:"Aléatoire",tones:"Sons",
start:"Jouer",stop:"Arrêt",addStep:"Ajouter",clear:"Vider",save:"Sauver",load:"Charger",node:"Nœud",
tone:"Son",delay:"Délai ms",settings:"Paramètres",wifi:"WiFi",connected:"Connecté",notConnected:"Non connecté",
storage:"Stockage",reboot:"Redémarrer",confirmReboot:"Redémarrer l'appareil ?",version:"Version",uptime:"Durée",
sequence:"Séquence",noTones:"Aucun son"},
es:{tabMap:"Mapa",tabPlay:"Tocar",tabSeq:"Sec",tabConfig:"Config",battery:"Batería",peers:"pares",
noNodes:"Sin nodos",buildMap:"Construir mapa",scanning:"Escaneando…",needNodes:"2+ nodos necesarios",
mode:"Modo",modeOff:"Apagado",modeTravel:"Viaje",modeRandom:"Aleatorio",modeSeq:"Sec",
modeSched:"Prog",travel:"Orden viaje",nearest:"Cercano",axis:"Eje",random:"Aleatorio",tones:"Tonos",
start:"Tocar",stop:"Parar",addStep:"Agregar",clear:"Limpiar",save:"Guardar",load:"Cargar",node:"Nodo",
tone:"Tono",delay:"Retardo ms",settings:"Ajustes",wifi:"WiFi",connected:"Conectado",notConnected:"Sin conexión",
storage:"Almacén",reboot:"Reiniciar",confirmReboot:"¿Reiniciar dispositivo?",version:"Versión",uptime:"Tiempo",
sequence:"Secuencia",noTones:"Sin tonos"},
de:{tabMap:"Karte",tabPlay:"Spielen",tabSeq:"Seq",tabConfig:"Config",battery:"Batterie",peers:"Peers",
noNodes:"Keine Knoten",buildMap:"Karte bauen",scanning:"Scanne…",needNodes:"2+ Knoten nötig",
mode:"Modus",modeOff:"Aus",modeTravel:"Reise",modeRandom:"Zufall",modeSeq:"Seq",
modeSched:"Plan",travel:"Reisefolge",nearest:"Nächster",axis:"Achse",random:"Zufall",tones:"Töne",
start:"Spielen",stop:"Stopp",addStep:"Hinzufügen",clear:"Leeren",save:"Speichern",load:"Laden",node:"Knoten",
tone:"Ton",delay:"Verzög. ms",settings:"Einstellungen",wifi:"WLAN",connected:"Verbunden",notConnected:"Getrennt",
storage:"Speicher",reboot:"Neustart",confirmReboot:"Gerät neustarten?",version:"Version",uptime:"Laufzeit",
sequence:"Sequenz",noTones:"Keine Töne"}
};
var langs=["en","fr","es","de"],flags=["\uD83C\uDDEC\uD83C\uDDE7","\uD83C\uDDEB\uD83C\uDDF7","\uD83C\uDDEA\uD83C\uDDF8","\uD83C\uDDE9\uD83C\uDDEA"];
var lang=localStorage.getItem("sqlang")||navigator.language.slice(0,2);
if(langs.indexOf(lang)<0)lang="en";

function t(k){return(L[lang]&&L[lang][k])||L.en[k]||k}
function applyLang(){
document.querySelectorAll("[data-i18n]").forEach(function(el){
var k=el.getAttribute("data-i18n");
if(el.tagName==="OPTION")el.textContent=t(k);
else if(el.tagName==="INPUT")el.placeholder=t(k);
else el.textContent=t(k);
});
var idx=langs.indexOf(lang);
document.getElementById("langBtn").textContent=flags[idx>=0?idx:0];
}
document.getElementById("langBtn").onclick=function(){
var idx=(langs.indexOf(lang)+1)%langs.length;
lang=langs[idx];localStorage.setItem("sqlang",lang);applyLang();
};
applyLang();

/* ===== Tab switching ===== */
var tabBtns=document.querySelectorAll("#tabs button");
var tabPanes=document.querySelectorAll(".tab");
tabBtns.forEach(function(btn){
btn.onclick=function(){
tabBtns.forEach(function(b){b.classList.remove("active")});
tabPanes.forEach(function(p){p.classList.remove("active")});
btn.classList.add("active");
document.getElementById(btn.dataset.tab).classList.add("active");
refreshTab(btn.dataset.tab);
};
});

/* ===== API helper ===== */
function api(path,opts){
return fetch(path,opts).then(function(r){
if(!r.ok)throw new Error(r.status);return r.json();
});
}

/* ===== Toast ===== */
var toastTimer;
function toast(msg){
var el=document.getElementById("toast");el.textContent=msg;el.classList.add("show");
clearTimeout(toastTimer);toastTimer=setTimeout(function(){el.classList.remove("show")},2000);
}

/* ===== State ===== */
var peers=[],distances=[],tones=[],orchState={mode:0,travel_order:0,sequence:[]},cfgMeta=[];
var seqSteps=[],scanActive=false;

/* ===== Refresh dispatcher ===== */
function refreshTab(id){
switch(id){
case"tab-map":refreshMap();break;
case"tab-play":refreshPlay();break;
case"tab-seq":refreshSeq();break;
case"tab-config":refreshConfig();break;
}
}

/* =========================================================
   MAP TAB — Canvas + manual 3D projection
   ========================================================= */
var mapRx=-20,mapRy=30,mapZoom=1;
var mapDragging=false,mapPinching=false,mapStartX,mapStartY,mapStartDist,mapStartZoom;
var mapFocal=400; /* perspective focal length in px */
var mapCanvas,mapCtx,mapNodesEl;

/* --- 3D math helpers --- */
function deg(a){return a*Math.PI/180;}
function rotatePoint(x,y,z,rx,ry){
/* Ry then Rx */
var cy=Math.cos(deg(ry)),sy=Math.sin(deg(ry));
var cx=Math.cos(deg(rx)),sx=Math.sin(deg(rx));
var x1=cy*x+sy*z, z1=-sy*x+cy*z;
var y1=cx*y-sx*z1, z2=sx*y+cx*z1;
return [x1,y1,z2];
}
function project(x,y,z,W,H){
var s=mapFocal/(mapFocal+z);
return [(W/2+x*s*mapZoom),(H/2+y*s*mapZoom),s];
}

function refreshMap(){
Promise.all([api("/api/peers"),api("/api/distances"),api("/api/status")]).then(function(r){
peers=r[0];distances=r[1];
document.getElementById("mapPeerCount").innerHTML=r[2].alive_peers+" "+t("peers");
document.getElementById("mapBattery").textContent=r[2].battery_mv+" mV";
document.getElementById("mapDim").textContent=r[2].dimension+"D";
renderMap();
});
}

function hasPositions(){
return peers.length>=2&&peers.some(function(p){return p.pos[0]!==0||p.pos[1]!==0||p.pos[2]!==0});
}

function updateScanOverlay(){
var ov=document.getElementById("scanOverlay");
var btn=document.getElementById("scanBtn");
var lbl=document.getElementById("scanLabel");
if(hasPositions()){
ov.classList.add("hidden");scanActive=false;
btn.textContent=t("buildMap");btn.disabled=false;
return;
}
ov.classList.remove("hidden");
if(peers.length<2){
btn.textContent=t("needNodes");btn.disabled=true;
lbl.textContent=t("needNodes");
}else if(scanActive){
btn.textContent=t("scanning");btn.disabled=true;
lbl.textContent=t("scanning");
}else{
btn.textContent=t("buildMap");btn.disabled=false;
lbl.textContent=t("buildMap");
}
}

function renderMap(){
if(!mapCanvas){mapCanvas=document.getElementById("mapCanvas");if(mapCanvas)mapCtx=mapCanvas.getContext("2d");mapNodesEl=document.getElementById("mapNodes");}
if(!mapCanvas||!mapNodesEl)return;
var vp=document.getElementById("mapViewport");
var W=vp.clientWidth,H=vp.clientHeight;
var dpr=window.devicePixelRatio||1;
mapCanvas.width=W*dpr;mapCanvas.height=H*dpr;
mapCtx.setTransform(dpr,0,0,dpr,0,0);
mapCtx.clearRect(0,0,W,H);
mapNodesEl.innerHTML="";
updateScanOverlay();

if(!peers.length){
mapNodesEl.innerHTML='<div class="no-nodes">'+t("noNodes")+'</div>';return;
}
if(!hasPositions())return;

/* Center and scale world coords */
var ctr=[0,0,0];
peers.forEach(function(p){ctr[0]+=p.pos[0];ctr[1]+=p.pos[1];ctr[2]+=p.pos[2];});
ctr[0]/=peers.length;ctr[1]/=peers.length;ctr[2]/=peers.length;

var maxR=0;
peers.forEach(function(p){
var dx=p.pos[0]-ctr[0],dy=p.pos[1]-ctr[1],dz=p.pos[2]-ctr[2];
var r=Math.sqrt(dx*dx+dy*dy+dz*dz);
if(r>maxR)maxR=r;
});
var worldScale=maxR>0?Math.min(W,H)*0.3/maxR:1;

/* Build projected peer list */
var projected=peers.map(function(p){
var wx=(p.pos[0]-ctr[0])*worldScale;
var wy=(p.pos[1]-ctr[1])*worldScale;
var wz=(p.pos[2]-ctr[2])*worldScale;
var r=rotatePoint(wx,wy,wz,mapRx,mapRy);
var pr=project(r[0],r[1],r[2],W,H);
return {peer:p,sx:pr[0],sy:pr[1],s:pr[2],rz:r[2]};
});

/* Sort edges back-to-front by average depth */
var edgeList=[];
distances.forEach(function(e){
var pa=projected.find(function(n){return n.peer.idx===e.a});
var pb=projected.find(function(n){return n.peer.idx===e.b});
if(!pa||!pb)return;
edgeList.push({a:pa,b:pb,d:e.d,avgZ:(pa.rz+pb.rz)/2});
});
edgeList.sort(function(a,b){return a.avgZ-b.avgZ;});

/* Draw edges on canvas */
edgeList.forEach(function(e){
var alpha=0.1+0.15*Math.min(e.a.s,e.b.s);
mapCtx.strokeStyle="rgba(255,255,255,"+alpha.toFixed(2)+")";
mapCtx.lineWidth=Math.max(0.5,1*Math.min(e.a.s,e.b.s));
mapCtx.beginPath();
mapCtx.moveTo(e.a.sx,e.a.sy);
mapCtx.lineTo(e.b.sx,e.b.sy);
mapCtx.stroke();

/* Distance label at midpoint */
var mx=(e.a.sx+e.b.sx)/2, my=(e.a.sy+e.b.sy)/2;
var lblAlpha=0.3+0.4*Math.min(e.a.s,e.b.s);
var lblSize=Math.max(8,10*Math.min(e.a.s,e.b.s));
mapCtx.font=lblSize.toFixed(0)+"px system-ui";
mapCtx.fillStyle="rgba(136,136,136,"+lblAlpha.toFixed(2)+")";
mapCtx.textAlign="center";mapCtx.textBaseline="bottom";
mapCtx.fillText(Math.round(e.d)+"cm",mx,my-2);
});

/* Sort nodes back-to-front */
projected.sort(function(a,b){return a.rz-b.rz;});

/* Draw DOM nodes */
projected.forEach(function(n){
var p=n.peer;
var baseSize=p.is_gateway?18:14;
var size=Math.max(6,baseSize*n.s);
var alpha=0.4+0.6*n.s;
var color=p.battery_mv>3500?"#4caf50":p.battery_mv>3200?"#ff9800":"#f44336";
if(p.is_gateway)color="#ff6b35";

var node=document.createElement("div");
node.className="map-node"+(p.is_gateway?" gw":"");
node.style.cssText="left:"+n.sx.toFixed(1)+"px;top:"+n.sy.toFixed(1)+"px;"+
"width:"+size.toFixed(1)+"px;height:"+size.toFixed(1)+"px;"+
"background:"+color+";opacity:"+alpha.toFixed(2);
node.dataset.idx=p.idx;
node.onclick=function(ev){ev.stopPropagation();showNodeInfo(p);};
mapNodesEl.appendChild(node);

/* Node label */
var lbl=document.createElement("span");
lbl.className="map-label";
lbl.style.cssText="position:absolute;left:"+n.sx.toFixed(1)+"px;top:"+(n.sy+size/2+2).toFixed(1)+"px;"+
"font-size:"+Math.max(8,9*n.s).toFixed(0)+"px;color:rgba(224,224,224,"+alpha.toFixed(2)+");"+
"transform:translateX(-50%);white-space:nowrap;pointer-events:none";
lbl.textContent=p.is_gateway?"GW":"#"+p.idx;
mapNodesEl.appendChild(lbl);
});
}

function showNodeInfo(p){
var info=document.getElementById("mapInfo");
var fl=[];
if(p.flags&0x01)fl.push("alive");
if(p.flags&0x02)fl.push("sleeping");
if(p.flags&0x04)fl.push("dead");
info.innerHTML="<b>"+p.mac+"</b>"+(p.is_gateway?" (GW)":"")+"<br>"+
t("battery")+": "+p.battery_mv+" mV<br>"+
"Pos: ["+p.pos.map(function(v){return v.toFixed(1)}).join(", ")+"]<br>"+
"Status: "+fl.join(", ");
info.classList.add("show");
}

document.getElementById("mapViewport").onclick=function(){
document.getElementById("mapInfo").classList.remove("show");
};

/* Touch/mouse controls for map */
var mvp=document.getElementById("mapViewport");
function getTouchDist(t){var dx=t[0].clientX-t[1].clientX,dy=t[0].clientY-t[1].clientY;return Math.sqrt(dx*dx+dy*dy);}

mvp.addEventListener("pointerdown",function(e){
if(e.pointerType==="touch")return;
mapDragging=true;mapStartX=e.clientX;mapStartY=e.clientY;
mvp.setPointerCapture(e.pointerId);
});
mvp.addEventListener("pointermove",function(e){
if(!mapDragging||e.pointerType==="touch")return;
mapRy+=(e.clientX-mapStartX)*0.5;
mapRx-=(e.clientY-mapStartY)*0.5;
mapRx=Math.max(-80,Math.min(80,mapRx));
mapStartX=e.clientX;mapStartY=e.clientY;
renderMap();
});
mvp.addEventListener("pointerup",function(){mapDragging=false;});

mvp.addEventListener("touchstart",function(e){
if(e.touches.length===1){mapDragging=true;mapPinching=false;mapStartX=e.touches[0].clientX;mapStartY=e.touches[0].clientY;}
if(e.touches.length===2){mapPinching=true;mapDragging=false;mapStartDist=getTouchDist(e.touches);mapStartZoom=mapZoom;}
},{passive:true});
mvp.addEventListener("touchmove",function(e){
if(mapPinching&&e.touches.length===2){
var d=getTouchDist(e.touches);
mapZoom=Math.max(0.3,Math.min(4,mapStartZoom*(d/mapStartDist)));
renderMap();
}else if(mapDragging&&e.touches.length===1){
mapRy+=(e.touches[0].clientX-mapStartX)*0.5;
mapRx-=(e.touches[0].clientY-mapStartY)*0.5;
mapRx=Math.max(-80,Math.min(80,mapRx));
mapStartX=e.touches[0].clientX;mapStartY=e.touches[0].clientY;
renderMap();
}
},{passive:true});
mvp.addEventListener("touchend",function(){mapDragging=false;mapPinching=false;},{passive:true});

mvp.addEventListener("wheel",function(e){
e.preventDefault();
mapZoom=Math.max(0.3,Math.min(4,mapZoom*(1-e.deltaY*0.001)));
renderMap();
},{passive:false});

document.getElementById("mapReset").onclick=function(){
mapRx=-20;mapRy=30;mapZoom=1;renderMap();
};

document.getElementById("scanBtn").onclick=function(ev){
console.log("[squeek] scanBtn clicked, scanActive="+scanActive);
ev.stopPropagation();
if(scanActive)return;
scanActive=true;updateScanOverlay();
console.log("[squeek] POST /api/sweep");
api("/api/sweep",{method:"POST"}).then(function(){
console.log("[squeek] sweep OK");toast(t("scanning"));
}).catch(function(err){console.error("[squeek] sweep failed",err);scanActive=false;updateScanOverlay();});
};

/* =========================================================
   PLAY TAB
   ========================================================= */
function refreshPlay(){
Promise.all([api("/api/orch"),api("/api/tones")]).then(function(r){
orchState=r[0];tones=r[1];renderPlay();
});
}

function renderPlay(){
document.querySelectorAll("#modeGrid button").forEach(function(b){
b.classList.toggle("active",parseInt(b.dataset.mode)===orchState.mode);
});
document.getElementById("travelCard").style.display=orchState.mode===1?"block":"none";
document.querySelectorAll("#travelGrid button").forEach(function(b){
b.classList.toggle("active",parseInt(b.dataset.travel)===orchState.travel_order);
});
var grid=document.getElementById("toneGrid");
grid.innerHTML="";
if(!tones.length){grid.innerHTML='<span style="color:var(--text2);font-size:.85em">'+t("noTones")+'</span>';return;}
tones.forEach(function(tn){
var btn=document.createElement("button");
btn.className="btn btn-sm btn-outline";
btn.textContent=tn.name;
btn.onclick=function(){toast(tn.name+" \u266B");};
grid.appendChild(btn);
});
}

document.getElementById("modeGrid").onclick=function(e){
var btn=e.target.closest("[data-mode]");if(!btn)return;
var mode=parseInt(btn.dataset.mode);
api("/api/orch",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify({mode:mode})}).then(function(){orchState.mode=mode;renderPlay();});
};
document.getElementById("travelGrid").onclick=function(e){
var btn=e.target.closest("[data-travel]");if(!btn)return;
var order=parseInt(btn.dataset.travel);
api("/api/orch",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify({travel_order:order})}).then(function(){orchState.travel_order=order;renderPlay();});
};

/* =========================================================
   SEQUENCES TAB
   ========================================================= */
function refreshSeq(){
Promise.all([api("/api/orch"),api("/api/peers"),api("/api/tones")]).then(function(r){
orchState=r[0];peers=r[1];tones=r[2];
seqSteps=orchState.sequence.slice();
renderSeq();populateSeqDropdowns();
});
}

function populateSeqDropdowns(){
var ns=document.getElementById("seqNode");ns.innerHTML="";
peers.forEach(function(p){
var o=document.createElement("option");o.value=p.idx;
o.textContent="#"+p.idx+" "+p.mac.slice(-5);ns.appendChild(o);
});
var ts=document.getElementById("seqTone");ts.innerHTML="";
tones.forEach(function(tn){
var o=document.createElement("option");o.value=tn.idx;o.textContent=tn.name;ts.appendChild(o);
});
}

function renderSeq(){
var list=document.getElementById("seqList");
list.innerHTML="";
seqSteps.forEach(function(s,i){
var row=document.createElement("div");row.className="seq-row";
var peer=peers.find(function(p){return p.idx===s.node});
var tn=tones.find(function(t){return t.idx===s.tone});
row.innerHTML='<span class="seq-num">'+(i+1)+'</span>'+
'<span>'+(peer?peer.mac.slice(-5):"#"+s.node)+'</span>'+
'<span>'+(tn?tn.name:"T"+s.tone)+'</span>'+
'<span>'+s.delay+' ms</span>'+
'<button class="seq-del" data-i="'+i+'">&times;</button>';
list.appendChild(row);
});
document.getElementById("seqCounter").textContent=seqSteps.length+"/32";
list.querySelectorAll(".seq-del").forEach(function(btn){
btn.onclick=function(){seqSteps.splice(parseInt(btn.dataset.i),1);renderSeq();};
});
}

document.getElementById("seqAdd").onclick=function(){
if(seqSteps.length>=32){toast("Max 32 steps");return;}
seqSteps.push({
node:parseInt(document.getElementById("seqNode").value)||0,
tone:parseInt(document.getElementById("seqTone").value)||0,
delay:parseInt(document.getElementById("seqDelay").value)||500
});
renderSeq();
};

document.getElementById("seqSave").onclick=function(){
api("/api/sequence",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify({steps:seqSteps,save:true})}).then(function(r){toast(t("save")+" ("+r.steps+")");});
};
document.getElementById("seqLoad").onclick=function(){refreshSeq();};
document.getElementById("seqClear").onclick=function(){seqSteps=[];renderSeq();};
document.getElementById("seqPlay").onclick=function(){
api("/api/sequence",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify({steps:seqSteps,save:false})}).then(function(){
return api("/api/orch",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify({mode:3})});
}).then(function(){orchState.mode=3;toast(t("start"));});
};

/* =========================================================
   CONFIG TAB
   ========================================================= */
var cfgValues={};

function refreshConfig(){
Promise.all([api("/api/config"),api("/api/status"),api("/api/storage")]).then(function(r){
var cfg=r[0],status=r[1],storage=r[2];
cfgMeta=cfg._meta||[];
cfgValues={};
cfgMeta.forEach(function(m){cfgValues[m.key]=cfg[m.key];});
renderConfig();

document.getElementById("sysWifi").textContent=status.mac;
document.getElementById("sysUptime").textContent=formatUptime(status.uptime_s);
document.getElementById("sysHeap").textContent=Math.round(status.free_heap/1024)+" KB";
document.getElementById("sysBuild").textContent=status.build||"--";
var pct=storage.total?Math.round(storage.used/storage.total*100):0;
document.getElementById("sysStorage").textContent=Math.round(storage.used/1024)+"/"+Math.round(storage.total/1024)+" KB";
document.getElementById("storageBar").style.width=pct+"%";
});
}

function formatUptime(s){
var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
return(h?h+"h ":"")+(m?m+"m ":"")+sec+"s";
}

function renderConfig(){
var list=document.getElementById("cfgList");
list.innerHTML="";
cfgMeta.forEach(function(m){
var row=document.createElement("div");row.className="cfg-row";
var val=cfgValues[m.key];
if(m.type==="bool"){
row.innerHTML='<div class="cfg-label"><div class="key">'+m.key+'</div><div class="desc">'+m.desc+'</div></div>'+
'<label class="toggle"><input type="checkbox" data-key="'+m.key+'"'+(val?' checked':'')+
'><span class="slider"></span></label>';
}else{
var step=m.type==="float"?"0.01":"1";
row.innerHTML='<div class="cfg-label"><div class="key">'+m.key+'</div><div class="desc">'+m.desc+'</div></div>'+
'<div class="cfg-input"><input type="number" step="'+step+'" data-key="'+m.key+'" value="'+val+'"></div>';
}
list.appendChild(row);
});
list.querySelectorAll("input").forEach(function(inp){
inp.onchange=function(){
var k=inp.dataset.key;
if(inp.type==="checkbox")cfgValues[k]=inp.checked;
else if(inp.step==="0.01")cfgValues[k]=parseFloat(inp.value);
else cfgValues[k]=parseInt(inp.value);
};
});
}

document.getElementById("cfgSave").onclick=function(){
var payload={};
cfgMeta.forEach(function(m){payload[m.key]=cfgValues[m.key];});
api("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify(payload)}).then(function(r){toast(t("save")+" ("+r.applied+")");});
};

document.getElementById("btnReboot").onclick=function(){
if(!confirm(t("confirmReboot")))return;
api("/api/reboot",{method:"POST"}).then(function(){toast(t("reboot")+"...");});
};

/* =========================================================
   WebSocket
   ========================================================= */
var ws,wsRetry=1000;

function wsConnect(){
var proto=location.protocol==="https:"?"wss":"ws";
ws=new WebSocket(proto+"://"+location.host+"/ws");
ws.onopen=function(){
document.getElementById("connDot").classList.add("on");wsRetry=1000;
};
ws.onclose=function(){
document.getElementById("connDot").classList.remove("on");
setTimeout(wsConnect,Math.min(wsRetry,10000));wsRetry*=1.5;
};
ws.onmessage=function(ev){
try{
var msg=JSON.parse(ev.data);
if(msg.type==="peer_join"||msg.type==="peer_leave"||msg.type==="peer_update"){
refreshMap();
}
if(msg.type==="orch_update"){
orchState.mode=msg.mode;
if(msg.travel_order!==undefined)orchState.travel_order=msg.travel_order;
var active2=document.querySelector(".tab.active");
if(active2&&active2.id==="tab-play")renderPlay();
}
}catch(e){}
};
}
wsConnect();

/* ===== Initial load ===== */
refreshMap();
