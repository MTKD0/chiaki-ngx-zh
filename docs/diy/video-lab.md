# 帧生成视频实验台（独立测试模块）

`gui/enhancement/videolab` 是一个独立的命令行程序 `enhancement-video-lab`。它把一段**本地录制的游戏视频**当作串流画面，送入与正式串流完全相同的 `EnhancementPipeline` 和增强进程（`enhancement/nvngx.dll`），不需要 PS5、PSN 登录或 Chiaki 界面。用途是在没有主机的情况下调试和量化帧生成。

它是单独的 CMake 工程，不参与 GUI、增强进程和 CI 构建；这里出错不会影响正式构建。

## 原理：用被跳过的帧做真值

以**目标帧率 × 倍数**录制。程序每 N 帧取 1 帧作为“真实帧”送入 FG，被跳过的帧正好是 FG 应该生成的画面：

| 想模拟的串流 | FG 倍数 | 录制帧率 | 参数 |
| --- | --- | --- | --- |
| 30 fps | 2x | 60 fps | `--multiplier 2`（默认） |
| 60 fps | 2x | 120 fps | `--multiplier 2` |
| 30 fps | 4x | 120 fps | `--multiplier 4` |
| 30 fps | 2x | 120 fps | `--multiplier 2 --decimate 4` |

`--decimate` 默认等于倍数。每张生成帧与对应的真值帧比较，得到 PSNR、SSIM 和“最差 64 像素块 PSNR”（整帧平均值看不出的局部撕裂、HUD 破损靠它定位）。同时给出两条基线：相邻真实帧线性混合（cross-fade）和重复上一帧。FG 相对混合基线的增益（`gain_vs_blend`）才是运动补偿真正带来的收益。

## 三种模式

| 模式 | 作用 | 需要 |
| --- | --- | --- |
| `quality`（默认） | 逐帧送入真实管线，读回每张生成帧与真值比较；可把输出按显示顺序编码成视频 | Windows + RTX + 已部署的增强进程和运行库 |
| `realtime` | 按串流节奏实时播放到窗口，记录每张真实帧、生成帧的呈现时刻，统计帧间隔抖动、丢帧 | 同上 |
| `dry-run` | 用 CPU 线性混合代替 GPU 增强进程，跑完整流程 | 只需 ffmpeg；任何系统 |

`dry-run` 用来确认录像、ffmpeg 和实验台本身没有问题，并得到该片段的基线分数。

## 录制建议

- OBS 等工具使用**恒定帧率（CFR）**。可变帧率录像中的重复帧、丢帧会污染真值，程序检测到时会警告。
- 游戏实际帧率要稳定达到录制帧率；达不到时录像里是重复帧，真值无效。
- 分辨率与串流一致（通常 1920x1080），SDR。应用内 DLSS FG 目前限 SDR，实验台也只处理 SDR。
- 码率尽量高（如 CQP 16–18），让真值帧干净。PC 录像没有 PS5 串流的压缩噪声，这一点与真实串流不同。
- 10–30 秒、包含快速镜头转动、HUD、细线和粒子的片段比长时间录制更有用。用 `--start`、`--duration` 截取。

## 构建

依赖与中文版相同（见 [Windows 中文版构建说明](windows-zh.md)），另需系统里有 `ffmpeg` / `ffprobe`（例如 `winget install Gyan.FFmpeg`）。Chiaki 自带的 FFmpeg 只含串流解码器，不能读 MP4/MKV，所以实验台调用外部 ffmpeg。

```powershell
./scripts/Build-VideoLab.ps1
```

脚本构建到 `build/zh-videolab`，运行自检，然后把 `enhancement-video-lab.exe` 复制到 `release/ngx`（不存在时为 `build/zh-stage`）。放在那里是为了直接找到 `enhancement/nvngx.dll`、`runtimes/` 和 Qt/libplacebo 的 DLL。也可以放在别处，用 `--stage <目录>`（或 `--worker`、`--runtime-dir`）指明位置，并保证 DLL 在 `PATH` 中。

## 使用

```powershell
cd release/ngx

# 1. 先确认录像和工具链（不需要显卡）
./enhancement-video-lab.exe --mode dry-run E:\rec\game_120fps.mkv

# 2. 质量评测：120 fps 录像 → 60 fps 真实帧 + DLSS FG 2x
./enhancement-video-lab.exe E:\rec\game_120fps.mkv --label "baseline" --history E:\rec\history.csv

# 3. 同时导出 FG 输出视频，便于逐帧查看
./enhancement-video-lab.exe E:\rec\game_120fps.mkv --record E:\rec\fg_out.mp4

# 4. 4x：120 fps 录像 → 30 fps 真实帧，三张生成帧分别评分
./enhancement-video-lab.exe E:\rec\game_120fps.mkv --multiplier 4

# 5. 实时节奏：60 fps 录像原速送入，2x 输出，看帧间隔是否均匀
./enhancement-video-lab.exe E:\rec\game_60fps.mkv --mode realtime
```

常用参数：`--start 12 --duration 20` 截取片段；`--frames 300` 限制真实帧数；`--width 1280 --height 720` 降分辨率；`--no-fg` 作为直通对照；`--nr` 叠加神经渲染；`--vsr 4 --output 2560x1440` 先超分再插帧（见下节）；`--vsync` 实时模式用 FIFO 呈现；`--worst 16` 多保存一些最差帧；`--help` 列出全部。

## 配合 VSR 超分

实际串流时如果启用 NVIDIA VSR（例如 1080p 串流超分到 2560x1440 的窗口），管线的默认顺序是：1080p 输入 → VSR 放大到窗口分辨率 →（NR）→ 在放大后的画面上做 DLSS FG；光流始终在 1080p 源画面上计算。录像仍然录 **1080p**（它对应 PS5 送来的画面），用 `--vsr` 让实验台走同样的顺序：

```powershell
./enhancement-video-lab.exe E:\rec\game_120fps.mkv --vsr 4 --output 2560x1440
./enhancement-video-lab.exe E:\rec\game_60fps.mkv --mode realtime --vsr 4 --output 2560x1440
```

`--vsr` 取 1–4，对应低、中、高、极高；`--output` 默认 2560x1440。需要 `runtimes/nvngx_vsr.dll`，VSR 没有激活时运行会报失败。

真值帧只有 1080p，所以 `quality` 模式把每张输出缩回 1080p 再评分，`--record` 导出的视频也是 1080p。这时 `real frame round trip PSNR` 不再是 100：它是真实帧经过 VSR 再缩回后的分数，也是生成帧分数的上限。比较不同版本时，看生成帧分数与这个上限的差距。`realtime` 模式的窗口显示的是真实的放大后输出。

## 输出

默认写入 `videolab-out/<时间>-<模式>/`，可用 `--out` 指定。

| 文件 | 内容 |
| --- | --- |
| `summary.txt` | 总览：帧数、PSNR/SSIM 的均值与 5% 分位、各相位分数、基线、增益、增强进程的 FG/光流 GPU 耗时 |
| `frames.csv` | 每张生成帧一行：真实帧序号、相位、真值在源视频中的帧号、各项分数、最差块坐标、`motion_psnr`（相邻真实帧的差异，越低运动越大） |
| `worst_NN_*_generated.png` / `_truth.png` / `_error_x4.png` | PSNR 最低的若干生成帧、对应真值、放大 4 倍的误差图 |
| `cadence.csv`（实时模式） | 每次呈现的时刻、帧号、相位（0 为真实帧）、与上次呈现的间隔、呈现耗时 |
| `--history` 指定的 CSV | 每次运行追加一行，用于对比不同版本 |

读数时注意：

- `real frames without generated output` 应接近 0（开头预热的一两帧除外）。数值大说明 FG 没有激活或频繁重置。
- `real frame round trip PSNR` 在不开 NR、VSR 时应为 100（逐位一致）。否则是共享纹理通路的问题，而不是 FG 的问题。
- 均值掩盖问题，优先看 5% 分位、最小值和最差块；再对照 `motion_psnr` 判断是否只在大幅运动时变差。

## 对比优化开关

本分支的增强进程带有几项可以关掉的优化（见 [VSR 与性能统计](vsr-and-statistics.md)）。实验台把它们做成了参数，不用改代码或重新构建就能逐项对比：

| 参数 | 作用 | 对应环境变量 |
| --- | --- | --- |
| `--fg-backend flow` | 实验性自建插帧：不调用 DLSS FG，沿光流向量对前后真实帧做运动补偿插值；默认 `dlss` | `CHIAKI_ENHANCEMENT_FG_BACKEND=flow` |
| `--motion-refine on\|off` | 强制开关向量精修（全局运动模型、中值、保边上采样）。默认关闭 | `CHIAKI_ENHANCEMENT_MOTION_REFINE` |
| `--no-motion-refine` | 等同 `--motion-refine off` | `CHIAKI_ENHANCEMENT_MOTION_REFINE=off` |
| `--fg-fallback` | 把 DLSS FG 低置信度区域混回最近真实帧（需精修开启）。默认关闭 | `CHIAKI_ENHANCEMENT_FG_FALLBACK=on` |
| `--no-fg-fallback` | 显式关闭回退（与默认相同） | `CHIAKI_ENHANCEMENT_FG_FALLBACK=off` |
| `--fg-vectors zero` | 诊断：给 DLSS FG 全零运动向量，用来确认它对输入向量的依赖程度 | `CHIAKI_ENHANCEMENT_FG_VECTORS=zero` |
| `--flow-quality fast\|medium\|slow` | NVOFA 光流质量档；不指定时 1080p 及以下为 medium | `CHIAKI_ENHANCEMENT_FLOW_QUALITY` |
| `--flow-scale 0.5` | 光流分辨率相对输入的比例（0.25–1，默认 1）。缩小后可跟踪更大的位移，向量更粗 | `CHIAKI_ENHANCEMENT_FLOW_SCALE` |
| `--flow-select projected\|direct\|candidates` | 自建后端的插值方式。默认 `projected`（向量前向投影）；`direct` 为逐像素两假设，`candidates` 为半分辨率候选选择 | `CHIAKI_ENHANCEMENT_FLOW_SELECT` |
| `--flow-bilinear` | 自建后端的补偿取样改用双线性（默认 Catmull-Rom 双三次，更锐但 PSNR 略低） | `CHIAKI_ENHANCEMENT_FLOW_SHARP=off` |
| `--dump-flow <目录> --dump-range 首-尾` | 诊断：把指定帧号范围内增强进程的原始光流网格写成 `flow_<帧号>_<宽>x<高>.s16` | `CHIAKI_ENHANCEMENT_DUMP_FLOW`、`CHIAKI_ENHANCEMENT_DUMP_RANGE` |
| `--chain nr-first` | 配合 `--vsr`：恢复旧顺序（NR 在 VSR 之前）；默认 `vsr-first` | `CHIAKI_ENHANCEMENT_CHAIN` |

`summary.txt` 的 `worker switches` 行和 `history.csv` 的 `worker_switches` 列记录每次运行实际使用的开关。对同一段录像跑一组：

```powershell
$clip = 'E:\rec\game_120fps.mkv'; $h = 'E:\rec\history.csv'
./enhancement-video-lab.exe $clip --history $h --label "dlss default"
./enhancement-video-lab.exe $clip --history $h --label "dlss zero vec" --fg-vectors zero
./enhancement-video-lab.exe $clip --history $h --label "flow backend"  --fg-backend flow
./enhancement-video-lab.exe $clip --history $h --label "flow raw vec"  --fg-backend flow --motion-refine off
./enhancement-video-lab.exe $clip --history $h --label "flow slow"    --flow-quality slow
```

读这组结果时注意：

- 置信度回退会在不可靠区域重复真实帧，所以要同时对照 `psnr_repeat`（重复上一帧的基线）。开启回退后分数若只是向 `psnr_repeat` 靠拢，说明回退触发得太多，画面会有顿挫而不是更准。
- 回退的目的是消除局部扭曲，主要体现在 `worst_tile_psnr` 和 `worst_*` 图片上，整帧 PSNR 均值可能变化不大。
- `--flow-quality slow` 的代价看 `summary.txt` 里的光流耗时，再用 `--mode realtime` 确认没有因此丢帧。

## 调优流程

1. 固定三四段不同场景的录像，先按上一节跑出各开关的基线。
2. 修改增强进程或管线，重新构建（`Build-EnhancementWorker.ps1`），更新 `release/ngx`。
3. 用相同参数和新的 `--label` 再跑，比较 `history.csv` 中的 PSNR 5% 分位、最差块和增益，并查看 `worst_*` 图片。
4. 画质没有退步后，用 `--mode realtime` 检查帧间隔的标准差、p99 和迟到次数。

DLSS FG 模型本身是 NVIDIA 的闭源运行库，本项目能调整的是它的输入、输出后处理和调度：

- 运动向量精修的阈值：吸附到全局模型的 0.75 像素、置信度误差 0.06 / 0.14、亮度相似度 0.08。它们目前写在 `worker.cpp`、`motion_model.h` 和 `worker_shaders.h.in` 里，是未经画面对比的初值，改动后需要重新构建增强进程。
- 全局运动模型的采用条件（至少 35% 采样点一致）和 RANSAC 参数（`motion_model.h`）。
- 传给 `DLSSG.*` 的参数（运动向量缩放、`MvecDilated`、深度、重置条件等）。
- `pipeline.cpp` 中各相位的呈现调度（微秒级截止时间、间隔平滑、`presentationLimited`）。这一项用 `realtime` 模式衡量，其余用 `quality` 模式衡量。

实时模式的呈现方式与界面一致：关闭垂直同步且启用 FG 时，显示表面支持就用 MAILBOX，否则 IMMEDIATE；`--immediate` 强制 IMMEDIATE，`--vsync` 使用 FIFO。实际选用的方式记录在 `summary.txt`。

## 验证边界

- 指标计算、真值对齐、ffmpeg 读写、输出文件和失败处理已在 Linux 上通过单元测试和 `dry-run` 端到端测试验证（`ctest`）。
- `quality` 和 `realtime` 模式接入真实增强进程的部分只做了编译检查，**尚未在 Windows + RTX 上实际运行**。首次使用请先用 `--frames 30` 试跑。
- 本分支增强进程的优化（VSR 前置、运动向量精修、置信度回退、微秒级排帧）本身也还没有在 RTX 硬件上构建和实测。实验台第一次运行同时也是这些改动的第一次实测，出问题时先用 `--no-motion-refine` 判断是否与它们有关。
- 抽帧模拟低帧率与真正以低帧率渲染不完全等同（例如逐帧运动模糊的曝光时间不同）。
- 这里测的是画质和呈现节奏，不包括输入延迟，也不代表显示器的实际扫描输出。
