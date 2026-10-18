# Audiobird v0.2-esp32（经典款 ESP32 移植版）

由 `firmware/Audiobird_v0.2`（ESP32-S3 版）移植，功能完全一致：
配置模式 AP 网页 App（状态/配置/对时/文件/OTA/清空/日志）、录制时间表、
DS3231 RTC（可选）、电池检测、断电续写。版本号 `0.2.0-esp32`。

## 开发板设置

- Arduino IDE / arduino-cli 开发板：**ESP32 Dev Module**
- **ESP32 Arduino Core 必须用 2.0.x（推荐 2.0.17）**：3.x 换了底层 IDF 5.x，ADC/I2S 驱动不兼容，
  烧进去会在启动时报 `ADC: CONFLICT! driver_ng ...` 然后不断重启。
  降级方法：Arduino IDE → 工具 → 开发板 → 开发板管理器 → 搜 esp32 → 版本选 2.0.17 → 安装
- Flash：4MB（默认分区即可，固件约 850KB）
- 上传失败时把上传速度降到 **115200**

## 接线表（ESP32-WROOM-32 DEVKIT V1 30 脚）

| 模块 | 模块引脚 | ESP32 引脚 | 说明 |
|---|---|---|---|
| INMP441 | VDD | 3V3 |  |
| INMP441 | GND | GND |  |
| INMP441 | SD | GPIO26 | I2S 数据输入 |
| INMP441 | WS | GPIO27 | I2S 声道选择 |
| INMP441 | SCK | GPIO14 | I2S 位时钟 |
| INMP441 | L/R | GND | 接 GND 选左声道 |
| MicroSD（SPI） | SCK | GPIO18 | VSPI |
| MicroSD（SPI） | MISO | GPIO19 |  |
| MicroSD（SPI） | MOSI | GPIO23 |  |
| MicroSD（SPI） | CS | GPIO5 |  |
| MicroSD（SPI） | VCC/GND | 5V/GND | SD 卡用 5V 供电 |
| DS3231（可选） | SDA/SCL | GPIO21/22 |  |
| 电池检测（可选） | 分压中点 | GPIO34 | 2:1 分压，ADC1 |
| 状态 LED | — | GPIO2 | 板载蓝色 LED |

注意：GPIO34 只能做输入；SD 卡座若自带 3.3V 稳压则 VCC 接 5V。

## 与 S3 版的差异

- 引脚全部重映射（S3 版用 GPIO4/5/6、10/11/12/13、48 等，经典款避开 strapping/Flash 引脚）
- 板载 LED：GPIO48 → GPIO2
- 电池 ADC：GPIO1 → GPIO34（ADC1，与 WiFi 共存）
- 其余逻辑（录音/WAV/网页 App/OTA/deep sleep）零改动
