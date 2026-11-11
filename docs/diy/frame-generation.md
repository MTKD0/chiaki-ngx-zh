# 插帧方式与接入说明

## 使用

在“设置 → 视频 → RTX 设置”或串流期间的 RTX 面板中启用帧生成，再选择：

| 插帧方式 | 可选倍数 | 实际处理 |
| --- | --- | --- |
| DLSS FG | 2x / 3x / 4x | 应用内 NGX 生成 1 / 2 / 3 张不同中间帧，再显示真实帧 |
| smooth motion | 仅 2x | 应用停用内置 DLSS FG，由用户在 NVIDIA App 中启用驱动插帧 |

DLSS 5 神经渲染独立于插帧方式，可以与任一方式组合。现有 HDR NR 路径保留；应用内 DLSS FG 目前仍限 SDR。Smooth Motion 的 HDR 支持与实际启用情况由 NVIDIA 驱动决定，本程序不承诺或检测其激活状态。

DLSS FG 的 3x/4x 需要兼容的 RTX 50 系列显卡和运行库。当前保守地对 GeForce RTX 50 系列放行多帧请求，并验证运行库输出；设备名称不等于运行成功。RTX 40 系列使用 2x。原有 RTX 20/30 非官方 SM86 路线保留 2x，未新增多帧支持。实际可见帧率还受 GPU 开销、串流帧率与显示器刷新率限制。

### NVIDIA App 的 smooth motion 设置

1. 在应用内选择 `smooth motion`。倍数栏固定为 `2x`。
2. 点击“复制主程序路径”。在 NVIDIA App 的“图形 → 程序设置”中手动添加该 `chiaki.exe`。
3. 在该程序的驱动设置中开启 Smooth Motion，并重新启动 Chiaki。使用兼容的 RTX 40/50 显卡、驱动和 Vulkan 渲染器。
4. 通过 NVIDIA 浮层确认驱动插帧效果。应用自身的 FPS 只统计应用提交的帧，不能用它证明 Smooth Motion 已激活。

需要添加的是主程序 `chiaki.exe`，不是 `enhancement/nvngx.dll`。移动便携程序目录后，需要重新核对 NVIDIA App 中配置的路径。

应用内设置只保存选用方式，不写入 NVIDIA 驱动配置。要停用 Smooth Motion，或切回 DLSS FG，请先在 NVIDIA App 中关闭 Smooth Motion，避免叠加两种插帧。

官方操作说明：[NVIDIA — Enabling Smooth Motion in NVIDIA App](https://nvidia.custhelp.com/app/answers/detail/a_id/5621)。

## DLSS 4 多帧接入来源

参考 [FzeNiX/chiaki-ng-enhanced](https://github.com/FzeNiX/chiaki-ng-enhanced)，检查时提交为 `9e878112dbc89d0edd326ee80a7f8cf55ee9185b`。

重点是 `gui/src/framegen.cpp` 中的 `DlssBackend::record` 和 `docs/setup/framegen.md`。原实现走 Vulkan NGX：查询多帧能力，针对同一个真实帧对逐次调用 Evaluate，为每张中间帧提供独立纹理、总生成帧数和从 1 开始的内层索引；先显示生成帧，最后显示真实帧。

本项目沿用已有 Vulkan ↔ D3D12 独立增强进程，没有移植参考项目的 FSR、Flow、VSR 后端。接入采用相同的 NVIDIA DLSS 4 参数语义：

- `MultiFrameCount = 倍数 - 1`，分别为 1、2、3。
- `MultiFrameIndex = 1..MultiFrameCount`，每次 Evaluate 写入独立输出纹理。
- 同一真实帧对使用相同的 `BackbufferFrameID`；每个真实帧递增一次。
- 共享资源增加至最多三张生成纹理。进程间协议当前为版本 9，必须成对部署 GUI 和增强进程。
- 按串流帧间隔除以输出倍数调度各阶段。旧帧的所有阶段结束后才能采用下一组输出，避免覆盖尚未显示的纹理。
- 首次激活时验证每张输出非黑、区别于相邻真实帧，并验证各中间帧互不重复。没有把重复显示同一张帧计作 3x/4x。

参考项目和本项目保留各自原作者的署名及 AGPL 许可。NVIDIA 接口和运行库仍受各自许可约束。

## 验证边界

本机 RTX 5070 Ti Laptop GPU 上，合成 GPU 测试通过 3x、4x：30 张真实帧分别计数到 58 张、87 张生成帧，首张真实帧用于历史预热。测试检查共享资源、独立输出及多阶段消费，不代表游戏画质、输入延迟或显示器实际扫描输出已经验证。

Smooth Motion 尚未通过实际 NVIDIA App 设置和外部帧率测量验证。真实 PlayStation 串流及 GTA6 专项效果仍待验证。
