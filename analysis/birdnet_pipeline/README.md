# 鸟鸣自动分析管道（BirdNET 本地版）

把一堆录音文件丢进去，自动识别鸟种、统计每种鸟的鸣叫规律，输出整齐的表格和图。
识别用 Cornell Lab 的 BirdNET 模型（v2.4），全程本地跑，不上传录音。

## 它是怎么工作的

- BirdNET 把每段录音切成 3 秒一段，逐段打分：这段里最可能是哪种鸟、置信度多少。
- 只有置信度 ≥ 阈值（默认 0.25）的结果才会进表。
- 如果给了经纬度 + 日期，BirdNET 会再用分布模型过滤掉那个地点那个季节不可能出现的鸟
  （实测：没给地点时，16kHz 测试文件里混进了一个北美物种 Fox Sparrow；给了上海经纬度后，
  它和一只 0.257 的 Indian Roller 一起被过滤掉了）。
- 每种鸟的规律从三张表里看：`species_summary.csv`（总数、平均置信度、首次/末次、高峰小时）、
  `hourly_activity.csv`（物种 × 24 小时的叫声分布）、`daily_summary.csv`（按天统计）。

## 环境（已装好，在这台机器上）

- 推理环境：`~/workspace/venvs/birdnet311/bin/python`
  （Python 3.11 独立版在 `~/workspace/tools/python3.11`，因为 tflite-runtime 官方轮子
  最高只到 Python 3.11，没有 3.12 版；本机系统 Python 是 3.12，走不通。）
- 关键版本钉：`numpy<2`（tflite-runtime 2.14 与 numpy 2.x 冲突，会报 `_ARRAY_API not found`），
  另需 `resampy`（librosa 把 44.1kHz 重采样到 48kHz 要用它，缺了会整文件报错）。
- 装包清单：`birdnetlib librosa audioread tflite-runtime==2.14.0 matplotlib resampy`，
  再加 `pip install "numpy<2"`。
- 速度：这台 2 核机器上约 3–4 倍实时（27 秒录音约 7 秒出结果），
  Audiobird 一段 300 秒录音约 1–2 分钟；整张卡批量跑建议放后台或过夜。

## 用法

```bash
# 基本：分析一整个文件夹（递归），输出到结果文件夹
~/workspace/venvs/birdnet311/bin/python analyze.py \
    --input <录音文件夹> --output <结果文件夹> --min-conf 0.25

# Audiobird：带上 ABIRD_LOG.CSV，用设备真实录音时间 + 每段录音的经纬度
~/workspace/venvs/birdnet311/bin/python analyze.py \
    --input <ABIRD_*.WAV 所在文件夹> --output <结果文件夹> \
    --log <ABIRD_LOG.CSV 路径> --min-conf 0.25

# 手动指定地点和日期（没有 log 时用）
~/workspace/venvs/birdnet311/bin/python analyze.py \
    --input <录音文件夹> --output <结果文件夹> \
    --lat 31.23 --lon 121.47 --date 2026-10-27
```

输入格式：wav / ogg / mp3 / flac / m4a（ogg、mp3 靠 ffmpeg 解码，本机已装）。
不带 `--log` 时，录音开始时间取文件修改时间，`detections.csv` 的 `time_basis` 列会写 `mtime`；
带 `--log` 时取日志里的 `datetime_local`（优先）或 `datetime_utc`，`time_basis` 写 `log_local`/`log_utc`。

## 输出

| 文件 | 内容 |
|---|---|
| detections.csv | 每一行一次检出：文件、录音开始时间、段起止秒、检出时刻、小时、日期、中文名、学名、置信度、用过的经纬度 |
| species_summary.csv | 每种鸟：段数、秒数、平均/最高置信度、首次/末次检出、高峰小时、占比 |
| hourly_activity.csv | 物种 × 24 小时矩阵：每种鸟一天里几点叫得多 |
| daily_summary.csv | 日期 × 物种段数（长表） |
| files.csv | 每个文件的状态、时长、检出数、物种 |
| species_counts.png | 每种鸟检出段数条形图 |
| hourly_heatmap.png | 热力图：每种鸟 × 24 小时 |
| timeline.png | 每文件一行，色块标出各段检出的鸟 |

## BirdNET 和 Xeno-canto 各自的角色

- **BirdNET（Cornell Lab）是识别算法**：就是这个管道里跑的模型。它的训练数据主要来自
  Xeno-canto 和 Macaulay Library 的标注录音，覆盖约 6000 多种鸟。
- **Xeno-canto 是开放数据库，不是识别器**：用来人工复核——对低置信度或存疑的检出，
  去 Xeno-canto 听该物种的标准录音比对；也可以从上面下载更多样本做验证。
- 注意：Xeno-canto 现在的 API v3 需要免费 API key（去 xeno-canto.org/account 注册后领取），
  旧版 v2 已停用。本管道不直接调用它，需要时手动查，或以后拿到 key 再接一个参考录音抓取脚本。

## 阈值怎么选

- 0.25（默认）：兼顾检出率和干净程度，适合日常统计。
- 0.1：做物种清单普查时用，能捡到弱信号，但假阳性多，必须人工听一遍。
- 0.5：只要高把握的结果，适合直接写进报告的数据。
- 经验：置信度 0.5 以下的检出建议都抽听复核；0.25–0.5 之间的尤其要听。

## 已验证的演示结果（demo_output / demo_output_log）

用你 Drive 里「鸟叫声」文件夹的 3 个录音（鹊鸲 wav + ogg、棕背伯劳 wav），
外加一个把鹊鸲 wav 转成 16kHz 单声道（模拟 Audiobird 格式）的文件，共 4 个文件跑通：

| 文件 | 结果 |
|---|---|
| 鹊鸲 call_and_song.wav（48kHz） | 7 段全是鹊鸲，置信度 0.73–0.98 |
| 棕背伯劳 call.wav（44.1kHz） | 5 段全是棕背伯劳，0.92–0.999 |
| 鹊鸲 song.ogg | 10 段：鹊鸲 9 段 + 1 段低置信度红耳鹎 0.28（背景鸟或边缘叫声） |
| 鹊鸲 16kHz 模拟版 | 7 段鹊鸲，0.53–0.90（比 48kHz 版低，但结论不变） |

合计 29 次检出、3 个物种。`demo_output_log` 是带模拟日志跑的版本，
证明了 `--log` 的时间拼接（检出时刻 = 日志开始时间 + 段偏移）无误。

## 16kHz（Audiobird 格式）的注意事项

- 能用，结论不变，但置信度系统性偏低（高频信息缺了 8kHz 以上部分），
  且更容易混进分布区外的物种。
- 对策：务必带 `--log`（自动用上每段录音的真实经纬度做分布过滤），
  16kHz 文件的阈值可以提到 0.3–0.4，低置信度结果人工听一遍。

## 下一步可以加的（还没做）

- 定时任务：SD 卡新文件拷进来就自动跑一批。
- 每种鸟的声谱图缩略图，附在报告里。
- 拿到 Xeno-canto key 后，接参考录音自动抓取，低置信度检出自动附上比对链接。

## 仓库版本说明

本目录为 BirdNET 鸟鸣分析管道的仓库版，与本地完整包的区别：

- **未包含 `demo_data/` 演示录音**（原始录音文件，约 11MB）：演示输出表格
  在 `demo_output/` 与 `demo_output_log/`，三张 PNG 图表可由脚本重新生成。
- 完整演示包（含录音与图表）见 `BirdNET_birdcall_pipeline.zip`。
