#pragma once
/*
 * Audiobird 网页 App (单文件, 存入 PROGMEM, 由固件 / 路由返回)
 * v0.2.15: 深色高级版界面 (用户 2026-10-26 定稿的演示方向); 新增 TF 卡
 * 真格式化 (/api/format_card)。单文件、零联网资源, 热点下离线可用。
 * 旧 ID 与 JS 函数名全部保留, 固件接口不动。
 */
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Audiobird</title>
<style>
body{font-family:-apple-system,system-ui,"PingFang SC","Microsoft YaHei",sans-serif;max-width:640px;margin:0 auto;padding:14px;background:#0B0E13;color:#EDEFF3}
.head{display:flex;align-items:center;gap:12px;margin:6px 0 4px}
.logo{width:46px;height:46px;border-radius:50%;background:#F2B33D;color:#1A1408;font-size:28px;line-height:46px;text-align:center;flex:none}
h1{font-size:26px;margin:0}h2{font-size:19px;margin:0 0 12px;display:flex;align-items:center;gap:8px}
.sub{color:#9AA5B4;font-size:13px;margin:4px 0 0}
.dot{width:10px;height:10px;border-radius:50%;flex:none}.dot.green{background:#3DDC84}.dot.gold{background:#F2B33D}.dot.grey{background:#6B7686}
.pill{margin-left:auto;font-size:13px;color:#9AA5B4;border:1px solid #2A3346;border-radius:20px;padding:4px 12px;white-space:nowrap}
.pill.gold{color:#F2B33D;border-color:#F2B33D}
.card{background:#181E2A;border:1px solid #2A3346;border-radius:18px;padding:16px;margin:14px 0}
.tiles{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:12px}
.tile{background:#11151E;border:1px solid #2A3346;border-radius:14px;padding:12px}
.tlabel{display:block;color:#9AA5B4;font-size:13px}.tbig{display:block;font-size:22px;font-weight:700;margin:4px 0}.tsmall{display:block;color:#6B7686;font-size:12px}
label{display:block;margin:14px 0 6px;color:#9AA5B4;font-size:14px}
input,select{width:100%;padding:11px;border:1px solid #2A3346;border-radius:10px;font-size:15px;box-sizing:border-box;background:#0E1219;color:#EDEFF3}
input[type=range]{padding:0;accent-color:#F2B33D}
button{border:0;border-radius:12px;padding:13px 16px;font-size:15px;margin:8px 0 0;cursor:pointer;width:100%}
button.gold{background:#F2B33D;color:#1A1408;font-weight:700}
button.sec{background:#232B3A;color:#EDEFF3;border:1px solid #2A3346}
button.danger{background:#E5484D;color:#fff;font-weight:700}
.row{display:flex;gap:10px}.row>*{flex:1}
.seg{display:flex;background:#0E1219;border:1px solid #2A3346;border-radius:10px;padding:4px;gap:4px}
.seg button{margin:0;padding:10px 4px;background:transparent;color:#9AA5B4;font-size:14px}
.seg button.on{background:#F2B33D;color:#1A1408;font-weight:700}
.bigtime{font-size:26px;font-weight:700;margin:4px 0 8px}
.warnbox{border:1px solid #F2B33D;background:#241C0A;border-radius:14px;padding:14px;margin-top:12px}
.mono{font-family:monospace;font-size:13px;white-space:pre-wrap;background:#0A0E14;color:#8B98AB;padding:10px;border-radius:10px;border:1px solid #2A3346}
small{color:#9AA5B4}p{margin:10px 0}
</style></head><body>
<div class="head"><div class="logo">🐦</div><div><h1>Audiobird</h1><p class="sub" id="headSub">正在连接设备…</p></div><span class="pill gold" id="modePill">配置模式</span></div>
<p class="sub" id="statusLine">正在连接设备…</p>
<div class="card" id="confirmBar" style="display:none"><span id="confirmText"></span><br>
<button class="gold" id="confirmYes">确认执行</button><button class="sec" onclick="hideConfirm()">取消</button></div>

<div class="card"><h2><span class="dot green"></span>状态 <span class="pill" id="statusPill">—</span></h2>
<div class="tiles">
<div class="tile"><span class="tlabel">存储</span><span class="tbig" id="stSd">—</span><span class="tsmall" id="stSdSub"></span></div>
<div class="tile"><span class="tlabel">电量</span><span class="tbig" id="stBat">—</span><span class="tsmall" id="stBatSub"></span></div>
<div class="tile"><span class="tlabel">录音文件</span><span class="tbig" id="stAudio">—</span><span class="tsmall">条录音</span></div>
<div class="tile"><span class="tlabel">增益</span><span class="tbig" id="stGain">—</span><span class="tsmall">数字增益 1–32</span></div>
</div>
<p>设备时间：<span id="devTime">—</span></p>
<p>位置：<span id="stLoc">未设置</span></p>
<div id="statusBox" style="display:none">加载中…</div>
<button class="sec" onclick="loadStatus()">刷新状态</button></div>

<div class="card"><h2><span class="dot gold"></span>配置</h2>
<label>采样率</label>
<div class="seg" id="srSeg"><button type="button" data-v="8000" onclick="pickSeg('srSeg','sample_rate','8000')">8 kHz</button><button type="button" data-v="16000" onclick="pickSeg('srSeg','sample_rate','16000')">16 kHz</button><button type="button" data-v="32000" onclick="pickSeg('srSeg','sample_rate','32000')">32 kHz</button><button type="button" data-v="48000" onclick="pickSeg('srSeg','sample_rate','48000')">48 kHz</button></div>
<select id="sample_rate" style="display:none"><option value="8000">8000 Hz</option><option value="16000">16000 Hz</option><option value="32000">32000 Hz</option><option value="48000">48000 Hz</option></select>
<label>数字增益 <span style="color:#F2B33D" id="gainVal">× 8</span></label>
<input type="range" id="gainRange" min="1" max="32" value="8">
<label>数字增益（精确值 1–32）</label><input id="gain" type="number" min="1" max="32">
<label>录制时长（秒）</label><input id="record_seconds" type="number" min="10" max="3600">
<label>休眠间隔（秒，0 = 连续录制）</label><input id="sleep_seconds" type="number" min="0" max="86400">
<label>录制时间表（如 05:00-08:00,17:00-19:00，为空 = 全天）</label><input id="schedule" type="text" placeholder="05:00-08:00,17:00-19:00">
<label>设备名</label><input id="device_name" type="text">
<button class="gold" onclick="saveConfig()">保存配置</button>
<small>保存后设备重启进入工作模式。</small></div>

<div class="card"><h2><span class="dot gold"></span>录制地点 (GPS)</h2>
<p id="locNow">尚未设置位置</p>
<label>粘贴坐标（推荐）：在地图 App 长按布点位置 → 分享/复制，回来粘到这里</label>
<input id="pasteBox" type="text" placeholder="粘贴地图分享文本、链接或 31.284512, 121.501234">
<button class="sec" onclick="parsePaste()">解析粘贴内容</button>
<p id="pasteMsg"></p>
<button class="sec" onclick="getPos()">📍 尝试浏览器自动定位</button>
<p id="geoMsg"><small>浏览器定位只允许在 HTTPS 页面使用，本页是局域网 http 页面，多数浏览器会拒绝（这不是设备故障）。推荐用上方粘贴：高德/苹果地图长按位置 → 分享 → 复制，粘贴后自动解析并按来源选坐标系。iPhone 也可以打开自带「指南针」，底部直接显示经纬度（WGS-84）。</small></p>
<div class="row"><div><label>纬度（如 31.284512）</label><input id="lat" type="text" inputmode="decimal" placeholder="31.284512"></div><div><label>经度（如 121.501234）</label><input id="lon" type="text" inputmode="decimal" placeholder="121.501234"></div></div>
<label>坐标系</label>
<div class="seg" id="geoSeg"><button type="button" data-v="WGS84" onclick="pickSeg('geoSeg','geo_sys','WGS84')">WGS-84</button><button type="button" data-v="GCJ02" onclick="pickSeg('geoSeg','geo_sys','GCJ02')">GCJ-02</button><button type="button" data-v="BD09" onclick="pickSeg('geoSeg','geo_sys','BD09')">BD-09</button></div>
<select id="geo_sys" style="display:none"><option value="WGS84">WGS-84</option><option value="GCJ02">GCJ-02</option><option value="BD09">BD-09</option></select>
<label>地点名（如：同济樱花林东侧）</label><input id="loc_name" type="text" placeholder="便于后期分析时识别">
<input id="loc_acc" type="hidden"><input id="loc_time" type="hidden">
<div class="row"><button class="sec" onclick="saveLoc()">仅保存位置</button><button class="sec" onclick="clearLoc()">清除位置</button></div>
<small>位置保存在 SD 卡 CONFIG.TXT；之后每条录音都会在 ABIRD_LOG.CSV 里记一行（时间 / 文件名 / 坐标 / 坐标系 / 地点名），后期分析时按文件名与识别结果对应。</small></div>

<div class="card"><h2><span class="dot green"></span>对时</h2>
<div class="bigtime" id="devTimeBig">—</div>
<button class="gold" onclick="syncTime()">用手机时间同步</button>
<p><small id="timeSyncNote"></small></p></div>

<div class="card"><h2><span class="dot gold"></span>系统</h2>
<label>固件升级 (.bin)</label><input id="fwFile" type="file" accept=".bin">
<button class="sec" onclick="uploadFw()">上传并升级</button>
<div class="row"><button class="sec" onclick="reboot()">重启设备</button><button class="danger" onclick="formatSd()">清空 SD 卡</button></div>
<p><small id="formatNote"></small></p>
<button class="danger" onclick="showFormatCard()">格式化 TF 卡</button>
<div id="formatCardBox" class="warnbox" style="display:none">
<p><small>格式化将抹掉卡上全部内容（含录音与日志），设备设置会从内存自动恢复。格式化中请勿断电。</small></p>
<label>输入 FORMAT 确认</label><input id="formatInput" type="text" placeholder="FORMAT">
<div class="row"><button class="danger" onclick="doFormatCard()">执行格式化</button><button class="sec" onclick="hideFormatCard()">取消</button></div>
</div>
<p><small id="formatCardResult"></small></p>
<small>清空 = 删除全部文件；格式化 = 重建文件系统（可修复文件系统损坏、删不掉的系统目录、exFAT 卡无法挂载）。</small>
<h2 style="margin-top:16px">日志</h2><div class="mono" id="logBox">—</div>
<button class="sec" onclick="loadLog()">刷新日志</button></div>
<p class="sub" style="text-align:center">Audiobird · 仅局域网访问</p>

<script>
async function api(p,o){const r=await fetch(p,o);if(!r.ok)throw new Error(r.status);return r.json();}
function fmtTime(u){if(!u)return '未对时';return new Date(u*1000).toLocaleString();}
function setSeg(id,v){const seg=document.getElementById(id);seg.querySelectorAll('button').forEach(function(b){b.classList.toggle('on',b.getAttribute('data-v')===String(v));});}
function pickSeg(id,hiddenId,v){document.getElementById(hiddenId).value=v;setSeg(id,v);}
async function loadStatus(){try{const s=await api('/api/status');
document.getElementById('statusLine').textContent='已连接 · 固件 v'+s.firmware+' · 模式: '+s.mode;
document.getElementById('headSub').textContent=s.device+' · 固件 v'+s.firmware;
document.getElementById('modePill').textContent=s.mode==='config'?'配置模式':s.mode;
document.getElementById('statusPill').textContent='已连接';
document.getElementById('statusBox').style.display='none';
document.getElementById('stSd').textContent=Math.round(s.sd_free/1024)+' MB';
document.getElementById('stSdSub').textContent='可用 / 共 '+Math.round(s.sd_total/1024)+' MB';
if(s.battery_mv>0){document.getElementById('stBat').textContent=s.battery_mv+' mV';document.getElementById('stBatSub').textContent=s.battery_pct+'%';}
else{document.getElementById('stBat').textContent='USB 供电';document.getElementById('stBatSub').textContent='未接电池检测';}
document.getElementById('stAudio').textContent=s.audio_files;
document.getElementById('stGain').textContent='× '+s.gain;
document.getElementById('devTime').textContent=fmtTime(s.time);
document.getElementById('devTimeBig').textContent=fmtTime(s.time);
document.getElementById('stLoc').textContent=s.has_loc?(s.loc_name+' · '+s.lat+', '+s.lon+' ('+s.geo_sys+')'):'未设置';
}catch(e){document.getElementById('statusLine').textContent='连接失败: '+e;
const b=document.getElementById('statusBox');b.style.display='block';
b.innerHTML='<small>状态加载失败（'+e+'），请点下方按钮重试。</small><br><button class="sec" onclick="loadStatus()">重新加载状态</button>';}}
async function loadConfig(){try{const c=await api('/api/config');
for(const k of ['sample_rate','gain','record_seconds','sleep_seconds','schedule','device_name'])
document.getElementById(k).value=c[k]??'';
setSeg('srSeg',c.sample_rate);setSeg('geoSeg',c.geo_sys||'WGS84');
document.getElementById('gainRange').value=c.gain;document.getElementById('gainVal').textContent='× '+c.gain;
if(c.has_loc){document.getElementById('lat').value=c.lat;document.getElementById('lon').value=c.lon;
document.getElementById('geo_sys').value=c.geo_sys||'WGS84';document.getElementById('loc_name').value=c.loc_name||'';
document.getElementById('locNow').textContent='当前已保存: '+c.lat+', '+c.lon+' ('+(c.geo_sys||'WGS84')+') '+(c.loc_name||'');}
}catch(e){}}
async function saveConfig(){const fd=new FormData();
for(const k of ['sample_rate','gain','record_seconds','sleep_seconds','schedule','device_name'])
fd.append(k,document.getElementById(k).value);
try{await api('/api/config',{method:'POST',body:new URLSearchParams(fd)});
alert('已保存, 设备将重启进入工作模式');}catch(e){alert('保存失败: '+e);}}
function parsePaste(){const t=document.getElementById('pasteBox').value.trim();
const pm=document.getElementById('pasteMsg');
if(!t){pm.innerHTML='<small>先粘贴内容再点解析。</small>';return;}
let lat=null,lon=null,sys='';
let m=t.match(/@(-?\d{1,3}\.\d+),(-?\d{1,3}\.\d+)/);
if(m){lat=parseFloat(m[1]);lon=parseFloat(m[2]);sys='WGS84';}
if(lat===null){const nums=t.match(/-?\d{1,3}\.\d{3,}/g);
if(nums&&nums.length>=2){let a=parseFloat(nums[0]),b=parseFloat(nums[1]);
if(Math.abs(a)>90&&Math.abs(b)<=90){lon=a;lat=b;}else{lat=a;lon=b;}}}
if(lat===null||isNaN(lat)||isNaN(lon)||Math.abs(lat)>90||Math.abs(lon)>180){
pm.innerHTML='<small>没有解析出有效的经纬度, 请检查粘贴内容, 或直接在下方手动填写。</small>';return;}
if(/百度|baidu/i.test(t))sys='BD09';
else if(/高德|amap|腾讯地图/i.test(t))sys='GCJ02';
else if(/google|谷歌/i.test(t))sys='WGS84';
document.getElementById('lat').value=lat.toFixed(6);
document.getElementById('lon').value=lon.toFixed(6);
if(sys){document.getElementById('geo_sys').value=sys;setSeg('geoSeg',sys);}
document.getElementById('loc_time').value=Math.floor(Date.now()/1000);
pm.innerHTML='<small>已解析: 纬度 '+lat.toFixed(6)+', 经度 '+lon.toFixed(6)+(sys?'，坐标系 '+sys:'')+'。核对无误后点「仅保存位置」。</small>';}
async function getPos(){const m=document.getElementById('geoMsg');
if(!navigator.geolocation){m.innerHTML='<small>此浏览器不支持定位, 请手动填入坐标。</small>';return;}
m.innerHTML='<small>正在定位, 请允许浏览器使用位置…</small>';
navigator.geolocation.getCurrentPosition(function(p){
document.getElementById('lat').value=p.coords.latitude.toFixed(6);
document.getElementById('lon').value=p.coords.longitude.toFixed(6);
document.getElementById('geo_sys').value='WGS84';setSeg('geoSeg','WGS84');
document.getElementById('loc_acc').value=Math.round(p.coords.accuracy||0);
document.getElementById('loc_time').value=Math.floor(Date.now()/1000);
m.innerHTML='<small>定位成功: 精度约 '+Math.round(p.coords.accuracy||0)+' 米 (WGS-84)。检查无误后点「仅保存位置」。</small>';},
function(e){m.innerHTML='<small>自动定位失败 ('+e.message+')。常见原因: 本页是 http 页面被浏览器拦截。请打开高德/苹果地图, 长按布点位置读出经纬度, 手动填入并选择坐标系 GCJ-02。</small>';},
{enableHighAccuracy:true,timeout:20000,maximumAge:0});}
async function saveLoc(){const lat=document.getElementById('lat').value.trim(),lon=document.getElementById('lon').value.trim();
if(!lat||!lon){alert('先填入纬度、经度');return;}
const fd=new FormData();fd.append('lat',lat);fd.append('lon',lon);
fd.append('geo_sys',document.getElementById('geo_sys').value);
fd.append('loc_name',document.getElementById('loc_name').value);
fd.append('loc_acc',document.getElementById('loc_acc').value||'0');
fd.append('loc_time',document.getElementById('loc_time').value||Math.floor(Date.now()/1000));
try{await api('/api/config',{method:'POST',body:new URLSearchParams(fd)});
alert('位置已保存');loadConfig();loadStatus();}catch(e){alert('保存失败: '+e);}}
async function clearLoc(){askConfirm('清除已保存的位置？',async function(){
try{await api('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'clear_loc=1'});
alert('已清除');loadConfig();loadStatus();}catch(e){alert('清除失败: '+e);}});}
async function syncTime(){try{await api('/api/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'unix='+Math.floor(Date.now()/1000)});
document.getElementById('timeSyncNote').textContent='时间已同步 ✓ '+new Date().toLocaleString();
alert('对时成功');loadStatus();}catch(e){alert('对时失败: '+e);}}
async function autoSyncTime(){try{await api('/api/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'unix='+Math.floor(Date.now()/1000)});
document.getElementById('timeSyncNote').textContent='时间已同步 ✓ '+new Date().toLocaleString();loadStatus();}catch(e){document.getElementById('timeSyncNote').textContent='自动同步失败（'+e+'），请点下方「用手机时间同步」重试';}}
async function uploadFw(){const f=document.getElementById('fwFile').files[0];if(!f){alert('先选择 .bin 文件');return;}
askConfirm('确认升级固件？升级过程请勿断电',async function(){
const fd=new FormData();fd.append('firmware',f);
await fetch('/api/firmware',{method:'POST',body:fd});alert('已上传, 设备正在重启');});}
async function reboot(){askConfirm('重启设备？',async function(){await fetch('/api/reboot',{method:'POST'});alert('已发送重启命令');});}
async function formatSd(){askConfirm('清空将删除 SD 卡全部文件，确认吗？',async function(){
try{await api('/api/format',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'confirm=FORMAT'});
document.getElementById('formatNote').textContent='已清空';loadStatus();}catch(e){document.getElementById('formatNote').textContent='清空失败：'+e;}});}
function showFormatCard(){document.getElementById('formatCardBox').style.display='block';document.getElementById('formatCardResult').textContent='';}
function hideFormatCard(){document.getElementById('formatCardBox').style.display='none';document.getElementById('formatInput').value='';}
async function doFormatCard(){const v=document.getElementById('formatInput').value.trim();
const n=document.getElementById('formatCardResult');
if(v!=='FORMAT'){n.textContent='请准确输入 FORMAT 再执行';return;}
n.textContent='正在格式化，请等待（约数秒至数十秒），请勿断电…';
try{await api('/api/format_card',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'confirm=FORMAT'});
hideFormatCard();n.textContent='格式化成功，设备设置已恢复';loadStatus();loadLog();}catch(e){n.textContent='格式化失败：'+e+'（可重试，或在电脑上格式化为 FAT32）';}}
let pendingAction=null;
function askConfirm(text,fn){pendingAction=fn;document.getElementById('confirmText').textContent=text;document.getElementById('confirmBar').style.display='block';window.scrollTo(0,0);}
function hideConfirm(){pendingAction=null;document.getElementById('confirmBar').style.display='none';}
document.getElementById('confirmYes').onclick=async function(){const fn=pendingAction;hideConfirm();if(fn)await fn();};
async function loadLog(){try{const l=await api('/api/log');document.getElementById('logBox').textContent=l.join('\n');}catch(e){}}
let userTouched=false;
['pointerdown','keydown','touchstart'].forEach(function(t){document.addEventListener(t,function(){userTouched=true;},{passive:true});});
function dismissAutoFocus(){try{const a=document.activeElement;
if(!userTouched&&a&&(a.tagName==='INPUT'||a.tagName==='SELECT'||a.tagName==='TEXTAREA'))a.blur();}catch(e){}}
dismissAutoFocus();setTimeout(dismissAutoFocus,600);
const textInputs=Array.from(document.querySelectorAll('input[type="text"],input[type="number"]'));
textInputs.forEach(function(el){el.setAttribute('readonly','');});
function unlockInputs(){textInputs.forEach(function(el){el.removeAttribute('readonly');});}
document.addEventListener('pointerdown',unlockInputs,{once:true,passive:true});
document.addEventListener('touchstart',unlockInputs,{once:true,passive:true});
document.getElementById('gainRange').addEventListener('input',function(){document.getElementById('gain').value=this.value;document.getElementById('gainVal').textContent='× '+this.value;});
document.getElementById('gain').addEventListener('input',function(){document.getElementById('gainRange').value=this.value;document.getElementById('gainVal').textContent='× '+this.value;});
loadStatus();loadConfig();loadLog();autoSyncTime();
</script></body></html>
)HTML";
