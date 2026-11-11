# Chiaki-ngx 中文开发版

基于 [TheHiepDang/chiaki-ngx](https://github.com/TheHiepDang/chiaki-ngx) 的中文开发分支，用于 PlayStation 远程游玩与串流画面增强研究。

## 当前版本：0.2.0-zh.2

- 默认使用简体中文；在“设置 → 常规 → 界面语言”选择 English，重新启动后生效。
- 汉化主界面、设置、连接与注册流程、RTX 面板、统计浮层及常见运行库提示。保留显卡、编码格式和算法名称；底层驱动、NGX 及增强进程的原始诊断信息可能仍为英文。
- 保留实验性 DLSS 5 神经渲染和 FSR 重建；插帧方式可选择 **DLSS FG（2x/3x/4x）** 或 **smooth motion（仅 2x，需在 NVIDIA App 中手动启用）**。NR 默认关闭，串流期间按 Ctrl+Shift+F 打开 RTX 设置。
- 本地测试需要兼容的 `runtimes/nvngx_dlssnr.dll`。仓库和下载 ZIP 不包含该文件。本次开发使用版本 310.9.16.0，SHA256 为 `cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed`；这是未签名的社区兼容构建。
- 开发版通过浏览器打开本仓库 Releases 下载更新，避免被原项目的更新覆盖。私有仓库需要在浏览器登录有权限的 GitHub 账号。

下载便携 ZIP 后完整解压，运行 `chiaki.exe`。完整构建方法见 [Windows 中文版构建说明](docs/diy/windows-zh.md)，插帧设置与接入来源见 [插帧说明](docs/diy/frame-generation.md)。

## 后续开发方向

1. 在已接入的 DLSS 多帧路径之外，针对串流视频研究或自研帧生成算法，评估延迟、帧节奏、遮挡、快速运动、压缩噪声和 HUD 稳定性。
2. 以 GTA6 为专项验证目标；取得可测游戏画面和串流环境后再进行内容适配和效果评估。
3. 对实验性 DLSS 5 路径进行 GPU 耗时、显存和多遍处理开销分析，再决定优化方案。

DLSS FG 的 2x/3x/4x 已接入；自研算法、GTA6 专项优化和 DLSS 5 性能优化仍是后续计划。实际主机串流效果尚未验证。

原项目说明和署名保留如下。

---

![chiaki-ngx logo](gui/res/chiaking-logo.svg)

# chiaki-ngx

A fork of [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), DLSS5, Upscaler and Framegen for all RTX GPUs.

Tested on RTX 3000s.

For PlayStation setup and general help, see the [original documentation](https://chiaki-ng.github.io/chiaki-ng/).

## Installation

1. Download the latest **chiaki-ngx.zipP** from [Releases](https://github.com/TheHiepDang/chiaki-ngx/releases) and extract it.
3. Supply your own **`nvngx_dlssnr.dll`** in **`runtimes/`**.

Check [NVIDIA’s DLSS SDK](https://github.com/NVIDIA/DLSS) or [TechPowerUp](https://www.techpowerup.com/download/nvidia-dlss-dll/) for latest DLSS.

RTX 40/50 use native FG. RTX 20/30 use the bundled unofficial SM86 route.

## Updates

Choose **Release** or **Pre-release** at the bottom right. The app checks at startup; click **Update** when a newer version is available. Windows portable updates verify the download, preserve your settings and supplied NR DLL, then restart the app. End your stream before updating.

## NR passes

During streaming, open **RTX settings** with **Ctrl+Shift+F**. Enable Neural Rendering and choose **1, 2 or 3 passes**. Extra passes follow pass 1 by default. Check **Customize each pass**, then **Configure pass**, to edit them independently; saved custom values are kept when you switch back.

Each pass offers **Default / Natural / Cinematic** NR presets, **Intensity**, **Local Structure**, **Local Tone**, **Skin Structure**, **Automatic Mask**, resolution, FSR and sharpness. Presets select the NR model without moving your strength sliders. Skin Structure requires Automatic Mask; **−1 / Off** disables skin adjustments. Use **↺** beside a slider to restore its default for the selected pass. Resolution, including its reset, applies three seconds after the last adjustment. Frame generation runs after the final pass. More passes use more GPU time and memory; one pass remains the default.

## Showcase

Video show case DLSS5, Upscaler and Framegen with Dragon's Dogma 2:

[![Watch the video](https://img.youtube.com/vi/tHVEoDi1oKc/hqdefault.jpg)](https://www.youtube.com/watch?v=tHVEoDi1oKc)

Native NR (100% resolution) vs. Upscaled NR (75% resolution, 30-40% performance gain) comparison:

![Native NR](<assets/github demo images/native nr.png>)

![Upscaled NR](<assets/github demo images/upscaled nr.png>)

## Bug reports

Open **Settings → General → Log Directory → Open**. Include the matching **`chiaki_ngx_*.log`** and **`.previous.log`** if present, plus **`chiaki_session_*.log`** for streaming issues. Include crash text if created; share crash dumps privately. For update failures, also include the matching **`chiaki_update_*.log`** from the same log directory. NR pass diagnostics use **`[RTX][NR][Pass1]`**, **`[RTX][NR][Pass2]`** and **`[RTX][NR][Pass3]`**. See [reporting details](docs/diy/enhancements.md#bug-reports) for SM86 logs and other information to attach.

## Credits

| Project | Contribution |
|---|---|
| [Chiaki](https://github.com/thestr4ng3r/chiaki) and [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng) | Original Remote Play client, upstream foundation and contributors. |
| [DLSSG for SM86 — sdli1995](https://github.com/sdli1995/dlssg_for_sm86/) | Older-RTX FG: `version.dll` proxy, `sm86_backend.dll` backend and `fg_kernels.dll` kernels. |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NGX/DLSS interfaces and the bundled NVIDIA FG runtime. |
| [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | EASU upscaling and RCAS sharpening. |
| [Qt](https://www.qt.io/), [FFmpeg](https://ffmpeg.org/), [libplacebo](https://github.com/haasn/libplacebo) and [SDL](https://www.libsdl.org/) | Interface, media processing, video rendering and input. |

---

This project is not endorsed or certified by Sony Interactive Entertainment LLC or NVIDIA Corporation.
