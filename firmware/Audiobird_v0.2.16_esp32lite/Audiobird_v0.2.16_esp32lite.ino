/*
 * Audiobird v0.2.12-esp32lite — ESP32 经典款野外鸟声记录仪固件 (带网页 App, 由 S3 版 v0.2 移植)
 * ============================================================================
 * v0.2.12 (相对 v0.2.11):
 *   - 配置页去掉录音文件列表: 文件多时 /api/status 每刷必全目录扫描 +
 *     大 JSON 拼装是页面卡顿的根因, 连同 /api/files 列表/下载/删除路由
 *     一并删除; web 只做设置, 文件改读卡; 状态区不再显示文件数
 *   - 开机默认进配置: 上电复位 (POWERON, 含 RST 键) 直接进配置, 不再等
 *     GPIO0 窗口; 配置态 2 分钟无 Wi-Fi 客户端接入自动回工作模式
 *     (软件重启, 不会死循环回配置); 有客户端接入过则沿用 5 分钟无操作定时器
 * v0.2.10 (相对 v0.2.9):
 *   - 录音改读右声道: 自检实测 INMP441 数据在右声道时隙 (B 轮高 16 位最大 7111,
 *     左声道高位全 0), ONLY_LEFT 改 ONLY_RIGHT, 其余 I2S 参数不动; 自检功能保留
 *   - 配置页自动对时: 页面加载时自动把手机当前时间发给设备写 DS3231/软件时钟,
 *     Captive Portal 弹出的就是同一配置页, 连上 Wi-Fi 即自动同步 (附合理范围校验)
 * 新增 (相对 v0.1):
 *   - 配置模式: ESP32 开 AP 热点, 手机浏览器打开 http://192.168.4.1
 *     即可配置 / 对时 / OTA 升级 (详见 docs/app-design.md)
 *   - 工作模式: WiFi 关闭, 按配置循环录音, deep sleep 省电
 *   - 录制时间表 SCHEDULE (需对时, 否则忽略时间表)
 *   - DS3231 RTC (可选, 有则用硬件时钟, 无则用软件时钟估算)
 *   - 电池电压检测 (分压 + ADC)
 *   - 固件 OTA 升级, SD 卡清空 (录音文件改读卡, 不走 web)
 *
 * 两种模式:
 *   - 配置模式: 上电默认进 (RST 键复位亦然); 深睡唤醒与软件重启走工作模式;
 *     CONFIG.TXT 中 FORCE_CONFIG=1 仍可强制进配置 (冗余保留)
 *   - 工作模式: 默认; 配置模式 2 分钟无 Wi-Fi 接入或 5 分钟无操作自动回到工作模式
 *
 * 开发环境: Arduino IDE + ESP32 Arduino Core 2.0.x
 *           开发板 "ESP32 Dev Module"; 若上传失败, 把上传速度降到 115200
 * 硬件: LOLIN32 Lite 版型 ESP32 板 (无 BOOT 键/GPIO21 未引出) + INMP441 + MicroSD (SPI) + 可选 DS3231, 完整接线见 README 硬件连接表
 *
 * 开源许可: MIT License
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <SD.h>
#include <ff.h>  // v0.2.15: TF 卡格式化用 f_mount / f_mkfs (Core 自带 FatFs)
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>  // v0.2.8: Captive Portal 自动弹页
#include <Update.h>
#include <Wire.h>
#include <time.h>
#include "esp_sleep.h"
#include "web_ui.h"

/* v0.2.1 修复: loopTask 默认栈只有 8192 B, recordToWav + SD/FatFS 深调用链会触发
 * Stack canary watchpoint (Guru Meditation) — 把 loopTask 栈加大到 16 KB。
 * 这是覆盖 Core 2.0.17 main.cpp 里的弱符号函数, Arduino 官方留的口子。 */
size_t getArduinoLoopTaskStackSize(void) { return 16384; }

/* ---------------- 引脚定义 (ESP32 经典款, 避开 strapping/Flash/串口引脚) ---------------- */
#define I2S_BCK_PIN   14    // INMP441 SCK
#define I2S_WS_PIN    27    // INMP441 WS
#define I2S_DIN_PIN   26    // INMP441 SD
#define SD_SCK_PIN    18
#define SD_MISO_PIN   19
#define SD_MOSI_PIN   23
#define SD_CS_PIN     5
#define LED_PIN       22  // v0.2.2: Lite 版型板载 LED 在 GPIO22 (原 DevKit 的 GPIO2 此板无 LED)
#define LED_ON_LEVEL  LOW   // v0.2.2: Lite 版型板载 LED 低电平点亮 (3V3->LED->GPIO22)
#define LED_OFF_LEVEL HIGH
static inline void ledSet(bool on) { if (LED_PIN >= 0) digitalWrite(LED_PIN, on ? LED_ON_LEVEL : LED_OFF_LEVEL); }
#define RTC_SDA_PIN   33    // v0.2.2: 此板未引出 GPIO21! DS3231 SDA 改到 GPIO33 (ESP32 I2C 可任意映射)
#define RTC_SCL_PIN   32    // v0.2.2: DS3231 SCL 改到 GPIO32, 与 33 相邻
#define BAT_ADC_PIN   35    // v0.2.2: Lite 版型板载电池分压 (JST 电池座, 100k/100k) 在 GPIO35; 原 GPIO34 此板为裸引脚
#define CONFIG_BTN_PIN 0   // BOOT 键 (v0.2.12 起不再用于进配置, 上电默认进配置; 保留定义防旧引用)

/* ---------------- 默认配置 ---------------- */
#define DEFAULT_SAMPLE_RATE  16000  // v0.2.16: 默认采样率改 16kHz
#define DEFAULT_RECORD_SEC   300    // v0.2.16: 默认录制时长改 5 分钟
#define DEFAULT_SLEEP_SEC    3300   // v0.2.16: 默认录制间隔改 55 分钟
#define DEFAULT_GAIN         4
#define DEFAULT_DEVICE_NAME  "Audiobird-01"
#define CONFIG_MODE_TIMEOUT_MS (5UL * 60UL * 1000UL)  // 配置模式 5 分钟无操作自动退出
#define CONFIG_NO_CLIENT_TIMEOUT_MS (2UL * 60UL * 1000UL)  // v0.2.12: 配置态 2 分钟无 Wi-Fi 客户端接入自动回工作
#define AP_PASSWORD "audiobird"
#define FIRMWARE_VERSION "0.2.16"

struct Config {
  uint32_t sampleRate = DEFAULT_SAMPLE_RATE;
  uint32_t recordSec  = DEFAULT_RECORD_SEC;
  uint32_t sleepSec   = DEFAULT_SLEEP_SEC;
  uint32_t gain       = DEFAULT_GAIN;
  String deviceName   = DEFAULT_DEVICE_NAME;
  int schedStart[4]   = {0, 0, 0, 0};   // 时间表: 每日分钟数 (0-1439)
  int schedEnd[4]     = {0, 0, 0, 0};
  int schedCount      = 0;
  bool forceConfig    = false;
  /* v0.2.6: 录制地点 (配置模式下由手机写入, 一次设定长期有效) */
  float  lat = 0, lon = 0;   // 纬度 / 经度 (十进制度)
  String geoSys;             // 坐标系: WGS84 / GCJ02 / BD09
  String locName;            // 地点名 (如: 同济樱花林东侧)
  int    locAcc  = 0;        // 定位精度 (米, 0 = 手动输入/未知)
  uint32_t locTime = 0;      // 定位时间 (unix 秒)
  bool   hasLoc  = false;
};

/* deep sleep 后保持的状态 */
RTC_DATA_ATTR uint32_t g_fileIndex = 0;
RTC_DATA_ATTR uint32_t g_bootCount = 0;
RTC_DATA_ATTR uint32_t g_softEpoch = 0;       // 软件时钟: 上次对时后的 epoch 基准
RTC_DATA_ATTR uint32_t g_lastSleepSec = 0;    // 上次休眠时长 (唤醒后累进软件时钟)
RTC_DATA_ATTR bool g_softTimeValid = false;

Config g_cfg;
bool g_rtcPresent = false;
bool g_configMode = false;
WebServer server(80);
DNSServer dnsServer;  // v0.2.8: 配置热点 DNS 全解析 -> 192.168.4.1
unsigned long g_lastClientActivity = 0;
unsigned long g_configStartMs = 0;      // v0.2.12: 进配置时刻 (无人接入超时起点)
bool g_configClientEverSeen = false;   // v0.2.12: 配置态内是否曾有 Wi-Fi 客户端接入

/* ---------------- 日志 (串口 + 内存镜像, 供 /api/log) ---------------- */
#define LOG_MAX 30
String g_logLines[LOG_MAX];
int g_logCount = 0;
void logLine(const String &s) {
  Serial.println(s);
  if (g_logCount < LOG_MAX) g_logLines[g_logCount++] = s;
  else {
    for (int i = 1; i < LOG_MAX; i++) g_logLines[i - 1] = g_logLines[i];
    g_logLines[LOG_MAX - 1] = s;
  }
}
void touch() { g_lastClientActivity = millis(); }

/* ---------------- WAV 文件头 ---------------- */
void writeWavHeader(File &f, uint32_t sampleRate, uint32_t dataBytes) {
  uint32_t byteRate  = sampleRate * 2;
  uint32_t chunkSize = 36 + dataBytes;
  uint32_t fmtSize   = 16;
  uint16_t audioFormat = 1, channels = 1, blockAlign = 2, bits = 16;
  f.write((const uint8_t *)"RIFF", 4);
  f.write((uint8_t *)&chunkSize, 4);
  f.write((const uint8_t *)"WAVE", 4);
  f.write((const uint8_t *)"fmt ", 4);
  f.write((uint8_t *)&fmtSize, 4);
  f.write((uint8_t *)&audioFormat, 2);
  f.write((uint8_t *)&channels, 2);
  f.write((uint8_t *)&sampleRate, 4);
  f.write((uint8_t *)&byteRate, 4);
  f.write((uint8_t *)&blockAlign, 2);
  f.write((uint8_t *)&bits, 2);
  f.write((const uint8_t *)"data", 4);
  f.write((uint8_t *)&dataBytes, 4);
}

/* ---------------- CONFIG.TXT 读写 (v0.2 格式, 向后兼容 v0.1) ---------------- */
bool parseSchedule(const String &v) {
  g_cfg.schedCount = 0;
  int start = 0;
  while (start < (int)v.length() && g_cfg.schedCount < 4) {
    int comma = v.indexOf(',', start);
    String item = (comma < 0) ? v.substring(start) : v.substring(start, comma);
    item.trim();
    int dash = item.indexOf('-');
    if (dash > 0) {
      int sh = item.substring(0, dash).toInt();
      int sm = 0;
      int c1 = item.indexOf(':');
      if (c1 >= 0 && c1 < dash) { sh = item.substring(0, c1).toInt(); sm = item.substring(c1 + 1, dash).toInt(); }
      String endPart = item.substring(dash + 1);
      int eh = endPart.toInt(), em = 0;
      int c2 = endPart.indexOf(':');
      if (c2 >= 0) { eh = endPart.substring(0, c2).toInt(); em = endPart.substring(c2 + 1).toInt(); }
      g_cfg.schedStart[g_cfg.schedCount] = sh * 60 + sm;
      g_cfg.schedEnd[g_cfg.schedCount]   = eh * 60 + em;
      g_cfg.schedCount++;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  return true;
}

void loadConfig() {
  File f = SD.open("/CONFIG.TXT", FILE_READ);
  if (!f) { logLine("[CFG] 无 CONFIG.TXT, 使用默认配置"); return; }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq); key.trim(); key.toUpperCase();
    String val = line.substring(eq + 1); val.trim();
    if (key == "SAMPLE_RATE")         g_cfg.sampleRate = val.toInt();
    else if (key == "RECORD_SECONDS")  g_cfg.recordSec  = val.toInt();
    else if (key == "SLEEP_SECONDS")   g_cfg.sleepSec   = val.toInt();
    else if (key == "GAIN")            g_cfg.gain       = val.toInt();
    else if (key == "DEVICE_NAME")      g_cfg.deviceName = val;
    else if (key == "SCHEDULE")         parseSchedule(val);
    else if (key == "FORCE_CONFIG")     g_cfg.forceConfig = (val == "1");
    else if (key == "LAT")              g_cfg.lat = val.toFloat();
    else if (key == "LON")              g_cfg.lon = val.toFloat();
    else if (key == "GEO_SYS")          g_cfg.geoSys = val;
    else if (key == "LOC_NAME")         g_cfg.locName = val;
    else if (key == "LOC_ACC")          g_cfg.locAcc = val.toInt();
    else if (key == "LOC_TIME")         g_cfg.locTime = (uint32_t)val.toInt();
  }
  f.close();
  if (g_cfg.sampleRate < 8000)  g_cfg.sampleRate = 8000;
  if (g_cfg.sampleRate > 48000) g_cfg.sampleRate = 48000;
  if (g_cfg.recordSec == 0) g_cfg.recordSec = DEFAULT_RECORD_SEC;
  if (g_cfg.gain == 0)      g_cfg.gain = 1;
  if (g_cfg.gain > 32)      g_cfg.gain = 32;
  if (g_cfg.deviceName.length() == 0) g_cfg.deviceName = DEFAULT_DEVICE_NAME;
  g_cfg.hasLoc = (g_cfg.lat >= -90.0f && g_cfg.lat <= 90.0f &&
                  g_cfg.lon >= -180.0f && g_cfg.lon <= 180.0f &&
                  !(g_cfg.lat == 0 && g_cfg.lon == 0));
  if (g_cfg.hasLoc) {
    if (g_cfg.geoSys.length() == 0) g_cfg.geoSys = "WGS84";
    logLine("[GEO] 录制地点: " + String(g_cfg.lat, 6) + ", " + String(g_cfg.lon, 6)
            + " (" + g_cfg.geoSys + ") " + g_cfg.locName);
  }
  logLine("[CFG] 已加载 CONFIG.TXT");
}

void saveConfig() {
  SD.remove("/CONFIG.TXT");
  File f = SD.open("/CONFIG.TXT", FILE_WRITE);
  if (!f) { logLine("[CFG] 保存 CONFIG.TXT 失败!"); return; }
  f.println("# Audiobird 配置文件 (v0.2)");
  f.printf("SAMPLE_RATE=%u\n", g_cfg.sampleRate);
  f.printf("RECORD_SECONDS=%u\n", g_cfg.recordSec);
  f.printf("SLEEP_SECONDS=%u\n", g_cfg.sleepSec);
  f.printf("GAIN=%u\n", g_cfg.gain);
  String sch;
  for (int i = 0; i < g_cfg.schedCount; i++) {
    if (i) sch += ",";
    sch += String(g_cfg.schedStart[i] / 60) + ":" + String(g_cfg.schedStart[i] % 60)
         + "-" + String(g_cfg.schedEnd[i] / 60) + ":" + String(g_cfg.schedEnd[i] % 60);
  }
  f.printf("SCHEDULE=%s\n", sch.c_str());
  f.printf("DEVICE_NAME=%s\n", g_cfg.deviceName.c_str());
  f.printf("FORCE_CONFIG=%d\n", g_cfg.forceConfig ? 1 : 0);
  if (g_cfg.hasLoc) {
    f.printf("LAT=%.6f\n", g_cfg.lat);
    f.printf("LON=%.6f\n", g_cfg.lon);
    f.printf("GEO_SYS=%s\n", g_cfg.geoSys.c_str());
    f.printf("LOC_NAME=%s\n", g_cfg.locName.c_str());
    f.printf("LOC_ACC=%d\n", g_cfg.locAcc);
    f.printf("LOC_TIME=%u\n", g_cfg.locTime);
  }
  f.close();
  logLine("[CFG] 已保存 CONFIG.TXT");
}

/* ---------------- 断电续写: 扫描已有文件取最大序号 +1 ---------------- */
void resolveFileIndex() {
  uint32_t scanStart = millis();
  File dir = SD.open("/");
  if (!dir) return;
  uint32_t maxIdx = 0;
  bool found = false;
  bool scanLed = false;
  while (true) {
    uint32_t stepStart = millis();
    File f = dir.openNextFile();
    if (!f) break;
    String name = f.name();
    int slash = name.lastIndexOf('/');  // ESP32 上 name() 可能带 "/" 前缀, 先取 basename
    if (slash >= 0) name = name.substring(slash + 1);
    f.close();
    /* v0.2.5: 逐个打印扫描进度+单步耗时 (坏卡/FAT 簇链损坏时能看出卡在哪个文件),
       并翻转 LED 作"还活着"的反馈 — v0.2.4 实测坏卡扫描要 64 秒, 全程无声无息。 */
    scanLed = !scanLed; ledSet(scanLed);
    logLine("[SD] 扫描到 " + name + " (" + String(millis() - stepStart) + "ms)");
    if (name.startsWith("ABIRD_") && name.endsWith(".WAV")) {
      uint32_t idx = name.substring(6, 10).toInt();
      if (!found || idx >= maxIdx) { maxIdx = idx; found = true; }
    }
  }
  dir.close();
  if (found) g_fileIndex = maxIdx + 1;
  logLine("[SD] 断电续写: 下一文件序号 " + String(g_fileIndex) + " (扫描耗时 " + String(millis() - scanStart) + "ms)");
}

/* ---------------- DS3231 RTC (可选, Wire 直接读写寄存器, 无需额外库) ---------------- */
uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtcDetect() {
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Wire.beginTransmission(0x68);
  return Wire.endTransmission() == 0;
}

uint32_t rtcReadUnix() {
  Wire.beginTransmission(0x68);
  Wire.write(0x00);
  Wire.endTransmission();
  Wire.requestFrom(0x68, 7);
  if (Wire.available() < 7) return 0;
  uint8_t sec  = bcd2dec(Wire.read() & 0x7F);
  uint8_t min_ = bcd2dec(Wire.read());
  uint8_t hr   = bcd2dec(Wire.read() & 0x3F);
  Wire.read();  // 星期, 不用
  uint8_t date = bcd2dec(Wire.read());
  uint8_t mon  = bcd2dec(Wire.read());
  uint8_t yr   = bcd2dec(Wire.read());
  struct tm t = {0};
  t.tm_sec = sec; t.tm_min = min_; t.tm_hour = hr;
  t.tm_mday = date; t.tm_mon = mon - 1; t.tm_year = yr + 100;
  return (uint32_t)mktime(&t);
}

void rtcWriteUnix(uint32_t unix) {
  time_t tt = (time_t)unix;
  struct tm t;
  gmtime_r(&tt, &t);
  Wire.beginTransmission(0x68);
  Wire.write(0x00);
  Wire.write(dec2bcd(t.tm_sec));
  Wire.write(dec2bcd(t.tm_min));
  Wire.write(dec2bcd(t.tm_hour));
  Wire.write(dec2bcd(t.tm_wday + 1));
  Wire.write(dec2bcd(t.tm_mday));
  Wire.write(dec2bcd(t.tm_mon + 1));
  Wire.write(dec2bcd(t.tm_year - 100));
  Wire.endTransmission();
}

bool timeIsValid() { return g_rtcPresent || g_softTimeValid; }

uint32_t nowUnix() {
  if (g_rtcPresent) return rtcReadUnix();
  if (g_softTimeValid) return g_softEpoch + (millis() / 1000);
  return 0;
}

void setTimeUnix(uint32_t unix) {
  if (g_rtcPresent) rtcWriteUnix(unix);
  g_softEpoch = unix - (millis() / 1000);  // 软件时钟基准: 扣除已开机时间
  g_softTimeValid = true;
}

/* ---------------- 电池检测 (2:1 分压 + ADC) ---------------- */
int readBatteryMv() {
  if (BAT_ADC_PIN < 0) return -1;
  int mv = analogReadMilliVolts(BAT_ADC_PIN);
  return mv * 2;
}
int batteryPct(int mv) {
  if (mv < 0) return -1;
  int p = (mv - 3300) * 100 / (4200 - 3300);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

/* ---------------- 时间表 ---------------- */
bool inSchedule(int nowMin) {
  if (g_cfg.schedCount == 0) return true;
  for (int i = 0; i < g_cfg.schedCount; i++) {
    int s = g_cfg.schedStart[i], e = g_cfg.schedEnd[i];
    if (s <= e) { if (nowMin >= s && nowMin < e) return true; }
    else { if (nowMin >= s || nowMin < e) return true; }  // 跨午夜时段
  }
  return false;
}

uint32_t secondsToNextWindow(int nowMin) {
  uint32_t best = 86400;
  for (int i = 0; i < g_cfg.schedCount; i++) {
    int s = g_cfg.schedStart[i];
    int diff = (s > nowMin) ? (s - nowMin) : (s + 1440 - nowMin);
    if ((uint32_t)diff * 60 < best) best = diff * 60;
  }
  return best + 30;  // 多睡 30 秒, 避免边界抖动
}

/* ---------------- I2S 麦克风与录音 (同 v0.1) ---------------- */
bool initMicrophone() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = (int)g_cfg.sampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    /* v0.2.10: 自检实测 INMP441 数据在右声道时隙 (B 轮高 16 位最大 7111,
     * A 轮左声道高位全 0), 故录音改读 ONLY_RIGHT; 其余参数一律不动。 */
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_DIN_PIN
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  return true;
}

bool recordToWav(const char *path) {
  /* v0.2.1: [REC] 日志提前到建文件之前, 方便定位崩溃点 */
  logLine("[REC] 开始录制 " + String(path) + ", " + String(g_cfg.sampleRate) + "Hz, " + String(g_cfg.recordSec) + "秒");
  Serial.printf("[DBG] loopTask stack high-water: %u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  File f = SD.open(path, FILE_WRITE);
  if (!f) { logLine("[REC] 无法创建文件"); return false; }
  writeWavHeader(f, g_cfg.sampleRate, 0);
  /* v0.2.1: raw/pcm 共 6 KB, 放在栈上会和 SD/FatFS 调用链一起撑爆 loopTask 栈
   * (v0.2.0 的 Guru Meditation 根因), 改为 static 放 BSS, 不占栈。 */
  static int32_t raw[1024];
  static int16_t pcm[1024];
  const size_t N = 1024;
  size_t bytesRead = 0;
  uint32_t totalSamples = g_cfg.sampleRate * g_cfg.recordSec;
  uint32_t written = 0;
  uint32_t lastBlink = 0;
  bool ledState = false;
  while (written < totalSamples) {
    size_t toRead = N;
    if (written + toRead > totalSamples) toRead = totalSamples - written;
    if (i2s_read(I2S_NUM_0, raw, toRead * sizeof(int32_t), &bytesRead, portMAX_DELAY) != ESP_OK) break;
    size_t n = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
      int32_t s = (raw[i] >> 16) * (int32_t)g_cfg.gain;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      pcm[i] = (int16_t)s;
    }
    f.write((uint8_t *)pcm, n * sizeof(int16_t));
    written += n;
    if (LED_PIN >= 0 && millis() - lastBlink > 500) {
      lastBlink = millis(); ledState = !ledState;
      ledSet(ledState);
    }
  }
  f.seek(0);
  writeWavHeader(f, g_cfg.sampleRate, written * sizeof(int16_t));
  f.close();
  i2s_driver_uninstall(I2S_NUM_0);
  ledSet(false);
  logLine("[REC] 完成, 共 " + String(written) + " 个采样点");
  return written > 0;
}

/* v0.2.6: 每条录音在 ABIRD_LOG.CSV 追加一行 (时间/文件/位置/参数/电量),
 * 这是后期数据分析的索引表: WAV 文件名 <-> 录制时间 <-> 地点 一一对应,
 * Excel / pandas 可直接打开, 也可与 AI 识别结果按文件名 join。 */
void appendRecLog(const char *path, uint32_t recStartUnix) {
  bool exists = SD.exists("/ABIRD_LOG.CSV");
  File f = SD.open("/ABIRD_LOG.CSV", FILE_APPEND);
  if (!f) { logLine("[GEO] ABIRD_LOG.CSV 写入失败"); return; }
  if (!exists) f.println("unix,datetime_utc,file,lat,lon,geo_sys,loc_name,sample_rate,record_seconds,battery_mv");
  char dt[24] = "";
  if (recStartUnix > 0) {
    time_t tt = (time_t)recStartUnix;
    struct tm t; gmtime_r(&tt, &t);
    strftime(dt, sizeof(dt), "%Y-%m-%dT%H:%M:%S", &t);
  }
  String name = g_cfg.locName; name.replace(",", " ");
  f.printf("%u,%s,%s,", recStartUnix, dt, path + 1);
  if (g_cfg.hasLoc) f.printf("%.6f,%.6f,%s,%s,", g_cfg.lat, g_cfg.lon, g_cfg.geoSys.c_str(), name.c_str());
  else f.print(",,,,");
  f.printf("%u,%u,%d\n", g_cfg.sampleRate, g_cfg.recordSec, readBatteryMv());
  f.close();
}

void goSleep(uint32_t sec) {
  g_lastSleepSec = sec;
  logLine("[PWR] 进入 deep sleep " + String(sec) + " 秒...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  esp_deep_sleep_start();
}

void blinkError() {
  if (LED_PIN < 0) return;
  for (int i = 0; i < 6; i++) {
    ledSet(true);  delay(150);
    ledSet(false); delay(150);
  }
}

/* ============================================================================
 * 网页 App 服务端 (配置模式)
 * API 见 docs/app-design.md §4, 参数用 form-encoded (与网页表单一致)
 * ========================================================================== */

String jsonEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c < 0x20) out += " ";
    else out += c;
  }
  return out;
}

void handleRoot() {
  touch();
  server.send_P(200, "text/html", INDEX_HTML);
}

/* v0.2.16: 仅数录音文件名, 不读文件内容、不拼 JSON、不排序。
 * 只数根目录 ABIRD_*.WAV (File::name() 可能带 "/" 前缀, 先剥掉;
 * macOS 的 ._ 文件与系统目录不在内)。比 v0.2.12 删掉的那个
 * 全目录扫描 + JSON 列表轻得多。 */
int countAudioFiles() {
  int n = 0;
  File dir = SD.open("/");
  if (!dir) return 0;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    String name = f.name();
    bool isDir = f.isDirectory();
    f.close();
    if (name.startsWith("/")) name = name.substring(1);
    if (!isDir && name.startsWith("ABIRD_") && (name.endsWith(".WAV") || name.endsWith(".wav"))) n++;
  }
  dir.close();
  return n;
}

void handleStatus() {
  touch();
  int mv = readBatteryMv();
  String j = "{";
  j += "\"device\":\"" + jsonEscape(g_cfg.deviceName) + "\",";
  j += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
  j += "\"mode\":\"config\",";
  j += "\"time\":" + String(nowUnix()) + ",";
  j += "\"battery_mv\":" + String(mv) + ",";
  j += "\"battery_pct\":" + String(batteryPct(mv)) + ",";
  j += "\"sd_total\":" + String((uint32_t)(SD.totalBytes() / 1024)) + ",";
  j += "\"sd_free\":" + String((uint32_t)(SD.totalBytes() / 1024 - SD.usedBytes() / 1024)) + ",";
  j += "\"audio_files\":" + String(countAudioFiles()) + ",";
  j += "\"sample_rate\":" + String(g_cfg.sampleRate) + ",";
  j += "\"gain\":" + String(g_cfg.gain) + ",";
  j += "\"record_seconds\":" + String(g_cfg.recordSec) + ",";
  j += "\"sleep_seconds\":" + String(g_cfg.sleepSec) + ",";
  j += "\"sched_count\":" + String(g_cfg.schedCount) + ",";
  j += "\"has_loc\":" + String(g_cfg.hasLoc ? "true" : "false") + ",";
  j += "\"lat\":" + String(g_cfg.lat, 6) + ",";
  j += "\"lon\":" + String(g_cfg.lon, 6) + ",";
  j += "\"geo_sys\":\"" + jsonEscape(g_cfg.geoSys) + "\",";
  j += "\"loc_name\":\"" + jsonEscape(g_cfg.locName) + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

void handleConfigGet() {
  touch();
  String j = "{";
  j += "\"sample_rate\":" + String(g_cfg.sampleRate) + ",";
  j += "\"record_seconds\":" + String(g_cfg.recordSec) + ",";
  j += "\"sleep_seconds\":" + String(g_cfg.sleepSec) + ",";
  j += "\"gain\":" + String(g_cfg.gain) + ",";
  j += "\"device_name\":\"" + jsonEscape(g_cfg.deviceName) + "\",";
  String sch;
  for (int i = 0; i < g_cfg.schedCount; i++) {
    if (i) sch += ",";
    sch += String(g_cfg.schedStart[i] / 60) + ":" + String(g_cfg.schedStart[i] % 60)
         + "-" + String(g_cfg.schedEnd[i] / 60) + ":" + String(g_cfg.schedEnd[i] % 60);
  }
  j += "\"schedule\":\"" + sch + "\",";
  j += "\"has_loc\":" + String(g_cfg.hasLoc ? "true" : "false") + ",";
  j += "\"lat\":\"" + String(g_cfg.lat, 6) + "\",";
  j += "\"lon\":\"" + String(g_cfg.lon, 6) + "\",";
  j += "\"geo_sys\":\"" + jsonEscape(g_cfg.geoSys) + "\",";
  j += "\"loc_name\":\"" + jsonEscape(g_cfg.locName) + "\",";
  j += "\"loc_acc\":" + String(g_cfg.locAcc) + ",";
  j += "\"loc_time\":" + String(g_cfg.locTime);
  j += "}";
  server.send(200, "application/json", j);
}

void handleConfigPost() {
  touch();
  if (server.hasArg("sample_rate"))  g_cfg.sampleRate = server.arg("sample_rate").toInt();
  if (server.hasArg("record_seconds")) g_cfg.recordSec = server.arg("record_seconds").toInt();
  if (server.hasArg("sleep_seconds"))  g_cfg.sleepSec  = server.arg("sleep_seconds").toInt();
  if (server.hasArg("gain"))           g_cfg.gain      = server.arg("gain").toInt();
  if (server.hasArg("device_name"))    g_cfg.deviceName = server.arg("device_name");
  if (server.hasArg("schedule"))       parseSchedule(server.arg("schedule"));
  /* v0.2.6: 位置字段 (可单独提交, 只更新位置不动其他配置) */
  if (server.hasArg("clear_loc") && server.arg("clear_loc") == "1") {
    g_cfg.hasLoc = false; g_cfg.lat = 0; g_cfg.lon = 0;
    g_cfg.geoSys = ""; g_cfg.locName = ""; g_cfg.locAcc = 0; g_cfg.locTime = 0;
  } else if (server.hasArg("lat") && server.hasArg("lon")) {
    float la = server.arg("lat").toFloat();
    float lo = server.arg("lon").toFloat();
    if (la >= -90.0f && la <= 90.0f && lo >= -180.0f && lo <= 180.0f && !(la == 0 && lo == 0)) {
      g_cfg.lat = la; g_cfg.lon = lo; g_cfg.hasLoc = true;
      if (server.hasArg("geo_sys"))  g_cfg.geoSys  = server.arg("geo_sys");
      if (server.hasArg("loc_name")) g_cfg.locName = server.arg("loc_name");
      if (server.hasArg("loc_acc"))  g_cfg.locAcc  = server.arg("loc_acc").toInt();
      if (server.hasArg("loc_time")) g_cfg.locTime = (uint32_t)server.arg("loc_time").toInt();
      if (g_cfg.geoSys.length() == 0) g_cfg.geoSys = "WGS84";
      logLine("[GEO] 位置已更新: " + String(la, 6) + ", " + String(lo, 6) + " (" + g_cfg.geoSys + ")");
    }
  }
  if (g_cfg.sampleRate < 8000)  g_cfg.sampleRate = 8000;
  if (g_cfg.sampleRate > 48000) g_cfg.sampleRate = 48000;
  if (g_cfg.gain == 0) g_cfg.gain = 1;
  if (g_cfg.gain > 32) g_cfg.gain = 32;
  if (g_cfg.deviceName.length() == 0) g_cfg.deviceName = DEFAULT_DEVICE_NAME;
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
  /* v0.2.9: v0.2.8 保存后只回包不重启, 设备永远停在 config 模式, 页面上
   * "保存后设备重启进入工作模式" 的文案成空话。现先回包再延时重启,
   * 浏览器能收到响应; 重启后 FORCE_CONFIG 已是 0 且 GPIO0 未短接,
   * setup() 必然走工作模式。注意: 仅保存位置 (saveLoc, 无 sample_rate
   * 字段) 不重启, 免得每次标个地点就断开配置页。 */
  if (server.hasArg("sample_rate")) {
    delay(500);
    ESP.restart();
  }
}

void handleTimePost() {
  touch();
  if (!server.hasArg("unix")) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing unix\"}"); return; }
  /* v0.2.10: 自动对时加合理范围校验 (2024-01-01 ~ 2035-12-31), 防误写坏时间。
   * 收到的是手机 Date.now()/1000 (UTC epoch 秒), 与日志的 UTC 时间基准一致。
   * v0.2.11 修正: 上限常量曾误写 1767225600 (= 2026-01-01, 非 2035 年),
   * 导致 2026 年后的所有真实时间都被 400 拒绝, 自动与手动对时全灭。 */
  uint32_t unix = (uint32_t)strtoul(server.arg("unix").c_str(), NULL, 10);
  if (unix < 1704067200UL || unix > 2082758399UL) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"unix out of range\"}"); return; }
  setTimeUnix(unix);
  server.send(200, "application/json", "{\"ok\":true,\"time\":" + String(nowUnix()) + "}");
}

void handleFirmwareUpload() {
  touch();
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    logLine("[OTA] 开始接收固件");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logLine("[OTA] Update.begin 失败");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) logLine("[OTA] 写入失败");
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) logLine("[OTA] 升级成功, 即将重启");
    else logLine("[OTA] 升级失败");
  }
}

void handleFirmwarePost() {
  touch();
  server.send(200, "application/json", Update.hasError() ? "{\"ok\":false}" : "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

void handleReboot() {
  touch();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

void handleFormat() {
  touch();
  if (!server.hasArg("confirm") || server.arg("confirm") != "FORMAT") {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"confirm required\"}");
    return;
  }
  // v0.2 实现: 删除全部文件 (真正格式化请在电脑上操作, 见 README)
  int deleted = 0;
  File dir = SD.open("/");
  if (dir) {
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      String name = f.name();
      bool isDir = f.isDirectory();
      f.close();
      if (!isDir) { if (SD.remove("/" + name)) deleted++; }
    }
    dir.close();
  }
  logLine("[SD] 已清空 " + String(deleted) + " 个文件");
  server.send(200, "application/json", "{\"ok\":true,\"deleted\":" + String(deleted) + "}");
}

/* v0.2.15: TF 卡真格式化 (区别于删除全部文件)。
 * 顺序是关键, 直接 SD.end() 再 f_mkfs 会失败, 因为 end() 会注销
 * diskio 驱动。正确顺序: f_mount(NULL) 先卸掉 FatFs 挂载 (VFS 注册
 * 与 diskio 保持, f_mkfs 可用) -> f_mkfs("0:", FM_ANY) (SPI SD 为
 * pdrv 0; 本 Core 的 FatFs 关了 exFAT, 大卡格成 FAT32) ->
 * SD.end() + SD.begin() 重新挂载新文件系统 -> saveConfig() 把内存里
 * 的配置写回新卡 (录音与日志无法恢复, 页面已警告)。
 * 格式化中途断电可能导致卡无文件系统, 可重格或电脑格。 */
void handleFormatCard() {
  touch();
  if (!server.hasArg("confirm") || server.arg("confirm") != "FORMAT") {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"confirm required\"}");
    return;
  }
  f_mount(NULL, "0:", 0);
  BYTE* work = (BYTE*)malloc(sizeof(BYTE) * FF_MAX_SS);
  if (!work) {
    SD.end();
    SD.begin(SD_CS_PIN, SPI, 4000000);
    server.send(500, "application/json", "{\"ok\":false,\"err\":\"no memory\"}");
    return;
  }
  FRESULT res = f_mkfs("0:", FM_ANY, 0, work, sizeof(BYTE) * FF_MAX_SS);
  free(work);
  if (res != FR_OK) {
    SD.end();
    SD.begin(SD_CS_PIN, SPI, 4000000);
    server.send(500, "application/json", "{\"ok\":false,\"err\":\"mkfs failed\"}");
    return;
  }
  SD.end();
  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
    server.send(500, "application/json", "{\"ok\":false,\"err\":\"remount failed\"}");
    return;
  }
  saveConfig();  // 从内存恢复 CONFIG.TXT (设备名/地点/时间表等)
  logLine("[SD] TF 卡已格式化并重新挂载，配置已恢复");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLog() {
  touch();
  String j = "[";
  for (int i = 0; i < g_logCount; i++) {
    if (i) j += ",";
    j += "\"" + jsonEscape(g_logLines[i]) + "\"";
  }
  j += "]";
  server.send(200, "application/json", j);
}

void handleNotFound() {
  String uri = server.uri();
  if (uri.startsWith("/api/")) { server.send(404, "application/json", "{\"ok\":false,\"err\":\"not found\"}"); return; }
  /* v0.2.8 Captive Portal: 各系统连上新 WiFi 时会请求探测地址
   * (iOS: captive.apple.com/hotspot-detect.html, Android: generate_204,
   *  Windows: msftconnecttest.com/connecttest.txt), 期望拿到"已联网"的
   * 固定回复; 这里一律 302 跳到配置页, 系统即判定"此网络需要登录",
   * 自动弹出配置页面。此分支不调 touch(): 探测不算用户操作, 避免反复
   * 探测让 5 分钟无操作自动退出永远不触发。 */
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void startConfigMode() {
  String ssid = g_cfg.deviceName + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  ssid.toUpperCase();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid.c_str(), AP_PASSWORD);
  dnsServer.start(53, "*", WiFi.softAPIP());  // v0.2.8: 所有域名解析到本机, 配合探测重定向弹页
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/time", HTTP_POST, handleTimePost);
  server.on("/api/firmware", HTTP_POST, handleFirmwarePost, handleFirmwareUpload);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/format", HTTP_POST, handleFormat);
  server.on("/api/format_card", HTTP_POST, handleFormatCard);  // v0.2.15: TF 卡真格式化
  server.on("/api/log", HTTP_GET, handleLog);
  server.onNotFound(handleNotFound);
  server.begin();
  g_lastClientActivity = millis();
  g_configStartMs = millis();  // v0.2.12: 无人接入超时从进配置起计时
  g_configClientEverSeen = false;
  logLine("[WEB] 配置模式: 热点 " + ssid + " (密码 " + String(AP_PASSWORD) + "), 打开 http://192.168.4.1");
  logLine("[WEB] Captive Portal 已开启: 手机连上热点将自动弹出配置页");
}

/* ---------------- 工作模式: 一次录音循环 ---------------- */
void recordCycle() {
  if (g_cfg.schedCount > 0 && timeIsValid()) {
    uint32_t now = nowUnix();
    int nowMin = (now % 86400) / 60;
    if (!inSchedule(nowMin)) {
      uint32_t wait = secondsToNextWindow(nowMin);
      logLine("[SCHED] 不在录制时段, 休眠 " + String(wait) + " 秒至下一时段");
      goSleep(wait);
    }
  }
  if (!initMicrophone()) {
    logLine("[I2S] 麦克风初始化失败!");
    blinkError();
    goSleep(60);
  }
  char path[32];
  snprintf(path, sizeof(path), "/ABIRD_%04u.WAV", g_fileIndex % 10000);
  uint32_t recStart = nowUnix();
  if (recordToWav(path)) { appendRecLog(path, recStart); g_fileIndex++; }
  goSleep(g_cfg.sleepSec);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  setenv("TZ", "UTC0", 1);
  tzset();
  Serial.println("\n==== Audiobird v" FIRMWARE_VERSION " ====");
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);
  ledSet(false);  // v0.2.5: 此板 LED 低电平点亮, pinMode 后默认低=常亮, 先关掉免得扫描期一直亮

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
    logLine("[SD] 初始化失败! 检查接线与卡格式 (需 FAT32)");
    blinkError();
    goSleep(60);
  }
  logLine("[SD] 初始化成功");

  loadConfig();
  resolveFileIndex();

  /* 时钟: 优先 DS3231, 否则用软件时钟 (唤醒时累进上次休眠时长) */
  g_rtcPresent = rtcDetect();
  if (g_rtcPresent) logLine("[RTC] DS3231 已检测到");
  else {
    if (g_softTimeValid) g_softEpoch += g_lastSleepSec;
    logLine("[RTC] 无 DS3231, 使用软件时钟 (精度约数分钟/天, 建议 App 对时)");
  }

  /* v0.2.12 决定模式: 上电默认进配置。
   * esp_reset_reason() 区分复位来源: POWERON (含 RST 键复位) 与
   * FORCE_CONFIG=1 进配置; 深睡唤醒 (DEEPSLEEP) 与软件重启 (SW, 如
   * 超时/保存后重启) 走工作模式 — 否则每次深睡唤醒都会回到配置,
   * 录音循环永远跑不起来, 超时重启也会死循环回配置。
   * 5 秒 GPIO0 窗口删除: 上电已默认进配置, 窗口只会拖慢每次唤醒。
   * (GPIO0 上电瞬间短接仍是 ROM 下载模式, 与本逻辑无关。) */
  bool forceCfg = g_cfg.forceConfig;
  if (forceCfg) { g_cfg.forceConfig = false; saveConfig(); }
  esp_reset_reason_t reason = esp_reset_reason();
  g_configMode = (reason == ESP_RST_POWERON) || forceCfg;
  logLine("[BOOT] 复位原因: " + String((int)reason) + (g_configMode ? " -> 配置" : " -> 工作"));
  logLine("[BOOT] 第 " + String(g_bootCount++) + " 次唤醒, 模式: " + String(g_configMode ? "配置" : "工作"));

  if (g_configMode) {
    ledSet(true);
    startConfigMode();
  } else {
    recordCycle();  // 内部以 deep sleep 结束, 不返回
  }
}

void loop() {
  if (g_configMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    /* v0.2.12: 无人接入超时: 进配置起 2 分钟内始终无 Wi-Fi 客户端接入
     * (softAPgetStationNum() 恒为 0), 自动重启回工作模式。重启是 SW 复位
     * (≠POWERON), 故 setup() 会走工作模式, 不会死循环回配置。
     * 一旦有任何客户端接入过, 本超时即取消, 后续沿用 5 分钟无操作定时器。 */
    if (WiFi.softAPgetStationNum() > 0) g_configClientEverSeen = true;
    if (!g_configClientEverSeen && millis() - g_configStartMs > CONFIG_NO_CLIENT_TIMEOUT_MS) {
      logLine("[WEB] 配置模式 2 分钟无 Wi-Fi 接入, 自动回工作模式");
      ESP.restart();
    }
    if (millis() - g_lastClientActivity > CONFIG_MODE_TIMEOUT_MS) {
      logLine("[WEB] 配置模式 5 分钟无操作, 自动回工作模式");
      ESP.restart();
    }
  }
}
