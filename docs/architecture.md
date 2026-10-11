# Audiobird 系统架构

## 设计目标

做一台青少年可复现的低成本野外鸟声记录仪: 能定时录音、
存 WAV 到 SD 卡、可配置、可长期野外部署。致敬并对标
[AudioMoth](https://www.openacousticdevices.info/) (Open Acoustic Devices)。

## 系统框图

```
                    +-------------------+
                    |    ESP32-S3 主控   |
                    |  (Arduino 固件)   |
                    +--+---+---+---+----+
                       |   |   |   |
        I2S 数字音频    |   |   |   |   SPI
      +-----------+    |   |   |   |  +-----------+
      | INMP441   |----+   |   |   +--| MicroSD   |
      | 麦克风模块 |        |   |      |  (FAT32)  |
      +-----------+        |   |      +-----------+
                           |   |
                     USB CDC   +-- 预留: DS3231 RTC / WiFi
                    (日志/配置)
```

## 固件模块划分 (firmware/Audiobird/)

| 模块 | 文件内位置 | 职责 |
|---|---|---|
| config | `loadConfig()` | 解析 SD 卡 `CONFIG.TXT`, 参数钳制 |
| audio | `initMicrophone()` | I2S 初始化 (32-bit 槽接收 24-bit 数据) |
| storage | `recordToWav()` / `writeWavHeader()` | 16-bit WAV 写入, 头部占位后回填 |
| power | `goSleep()` | deep sleep + RTC 定时唤醒 |
| ui | LED 闪烁 / `blinkError()` | 录音指示与故障报警 |
| log | Serial 输出 | 调试日志 |

## 与 AudioMoth 功能对照

| 功能 | AudioMoth (EFM32) | Audiobird v0.1 (ESP32-S3) |
|---|---|---|
| 定时录音 | ✅ | ✅ (deep sleep 定时唤醒) |
| 采样率可调 | 8–384kHz | 8–48kHz (鸟类够用, 超声波不行) |
| 增益可调 | 模拟增益 | 数字增益 |
| WAV 存 SD 卡 | ✅ | ✅ |
| 幅度触发录音 | ✅ (固件 1.4+) | ⏳ v0.2 |
| USB 配置 | 桌面 App | ⏳ v0.2 (网页配置 via AP 模式) |
| 超低功耗 | 极致 (nA 级休眠) | 够用 (uA 级休眠) |
| WiFi 上传 | ❌ | ⏳ v0.3 (ESP32-S3 原生支持) |

## 路线图

- **v0.1** (当前): I2S 录音 → WAV → SD, 定时休眠, CONFIG.TXT 配置
- **v0.2**: 幅度触发录音; AP 模式网页配置 (替代 CONFIG.TXT); 可选 DS3231 高精度 RTC
- **v0.3**: WiFi 定时上传录音到服务器; BirdNET 离线识别 (树莓派端, 见课题申报框架)
- **v1.0**: 一体化 PCB (ESP32-S3 + 数字麦 + SD + 电源管理), 小批量验证
