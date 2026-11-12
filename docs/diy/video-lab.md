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

常用参数：`--start 12 --duration 20` 截取片段；`--frames 300` 限制真实帧数；`--width 1280 --height 720` 降分辨率；`--no-fg` 作为直通对照；`--nr` 叠加神经渲染；`--vsync` 实时模式用 FIFO 呈现；`--worst 16` 多保存一些最差帧；`--help` 列出全部。

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
- `real frame round trip PSNR` 在不开 NR 时应为 100（逐位一致）。否则是共享纹理通路的问题，而不是 FG 的问题。
- 均值掩盖问题，优先看 5% 分位、最小值和最差块；再对照 `motion_psnr` 判断是否只在大幅运动时变差。

## 调优流程

1. 固定一两段录像，先跑一次并用 `--label baseline --history history.csv` 记录。
2. 修改增强进程或管线，重新构建（`Build-EnhancementWorker.ps1` / `Build-VideoLab.ps1`）。
3. 用相同参数和新的 `--label` 再跑，比较 `history.csv` 中的 PSNR 5% 分位、最差块和增益，并查看 `worst_*` 图片。
4. 画质没有退步后，用 `--mode realtime` 检查帧间隔的标准差、p99 和迟到次数。

DLSS FG 模型本身是 NVIDIA 的闭源运行库，本项目能调整的是它的输入和调度：

- `worker.cpp` 中 NVOFA 光流的配置，以及把光流转换为运动向量的 `motionMain` 着色器（`worker_shaders.h.in`）。
- 传给 `DLSSG.*` 的参数（运动向量缩放、`MvecDilated`、深度、重置条件等）。
- `pipeline.cpp` 中各相位的呈现调度（`phaseIntervalMs`、`presentationLimited`）。前两项用 `quality` 模式衡量，这一项用 `realtime` 模式衡量。

## 验证边界

- 指标计算、真值对齐、ffmpeg 读写、输出文件和失败处理已在 Linux 上通过单元测试和 `dry-run` 端到端测试验证（`ctest`）。
- `quality` 和 `realtime` 模式接入真实增强进程的部分只做了编译检查，**尚未在 Windows + RTX 上实际运行**。首次使用请先用 `--frames 30` 试跑。
- 抽帧模拟低帧率与真正以低帧率渲染不完全等同（例如逐帧运动模糊的曝光时间不同）。
- 这里测的是画质和呈现节奏，不包括输入延迟，也不代表显示器的实际扫描输出。
