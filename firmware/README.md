# Audiobird 固件说明

ESP32-S3 野外鸟声记录仪固件 (Arduino 框架), v0.1。

## 功能

- I2S 数字麦克风采集 (INMP441), 8–48kHz 可配置
- 16-bit PCM 单声道 WAV 写入 SD 卡, 文件名 `ABIRD_XXXX.WAV`
- 断电续写: 启动时扫描卡上已有录音, 从最大序号 +1 继续 (断电/换电池后不重名)
- SD 卡 `CONFIG.TXT` 配置采样率 / 录制时长 / 休眠时长 / 数字增益
- 录制完成后 deep sleep 休眠, 定时唤醒循环工作
- 录音中 LED 闪烁, 出错时 LED 快闪报警, 串口输出运行日志

## 环境搭建

1. 安装 Arduino IDE (1.8.x 或 2.x 均可)
2. 安装 ESP32 Arduino Core **2.0.x** (3.x 的 I2S API 有变化, 本固件暂未适配)
   - Arduino IDE: 文件 → 首选项 → 附加开发板管理器网址, 添加
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - 工具 → 开发板 → 开发板管理器, 搜索 ESP32 安装 2.0.x 版本
3. 工具 → 开发板 → 选择 **ESP32S3 Dev Module**
   - USB CDC On Boot: Enabled (方便看串口日志)
   - Flash Size: 按你的模组选择 (常见 8MB / 16MB)

## 接线

| ESP32-S3 | INMP441 | MicroSD 模块(SPI) | 说明 |
|---|---|---|---|
| GPIO4 | SCK | — | I2S BCK |
| GPIO5 | WS | — | I2S WS |
| GPIO6 | SD | — | I2S 数据输入 |
| — | L/R | — | 接 GND (选左声道) |
| GPIO12 | — | SCK | SD SPI 时钟 |
| GPIO13 | — | MISO | |
| GPIO11 | — | MOSI | |
| GPIO10 | — | CS | |
| 3V3 | VDD | VCC | 共 3.3V |
| GND | GND | GND | 共地 |

注意: GPIO 4/5/6/10/11/12/13 均避开了 ESP32-S3 的 strapping 引脚,
上电不会影响启动模式。

## 烧录与运行

1. 用 USB 线连接 ESP32-S3 开发板
2. Arduino IDE 打开 `firmware/Audiobird/Audiobird.ino`, 点击上传
3. 打开串口监视器 (115200), 应看到初始化日志与录制进度
4. 首次运行建议先不插 SD 卡观察报错, 确认后再插入 FAT32 格式的卡

## CONFIG.TXT

在 SD 卡根目录创建 `CONFIG.TXT` (纯文本), 示例:

```
# Audiobird 配置文件
SAMPLE_RATE=32000
RECORD_SECONDS=60
SLEEP_SECONDS=300
GAIN=4
```

| 参数 | 说明 | 默认值 |
|---|---|---|
| SAMPLE_RATE | 采样率 Hz, 8000–48000 | 32000 |
| RECORD_SECONDS | 每次录制时长 (秒) | 60 |
| SLEEP_SECONDS | 两次录制间隔休眠 (秒) | 300 |
| GAIN | 数字增益倍数, 1–32 | 4 |

## 功耗参考

- 录制中约 100–150mA, deep sleep 约几十 uA
- 按"每 5 分钟录 1 分钟"估算, 一节 18650 (3000mAh) 可工作约 2–3 周
- 实际续航以实测为准, 野外部署前建议先做 48 小时连续测试

## 故障排查

| 现象 | 可能原因 |
|---|---|
| `[SD] 初始化失败` | 接线错误 / 卡未格式化为 FAT32 / 卡座接触不良 |
| 录出的 WAV 无声 | INMP441 的 L/R 未接 GND / I2S 引脚接错 / 增益太小 |
| 录出的 WAV 全是爆音 | 增益太大, 调小 GAIN |
| 唤醒后不录制直接休眠 | 上一次录制异常, 看串口日志定位 |
