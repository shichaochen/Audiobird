# Audiobird

基于 ESP32-S3 的低成本城市鸟类声音采集设备。
Low-cost urban bird sound recorder based on ESP32-S3.

> 青少年创新课题《城市鸟类声音采集与分析设备开发》的开源硬件载体。
> Open hardware for the youth innovation project on urban bird sound collection.

## 特性

- I2S 数字麦克风采集, 8–48kHz 可配置, 16-bit WAV 存 SD 卡
- deep sleep 定时休眠, 一节 18650 可野外工作数周
- SD 卡 `CONFIG.TXT` 纯文本配置, 无需专用 App
- 整机物料约 ¥70–95, 仅为 AudioMoth ($99) 的一半左右
- Arduino 框架固件, 中学生可读懂、可修改

## 快速开始

1. 按 `hardware/README.md` 接线 (ESP32-S3 + INMP441 + MicroSD)
2. 按 `firmware/README.md` 搭建 Arduino 环境并烧录 `firmware/Audiobird/Audiobird.ino`
3. SD 卡格式化为 FAT32, 可选创建 `CONFIG.TXT` 调整参数
4. 上电, 观察串口日志, 检查 SD 卡中的 `ABIRD_XXXX.WAV`

## 目录结构

```
Audiobird/
├── firmware/          固件
│   ├── Audiobird/     Arduino 工程 (Audiobird.ino)
│   └── README.md      环境搭建 / 烧录 / 配置 / 故障排查
├── hardware/          硬件
│   └── README.md      接线表 / BOM / 供电 / 野外部署 / v0.2 PCB 规划
├── docs/
│   └── architecture.md  系统架构 / 模块划分 / 与 AudioMoth 对照 / 路线图
├── LICENSE
└── README.md
```

## 路线图

- v0.1: 定时录音 → WAV → SD (当前)
- v0.2: 幅度触发录音, AP 模式网页配置, DS3231 高精度时钟
- v0.3: WiFi 上传, 配合树莓派 BirdNET 做离线识别
- v1.0: 一体化 PCB 小批量验证

## 致谢

- [AudioMoth](https://www.openacousticdevices.info/) (Open Acoustic Devices) — 本项目的灵感来源与功能对标
- [BirdNET](https://birdnet.cornell.edu/) (Cornell Lab) — 后续识别算法的基准
- [Xeno-canto](https://xeno-canto.org/) — 开放鸟声数据库

## 开源许可

MIT License, 详见 [LICENSE](LICENSE)。
