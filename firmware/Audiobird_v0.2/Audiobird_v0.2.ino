/*
 * Audiobird v0.2 — ESP32-S3 野外鸟声记录仪固件 (带网页 App)
 * ============================================================================
 * 新增 (相对 v0.1):
 *   - 配置模式: ESP32-S3 开 AP 热点, 手机浏览器打开 http://192.168.4.1
 *     即可配置 / 对时 / 下载录音 / OTA 升级 (详见 docs/app-design.md)
 *   - 工作模式: WiFi 关闭, 按配置循环录音, deep sleep 省电
 *   - 录制时间表 SCHEDULE (需对时, 否则忽略时间表)
 *   - DS3231 RTC (可选, 有则用硬件时钟, 无则用软件时钟估算)
 *   - 电池电压检测 (分压 + ADC)
 *   - 固件 OTA 升级, 文件下载/删除, SD 卡清空
 *
 * 两种模式:
 *   - 配置模式: 上电时长按 BOOT 键 (GPIO0) 或 CONFIG.TXT 中 FORCE_CONFIG=1
 *   - 工作模式: 默认; 配置模式 5 分钟无操作自动回到工作模式
 *
 * 开发环境: Arduino IDE + ESP32 Arduino Core 2.0.x
 *           开发板 "ESP32S3 Dev Module", USB CDC On Boot 建议 Enabled
 * 硬件: ESP32-S3-WROOM-1 + INMP441 + MicroSD (SPI) + 可选 DS3231
 *
 * 开源许可: MIT License
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <time.h>
#include "esp_sleep.h"
#include "web_ui.h"

/* ---------------- 引脚定义 (ESP32-S3, 避开 strapping 引脚) ---------------- */
#define I2S_BCK_PIN   4    // INMP441 SCK
#define I2S_WS_PIN    5    // INMP441 WS
#define I2S_DIN_PIN   6    // INMP441 SD
#define SD_SCK_PIN    12
#define SD_MISO_PIN   13
#define SD_MOSI_PIN   11
#define SD_CS_PIN     10
#define LED_PIN       48   // 多数 S3 开发板载 LED; 若无改为 -1
#define RTC_SDA_PIN   8    // DS3231 SDA (可选)
#define RTC_SCL_PIN   9    // DS3231 SCL (可选)
#define BAT_ADC_PIN   1    // 电池分压检测 (2:1 分压); 无此电路改为 -1
#define CONFIG_BTN_PIN 0   // BOOT 键, 上电时长按进配置模式

/* ---------------- 默认配置 ---------------- */
#define DEFAULT_SAMPLE_RATE  32000
#define DEFAULT_RECORD_SEC   60
#define DEFAULT_SLEEP_SEC    300
#define DEFAULT_GAIN         4
#define DEFAULT_DEVICE_NAME  "Audiobird-01"
#define CONFIG_MODE_TIMEOUT_MS (5UL * 60UL * 1000UL)  // 配置模式 5 分钟无操作自动退出
#define AP_PASSWORD "audiobird"
#define FIRMWARE_VERSION "0.2.0"

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
unsigned long g_lastClientActivity = 0;

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
  }
  f.close();
  if (g_cfg.sampleRate < 8000)  g_cfg.sampleRate = 8000;
  if (g_cfg.sampleRate > 48000) g_cfg.sampleRate = 48000;
  if (g_cfg.recordSec == 0) g_cfg.recordSec = DEFAULT_RECORD_SEC;
  if (g_cfg.gain == 0)      g_cfg.gain = 1;
  if (g_cfg.gain > 32)      g_cfg.gain = 32;
  if (g_cfg.deviceName.length() == 0) g_cfg.deviceName = DEFAULT_DEVICE_NAME;
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
  f.close();
  logLine("[CFG] 已保存 CONFIG.TXT");
}

/* ---------------- 断电续写: 扫描已有文件取最大序号 +1 ---------------- */
void resolveFileIndex() {
  File dir = SD.open("/");
  if (!dir) return;
  uint32_t maxIdx = 0;
  bool found = false;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    String name = f.name();
    int slash = name.lastIndexOf('/');  // ESP32 上 name() 可能带 "/" 前缀, 先取 basename
    if (slash >= 0) name = name.substring(slash + 1);
    f.close();
    if (name.startsWith("ABIRD_") && name.endsWith(".WAV")) {
      uint32_t idx = name.substring(6, 10).toInt();
      if (!found || idx >= maxIdx) { maxIdx = idx; found = true; }
    }
  }
  dir.close();
  if (found) g_fileIndex = maxIdx + 1;
  logLine("[SD] 断电续写: 下一文件序号 " + String(g_fileIndex));
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
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
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
  File f = SD.open(path, FILE_WRITE);
  if (!f) { logLine("[REC] 无法创建文件"); return false; }
  writeWavHeader(f, g_cfg.sampleRate, 0);
  const size_t N = 1024;
  int32_t raw[N];
  int16_t pcm[N];
  size_t bytesRead = 0;
  uint32_t totalSamples = g_cfg.sampleRate * g_cfg.recordSec;
  uint32_t written = 0;
  uint32_t lastBlink = 0;
  bool ledState = false;
  logLine("[REC] 开始录制 " + String(path) + ", " + String(g_cfg.sampleRate) + "Hz, " + String(g_cfg.recordSec) + "秒");
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
      digitalWrite(LED_PIN, ledState);
    }
  }
  f.seek(0);
  writeWavHeader(f, g_cfg.sampleRate, written * sizeof(int16_t));
  f.close();
  i2s_driver_uninstall(I2S_NUM_0);
  if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);
  logLine("[REC] 完成, 共 " + String(written) + " 个采样点");
  return written > 0;
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
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
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

int countWavFiles() {
  int n = 0;
  File dir = SD.open("/");
  if (!dir) return 0;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    String name = f.name();
    if (!f.isDirectory() && name.endsWith(".WAV")) n++;
    f.close();
  }
  dir.close();
  return n;
}

void handleRoot() {
  touch();
  server.send_P(200, "text/html", INDEX_HTML);
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
  j += "\"file_count\":" + String(countWavFiles()) + ",";
  j += "\"sample_rate\":" + String(g_cfg.sampleRate) + ",";
  j += "\"gain\":" + String(g_cfg.gain) + ",";
  j += "\"record_seconds\":" + String(g_cfg.recordSec) + ",";
  j += "\"sleep_seconds\":" + String(g_cfg.sleepSec) + ",";
  j += "\"sched_count\":" + String(g_cfg.schedCount);
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
  j += "\"schedule\":\"" + sch + "\"";
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
  if (g_cfg.sampleRate < 8000)  g_cfg.sampleRate = 8000;
  if (g_cfg.sampleRate > 48000) g_cfg.sampleRate = 48000;
  if (g_cfg.gain == 0) g_cfg.gain = 1;
  if (g_cfg.gain > 32) g_cfg.gain = 32;
  if (g_cfg.deviceName.length() == 0) g_cfg.deviceName = DEFAULT_DEVICE_NAME;
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTimePost() {
  touch();
  if (!server.hasArg("unix")) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing unix\"}"); return; }
  setTimeUnix(server.arg("unix").toInt());
  server.send(200, "application/json", "{\"ok\":true,\"time\":" + String(nowUnix()) + "}");
}

void handleFiles() {
  touch();
  String j = "[";
  bool first = true;
  File dir = SD.open("/");
  if (dir) {
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      String name = f.name();
      if (!f.isDirectory() && name.endsWith(".WAV")) {
        if (!first) j += ",";
        first = false;
        uint32_t dur = (g_cfg.sampleRate > 0) ? (uint32_t)(f.size() / (g_cfg.sampleRate * 2)) : 0;
        j += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((uint32_t)f.size())
           + ",\"duration_est\":" + String(dur) + "}";
      }
      f.close();
    }
    dir.close();
  }
  j += "]";
  server.send(200, "application/json", j);
}

bool safeFileName(const String &name) {
  return name.length() > 0 && name.indexOf('/') < 0 && name.indexOf("..") < 0;
}

void handleFileDownload(const String &name) {
  touch();
  if (!safeFileName(name)) { server.send(400, "text/plain", "bad filename"); return; }
  File f = SD.open("/" + name, FILE_READ);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.streamFile(f, "audio/wav");
  f.close();
}

void handleFileDelete(const String &name) {
  touch();
  if (!safeFileName(name)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  bool ok = SD.remove("/" + name);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
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
  touch();
  String uri = server.uri();
  if (uri.startsWith("/api/files/")) {
    String name = uri.substring(strlen("/api/files/"));
    if (server.method() == HTTP_GET) handleFileDownload(name);
    else if (server.method() == HTTP_DELETE) handleFileDelete(name);
    else server.send(405, "text/plain", "method not allowed");
    return;
  }
  server.send(404, "text/plain", "not found");
}

void startConfigMode() {
  String ssid = g_cfg.deviceName + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  ssid.toUpperCase();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid.c_str(), AP_PASSWORD);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/time", HTTP_POST, handleTimePost);
  server.on("/api/files", HTTP_GET, handleFiles);
  server.on("/api/firmware", HTTP_POST, handleFirmwarePost, handleFirmwareUpload);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/format", HTTP_POST, handleFormat);
  server.on("/api/log", HTTP_GET, handleLog);
  server.onNotFound(handleNotFound);
  server.begin();
  g_lastClientActivity = millis();
  logLine("[WEB] 配置模式: 热点 " + ssid + " (密码 " + String(AP_PASSWORD) + "), 打开 http://192.168.4.1");
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
  if (recordToWav(path)) g_fileIndex++;
  goSleep(g_cfg.sleepSec);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  setenv("TZ", "UTC0", 1);
  tzset();
  Serial.println("\n==== Audiobird v" FIRMWARE_VERSION " ====");
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);

  /* 配置键: 上电时长按 BOOT 键 (GPIO0) 800ms 进入配置模式 */
  pinMode(CONFIG_BTN_PIN, INPUT_PULLUP);
  bool btnConfig = false;
  if (digitalRead(CONFIG_BTN_PIN) == LOW) {
    delay(800);
    if (digitalRead(CONFIG_BTN_PIN) == LOW) btnConfig = true;
  }

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

  /* 决定模式: 按键 或 FORCE_CONFIG */
  bool forceCfg = g_cfg.forceConfig;
  if (forceCfg) { g_cfg.forceConfig = false; saveConfig(); }
  g_configMode = btnConfig || forceCfg;
  logLine("[BOOT] 第 " + String(g_bootCount++) + " 次唤醒, 模式: " + String(g_configMode ? "配置" : "工作"));

  if (g_configMode) {
    if (LED_PIN >= 0) digitalWrite(LED_PIN, HIGH);
    startConfigMode();
  } else {
    recordCycle();  // 内部以 deep sleep 结束, 不返回
  }
}

void loop() {
  if (g_configMode) {
    server.handleClient();
    if (millis() - g_lastClientActivity > CONFIG_MODE_TIMEOUT_MS) {
      logLine("[WEB] 配置模式 5 分钟无操作, 自动回工作模式");
      ESP.restart();
    }
  }
}
