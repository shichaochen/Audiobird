/*
 * Audiobird v0.1 — ESP32-S3 野外鸟声记录仪固件
 * ============================================================================
 * 功能: I2S 数字麦克风采集 -> 16-bit WAV 写入 SD 卡 -> deep sleep 定时休眠
 *
 * 开发环境: Arduino IDE + ESP32 Arduino Core 2.0.x
 *           开发板选择 "ESP32S3 Dev Module", USB CDC On Boot 建议 Enabled
 * 硬件: ESP32-S3-WROOM-1 + INMP441 (I2S 麦克风, L/R 引脚接 GND) + MicroSD (SPI 模式)
 *
 * 工作流程 (每次上电 / 定时唤醒):
 *   1. 初始化 SD 卡, 扫描已有录音从最大序号续写 (断电后不重名), 读取 /CONFIG.TXT (若存在则覆盖默认配置)
 *   2. 初始化 I2S 麦克风, 按配置录制 RECORD_SECONDS 秒
 *   3. 保存为 /ABIRD_XXXX.WAV (16-bit PCM 单声道), 录音中 LED 闪烁
 *   4. 进入 deep sleep SLEEP_SECONDS 秒, 时间到自动唤醒重复
 *
 * 开源许可: MIT License
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include "esp_sleep.h"

/* ---------------- 引脚定义 (ESP32-S3, 避开 strapping 引脚) ---------------- */
#define I2S_BCK_PIN   4    // INMP441 SCK
#define I2S_WS_PIN    5    // INMP441 WS
#define I2S_DIN_PIN   6    // INMP441 SD
#define SD_SCK_PIN    12
#define SD_MISO_PIN   13
#define SD_MOSI_PIN   11
#define SD_CS_PIN     10
#define LED_PIN       48   // 多数 S3 开发板载 LED; 若你的板子没有, 改为 -1 关闭

/* ---------------- 默认配置 ---------------- */
#define DEFAULT_SAMPLE_RATE  32000   // 鸟鸣主要能量 < 12kHz, 32kHz 足够且省空间
#define DEFAULT_RECORD_SEC   60
#define DEFAULT_SLEEP_SEC    300     // 每 5 分钟录制 1 分钟
#define DEFAULT_GAIN         4       // 数字增益倍数

struct Config {
  uint32_t sampleRate = DEFAULT_SAMPLE_RATE;
  uint32_t recordSec  = DEFAULT_RECORD_SEC;
  uint32_t sleepSec   = DEFAULT_SLEEP_SEC;
  uint32_t gain       = DEFAULT_GAIN;
};

/* deep sleep 后保持: 文件序号与唤醒计数 */
RTC_DATA_ATTR uint32_t g_fileIndex = 0;
RTC_DATA_ATTR uint32_t g_bootCount = 0;

Config g_cfg;

/* ---------------- WAV 文件头 (44 字节, 逐字段写入避免结构体对齐问题) ---------------- */
void writeWavHeader(File &f, uint32_t sampleRate, uint32_t dataBytes) {
  uint32_t byteRate  = sampleRate * 2;   // 单声道 16-bit
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

/* ---------------- 读取 SD 卡上的 CONFIG.TXT ----------------
 * 格式示例:
 *   SAMPLE_RATE=32000
 *   RECORD_SECONDS=60
 *   SLEEP_SECONDS=300
 *   GAIN=4
 */
void loadConfig() {
  File f = SD.open("/CONFIG.TXT", FILE_READ);
  if (!f) { Serial.println("[CFG] 无 CONFIG.TXT, 使用默认配置"); return; }
  Serial.println("[CFG] 读取 CONFIG.TXT");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq); key.trim(); key.toUpperCase();
    String val = line.substring(eq + 1); val.trim();
    if (key == "SAMPLE_RATE")         g_cfg.sampleRate = val.toInt();
    else if (key == "RECORD_SECONDS") g_cfg.recordSec  = val.toInt();
    else if (key == "SLEEP_SECONDS")  g_cfg.sleepSec   = val.toInt();
    else if (key == "GAIN")           g_cfg.gain       = val.toInt();
  }
  f.close();
  if (g_cfg.sampleRate < 8000)  g_cfg.sampleRate = 8000;    // INMP441 有效下限
  if (g_cfg.sampleRate > 48000) g_cfg.sampleRate = 48000;   // I2S 稳定上限
  if (g_cfg.recordSec == 0) g_cfg.recordSec = DEFAULT_RECORD_SEC;
  if (g_cfg.gain == 0)      g_cfg.gain = 1;
  if (g_cfg.gain > 32)      g_cfg.gain = 32;
}

/* ---------------- I2S 麦克风初始化 ---------------- */
bool initMicrophone() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = (int)g_cfg.sampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 输出 24-bit, 用 32-bit 槽接收
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // INMP441 的 L/R 引脚接 GND = 左声道
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

/* ---------------- 录制并保存 WAV ---------------- */
bool recordToWav(const char *path) {
  File f = SD.open(path, FILE_WRITE);
  if (!f) { Serial.println("[REC] 无法创建文件"); return false; }
  writeWavHeader(f, g_cfg.sampleRate, 0);  // 先占位, 录完再回填真实长度

  const size_t N = 1024;
  int32_t raw[N];
  int16_t pcm[N];
  size_t bytesRead = 0;
  uint32_t totalSamples = g_cfg.sampleRate * g_cfg.recordSec;
  uint32_t written = 0;
  uint32_t lastBlink = 0;
  bool ledState = false;

  Serial.printf("[REC] 开始录制 %s, %u Hz, %u 秒\n", path, g_cfg.sampleRate, g_cfg.recordSec);
  while (written < totalSamples) {
    size_t toRead = N;
    if (written + toRead > totalSamples) toRead = totalSamples - written;
    if (i2s_read(I2S_NUM_0, raw, toRead * sizeof(int32_t), &bytesRead, portMAX_DELAY) != ESP_OK) break;
    size_t n = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
      // INMP441 数据为 24-bit 左对齐: 取高 16 位, 乘数字增益并限幅
      int32_t s = (raw[i] >> 16) * (int32_t)g_cfg.gain;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      pcm[i] = (int16_t)s;
    }
    f.write((uint8_t *)pcm, n * sizeof(int16_t));
    written += n;
    if (LED_PIN >= 0 && millis() - lastBlink > 500) {  // 录音中 LED 闪烁
      lastBlink = millis(); ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  f.seek(0);  // 回填 WAV 头中的数据长度
  writeWavHeader(f, g_cfg.sampleRate, written * sizeof(int16_t));
  f.close();
  i2s_driver_uninstall(I2S_NUM_0);
  if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);
  Serial.printf("[REC] 完成, 共 %u 个采样点\n", written);
  return written > 0;
}

void goSleep(uint32_t sec) {
  Serial.printf("[PWR] 进入 deep sleep %u 秒...\n", sec);
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

/* ---------------- 断电续写: 从卡上已有最大序号继续 ----------------
 * g_fileIndex 只存在 RTC 内存, 断电 / 换电池 / 复位后会清零。
 * 启动时扫描卡上 ABIRD_XXXX.WAV, 从最大序号 +1 继续, 避免重名破坏旧录音。
 * 须在 SD.begin 成功后调用。
 */
void resolveFileIndex() {
  File root = SD.open("/");
  if (!root) return;
  uint32_t maxIdx = 0;
  bool found = false;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');  // ESP32 上 name() 可能带 "/" 前缀, 先取 basename
      if (slash >= 0) name = name.substring(slash + 1);
      if (name.startsWith("ABIRD_") && (name.endsWith(".WAV") || name.endsWith(".wav"))) {
        uint32_t idx = (uint32_t)name.substring(6, 10).toInt();
        if (!found || idx > maxIdx) { maxIdx = idx; found = true; }
      }
    }
    entry.close();
  }
  root.close();
  if (found) g_fileIndex = (maxIdx + 1) % 10000;
  Serial.printf("[SD] 续写序号: %u\n", g_fileIndex);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==== Audiobird v0.1 ====");
  Serial.printf("本次为第 %u 次唤醒\n", g_bootCount++);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
    Serial.println("[SD] 初始化失败! 检查接线与卡格式 (需 FAT32)");
    blinkError();
    goSleep(60);  // 1 分钟后重试
  }
  Serial.println("[SD] 初始化成功");

  resolveFileIndex();  // 断电后从卡上最大序号继续, 避免重名

  loadConfig();
  Serial.printf("[CFG] 采样率=%uHz 录制=%u秒 休眠=%u秒 增益=%u\n",
                g_cfg.sampleRate, g_cfg.recordSec, g_cfg.sleepSec, g_cfg.gain);

  if (!initMicrophone()) {
    Serial.println("[I2S] 麦克风初始化失败!");
    blinkError();
    goSleep(60);
  }

  char path[32];
  snprintf(path, sizeof(path), "/ABIRD_%04u.WAV", g_fileIndex % 10000);
  if (recordToWav(path)) g_fileIndex++;

  goSleep(g_cfg.sleepSec);
}

void loop() {
  // 不会执行到: 每次都在 setup() 末尾进入 deep sleep, 唤醒后从 setup() 重新开始
}
