#pragma once
/*
 * Audiobird 网页 App (单文件, 存入 PROGMEM, 由固件 / 路由返回)
 * 功能: 状态 / 配置 / 对时 / 文件 / 系统, 调用 /api/* 接口
 */
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Audiobird</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;max-width:640px;margin:0 auto;padding:12px;background:#f6f8fa;color:#222}
h1{font-size:22px}h2{font-size:17px;margin-top:22px}
.card{background:#fff;border:1px solid #ddd;border-radius:10px;padding:14px;margin:10px 0}
label{display:block;margin:10px 0 4px;color:#444}
input,select{width:100%;padding:9px;border:1px solid #ccc;border-radius:6px;font-size:15px;box-sizing:border-box}
button{background:#0969da;color:#fff;border:0;border-radius:6px;padding:10px 16px;font-size:15px;margin:6px 4px 0 0;cursor:pointer}
button.danger{background:#cf222e}button.sec{background:#57606a}
table{width:100%;border-collapse:collapse;font-size:14px}td,th{padding:6px 4px;border-bottom:1px solid #eee;text-align:left}
.mono{font-family:monospace;font-size:13px;white-space:pre-wrap;background:#f6f8fa;padding:8px;border-radius:6px}
small{color:#666}
</style></head><body>
<h1>🐦 Audiobird</h1><p id="statusLine">正在连接设备…</p>

<div class="card"><h2>状态</h2>
<div id="statusBox">加载中…</div>
<button onclick="loadStatus()">刷新状态</button></div>

<div class="card"><h2>配置</h2>
<label>采样率</label><select id="sample_rate">
<option value="8000">8000 Hz</option><option value="16000">16000 Hz</option>
<option value="32000">32000 Hz</option><option value="48000">48000 Hz</option></select>
<label>数字增益 (1–32)</label><input id="gain" type="number" min="1" max="32">
<label>录制时长 (秒)</label><input id="record_seconds" type="number" min="10" max="3600">
<label>休眠间隔 (秒, 0=连续录制)</label><input id="sleep_seconds" type="number" min="0" max="86400">
<label>录制时间表 (如 05:00-08:00,17:00-19:00, 为空=全天)</label><input id="schedule" type="text" placeholder="05:00-08:00,17:00-19:00">
<label>设备名</label><input id="device_name" type="text">
<button onclick="saveConfig()">保存配置</button>
<small>保存后设备重启进入工作模式。</small></div>

<div class="card"><h2>录制地点 (GPS)</h2>
<p id="locNow">尚未设置位置</p>
<label>粘贴坐标 (推荐): 在地图 App 长按布点位置 → 分享/复制, 回来粘到这里</label>
<input id="pasteBox" type="text" placeholder="粘贴地图分享文本、链接或 31.284512, 121.501234">
<button onclick="parsePaste()">解析粘贴内容</button>
<p id="pasteMsg"></p>
<button class="sec" onclick="getPos()">📍 尝试浏览器自动定位</button>
<p id="geoMsg"><small>浏览器定位只允许在 HTTPS 页面使用, 本页是局域网 http 页面, 多数浏览器会拒绝 (这不是设备故障)。推荐用上方粘贴: 高德/苹果地图长按位置 → 分享 → 复制, 粘贴后自动解析并按来源选坐标系。iPhone 也可以打开自带「指南针」, 底部直接显示经纬度 (WGS-84)。</small></p>
<label>纬度 (如 31.284512)</label><input id="lat" type="text" inputmode="decimal" placeholder="31.284512">
<label>经度 (如 121.501234)</label><input id="lon" type="text" inputmode="decimal" placeholder="121.501234">
<label>坐标系</label><select id="geo_sys">
<option value="WGS84">WGS-84 (GPS 原始坐标)</option>
<option value="GCJ02">GCJ-02 (高德 / 腾讯地图)</option>
<option value="BD09">BD-09 (百度地图)</option></select>
<label>地点名 (如: 同济樱花林东侧)</label><input id="loc_name" type="text" placeholder="便于后期分析时识别">
<input id="loc_acc" type="hidden"><input id="loc_time" type="hidden">
<button onclick="saveLoc()">仅保存位置</button>
<button class="sec" onclick="clearLoc()">清除位置</button><br>
<small>位置保存在 SD 卡 CONFIG.TXT; 之后每条录音都会在 ABIRD_LOG.CSV 里记一行 (时间 / 文件名 / 坐标 / 坐标系 / 地点名), 后期分析时按文件名与识别结果对应。</small></div>

<div class="card"><h2>对时</h2>
<p>设备时间: <span id="devTime">—</span></p>
<button onclick="syncTime()">用手机时间同步</button></div>

<div class="card"><h2>录音文件</h2>
<div id="filesBox">加载中…</div>
<button onclick="loadFiles()">刷新文件</button></div>

<div class="card"><h2>系统</h2>
<label>固件升级 (.bin)</label><input id="fwFile" type="file" accept=".bin">
<button onclick="uploadFw()">上传并升级</button><br>
<button class="sec" onclick="reboot()">重启设备</button>
<button class="danger" onclick="formatSd()">清空 SD 卡</button><br>
<small>清空 = 删除全部文件 (真正格式化请在电脑上操作)。</small>
<h2>日志</h2><div class="mono" id="logBox">—</div>
<button class="sec" onclick="loadLog()">刷新日志</button></div>

<script>
async function api(p,o){const r=await fetch(p,o);if(!r.ok)throw new Error(r.status);return r.json();}
function fmtTime(u){if(!u)return '未对时';return new Date(u*1000).toLocaleString();}
async function loadStatus(){try{const s=await api('/api/status');
document.getElementById('statusLine').textContent='已连接 · 固件 v'+s.firmware+' · 模式: '+s.mode;
document.getElementById('statusBox').innerHTML=
'设备: '+s.device+'<br>电量: '+(s.battery_mv>0?s.battery_mv+' mV ('+s.battery_pct+'%)':'未接检测电路')+
'<br>存储: '+Math.round(s.sd_free/1024)+' MB 可用 / '+Math.round(s.sd_total/1024)+' MB'+
'<br>录音文件: '+s.file_count+' 个<br>采样率: '+s.sample_rate+' Hz · 增益: '+s.gain+
' · 录制: '+s.record_seconds+'s · 休眠: '+s.sleep_seconds+'s'+
(s.has_loc?'<br>位置: '+s.lat+', '+s.lon+' ('+s.geo_sys+') '+s.loc_name:'<br>位置: 未设置');
document.getElementById('devTime').textContent=fmtTime(s.time);
}catch(e){document.getElementById('statusLine').textContent='连接失败: '+e;}}
async function loadConfig(){try{const c=await api('/api/config');
for(const k of ['sample_rate','gain','record_seconds','sleep_seconds','schedule','device_name'])
document.getElementById(k).value=c[k]??'';
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
// 1) Google 地图链接 @lat,lon 形式
let m=t.match(/@(-?\d{1,3}\.\d+),(-?\d{1,3}\.\d+)/);
if(m){lat=parseFloat(m[1]);lon=parseFloat(m[2]);sys='WGS84';}
// 2) 任意文本里的两个带小数数字
if(lat===null){const nums=t.match(/-?\d{1,3}\.\d{3,}/g);
if(nums&&nums.length>=2){let a=parseFloat(nums[0]),b=parseFloat(nums[1]);
// 经度数值通常更大: 若第一个像经度 (绝对值>90), 则输入是 经,纬 顺序
if(Math.abs(a)>90&&Math.abs(b)<=90){lon=a;lat=b;}else{lat=a;lon=b;}}}
if(lat===null||isNaN(lat)||isNaN(lon)||Math.abs(lat)>90||Math.abs(lon)>180){
pm.innerHTML='<small>没有解析出有效的经纬度, 请检查粘贴内容, 或直接在下方手动填写。</small>';return;}
// 3) 按来源猜坐标系: 高德/腾讯=GCJ02, 百度=BD09, Google/GPS=WGS84
if(/百度|baidu/i.test(t))sys='BD09';
else if(/高德|amap|腾讯地图/i.test(t))sys='GCJ02';
else if(/google|谷歌/i.test(t))sys='WGS84';
document.getElementById('lat').value=lat.toFixed(6);
document.getElementById('lon').value=lon.toFixed(6);
if(sys)document.getElementById('geo_sys').value=sys;
document.getElementById('loc_time').value=Math.floor(Date.now()/1000);
pm.innerHTML='<small>已解析: 纬度 '+lat.toFixed(6)+', 经度 '+lon.toFixed(6)+(sys?'，坐标系 '+sys:'')+'。核对无误后点「仅保存位置」。</small>';}
async function getPos(){const m=document.getElementById('geoMsg');
if(!navigator.geolocation){m.innerHTML='<small>此浏览器不支持定位, 请手动填入坐标。</small>';return;}
m.innerHTML='<small>正在定位, 请允许浏览器使用位置…</small>';
navigator.geolocation.getCurrentPosition(function(p){
document.getElementById('lat').value=p.coords.latitude.toFixed(6);
document.getElementById('lon').value=p.coords.longitude.toFixed(6);
document.getElementById('geo_sys').value='WGS84';
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
async function clearLoc(){if(!confirm('清除已保存的位置?'))return;
try{await api('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'clear_loc=1'});
alert('已清除');loadConfig();loadStatus();}catch(e){alert('清除失败: '+e);}}
async function syncTime(){try{await api('/api/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'unix='+Math.floor(Date.now()/1000)});
alert('对时成功');loadStatus();}catch(e){alert('对时失败: '+e);}}
async function loadFiles(){try{const fs=await api('/api/files');
if(!fs.length){document.getElementById('filesBox').innerHTML='暂无录音文件';return;}
let h='<table><tr><th>文件名</th><th>大小</th><th>时长</th><th></th></tr>';
for(const f of fs)h+='<tr><td>'+f.name+'</td><td>'+(f.size/1024).toFixed(0)+' KB</td><td>'+Math.round(f.duration_est)+'s</td><td><a href="/api/files/'+f.name+'">下载</a> · <a href="#" onclick="delFile(\''+f.name+'\');return false;">删除</a></td></tr>';
document.getElementById('filesBox').innerHTML=h+'</table>';}catch(e){document.getElementById('filesBox').textContent='加载失败: '+e;}}
async function delFile(n){if(!confirm('删除 '+n+' ?'))return;
await fetch('/api/files/'+n,{method:'DELETE'});loadFiles();}
async function uploadFw(){const f=document.getElementById('fwFile').files[0];if(!f){alert('先选择 .bin 文件');return;}
if(!confirm('确认升级固件? 升级过程请勿断电'))return;
const fd=new FormData();fd.append('firmware',f);
await fetch('/api/firmware',{method:'POST',body:fd});alert('已上传, 设备正在重启');}
async function reboot(){if(!confirm('重启设备?'))return;await fetch('/api/reboot',{method:'POST'});alert('已发送重启命令');}
async function formatSd(){const c=prompt('清空将删除 SD 卡全部文件! 输入 FORMAT 确认:');
if(c!=='FORMAT')return;await api('/api/format',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'confirm=FORMAT'});
alert('已清空');loadFiles();}
async function loadLog(){try{const l=await api('/api/log');document.getElementById('logBox').textContent=l.join('\n');}catch(e){}}
loadStatus();loadConfig();loadFiles();loadLog();
</script></body></html>
)HTML";
