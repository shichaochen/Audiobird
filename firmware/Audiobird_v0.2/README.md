# Audiobird 固件 v0.2 (网页 App 版)

在 v0.1 (定时录音 → WAV → SD) 基础上增加网页配置 App。
设计文档见 `docs/app-design.md`。

## 新增功能

- **配置模式**: 上电时长按 BOOT 键 (GPIO0) 800ms, 或 `CONFIG.TXT` 中
  `FORCE_CONFIG=1`, 进入配置模式: ESP32-S3 开 AP 热点
  (`Audiobird-XXXX`, 密码 `audiobird`), 手机浏览器打开 `http://192.168.4.1`
- **网页 App**: 状态 / 配置 / 对时 / 文件下载删除 / OTA 固件升级 /
  清空 SD 卡 / 日志查看 (单文件 HTML 存于 `web_ui.h`, PROGMEM)
- **录制时间表**: `SCHEDULE=05:00-08:00,17:00-19:00`, 时段外休眠 (需对时)
- **时钟**: DS3231 RTC (可选, I2C GPIO 8/9); 无 RTC 时用软件时钟估算
  (精度约数分钟/天, 建议每次布设前用 App 对时)
- **电量检测**: 电池经 2:1 分压接 GPIO1 (ADC), App 状态页显示电压与百分比

## 环境搭建

同 v0.1: Arduino IDE + ESP32 Arduino Core **2.0.x**, 开发板
"ESP32S3 Dev Module", USB CDC On Boot 建议 Enabled。
无额外库依赖 (WebServer / Update / Wire / SD 均为内置库)。

## 接线 (v0.1 基础上增加)

| 新增 | ESP32-S3 | 说明 |
|---|---|---|
| DS3231 (可选) | SDA=GPIO8, SCL=GPIO9, VCC=3V3, GND | 高精度时钟 |
| 电池分压 | GPIO1 (ADC) | 100k+100k 2:1 分压, 最高测 4.2V 锂电 |
| 配置键 | GPIO0 (BOOT 键) | 板载按键, 无需外接 |

其余接线 (I2S 麦克风、SD 卡) 同 v0.1, 见 `firmware/README.md`。

## 使用流程

1. 烧录 `Audiobird_v0.2.ino` (与 `web_ui.h` 同一文件夹)
2. 首次使用: 上电时长按 BOOT 键进配置模式 → 手机连热点 →
   浏览器打开 `http://192.168.4.1` → 配置参数 → 对时 → 保存
   (保存后设备自动重启进入工作模式)
3. 工作模式: 按配置循环录音, WiFi 关闭省电
4. 需要改配置: 上电时长按 BOOT 键, 或在 App 里改完保存

## CONFIG.TXT (v0.2 扩展, 向后兼容 v0.1)

```
SAMPLE_RATE=32000
RECORD_SECONDS=60
SLEEP_SECONDS=300
GAIN=4
SCHEDULE=05:00-08:00,17:00-19:00
DEVICE_NAME=Audiobird-01
FORCE_CONFIG=0
```

## 与 v0.1 的区别说明

- **清空 SD 卡**: v0.2 的 `/api/format` 实现为删除全部文件
  (Arduino SD 库无格式化接口, 真正格式化请在电脑上操作)
- **时间表**: 无 RTC 且未对时时, 时间表被忽略 (无法知道当前时间),
  设备按原有间隔循环录音
- **电量**: 百分比为粗略映射 (18650: 4.2V=100%, 3.7V≈50%, 3.3V≈5%),
  仅作参考, 勿作精确电量计
- **配置模式超时**: 5 分钟无操作自动重启回工作模式

## 故障排查

| 现象 | 可能原因 |
|---|---|
| 热点搜不到 | 未进配置模式: 上电时 BOOT 键需按住 800ms 以上再松开 |
| 打开 192.168.4.1 无页面 | 手机可能仍走移动数据: 连热点后关闭移动数据, 或直接访问 |
| 配置保存后未生效 | 检查 CONFIG.TXT 是否写入; 配置页保存后设备会重启 |
| 对时后时间仍不准 | 未接 DS3231 时软件时钟会漂移, 每次布设前重新对时 |
| OTA 失败 | 上传文件必须是对应板型的 .bin; 失败后设备保留旧固件, 可重新尝试 |
