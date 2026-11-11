# NVIDIA VSR、功能切换与性能浮窗

## VSR 使用

在“视频 → RTX 设置”或串流中的 RTX 面板选择 NVIDIA VSR：关闭、低、中、高、极高。此实现处理 SDR；HDR 保留原有 NR 路径和普通缩放。运行于 Windows Vulkan 渲染器，要求兼容的 NVIDIA RTX 显卡与驱动。

运行库放在主程序目录下的 `runtimes/nvngx_vsr.dll`。本机 `release/ngx` 已准备；可分发 ZIP 保持不包含 NR、VSR 这两项用户提供的 DLL。

当前验证并锁定 NVIDIA RTX Video SDK 1.1 的 `nvngx_vsr.dll` 1.6.0.0：

```text
SHA256 c3d88eea5ff7a548edefa66414cf6e77464d0947277c904f324dd23abf58a1ed
```

优先从 [NVIDIA RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk) 或 [NVIDIA NGC 的 SDK 文件](https://catalog.ngc.nvidia.com/orgs/nvidia/multimedia/models/dlpp/1.5/file-browser)取得运行库。此次官方端点连接失败，本机测试 DLL 取自 [dlss5-video-player v0.30.0 的完整程序包](https://github.com/2600th/dlss5-video-player/releases/tag/dlss5-video-player-v0.30.0)，提取后检查 NVIDIA Corporation 的有效 Authenticode 签名、版本及上述哈希，没有运行该程序包中的其他程序。DLL 受 NVIDIA 自身许可约束。

参考 [FzeNiX/chiaki-ng-enhanced](https://github.com/FzeNiX/chiaki-ng-enhanced) 提交 `9e878112dbc89d0edd326ee80a7f8cf55ee9185b` 的 `gui/src/rtxvsr.cpp`：使用 NGX feature 16、`Input1` / `Output`、`Rect.*` / `OutRect.*`、`VSR.QualityLevel` 参数。本项目将其接入现有 MSVC D3D12 增强进程；协议当前为 9，GUI 和增强进程必须同时更新。输入共享纹理保留原始可见区域分辨率，VSR 输出到窗口目标分辨率，随后执行 DLSS FG。

只接入 1.6.0.0 支持的四档超分模式。参考项目中的 Artifact Reduction / Streaming 扩展模式需要更新运行库，当前不提供这些未验证选项。

## 链路顺序与性能调整（协议 9）

默认顺序为 `解码 → VSR → NR → DLSS FG → 调色 → 呈现`：VSR 先在源分辨率画面上去压缩伪影并放大到窗口目标分辨率，NR 和插帧不再放大串流伪影。此顺序下 NR 分辨率百分比相对于 VSR 输出计算，例如 1080p 串流、1440p 窗口时 75% 即 1920×1080；NR 的差值按普通缩放叠回输出。启动串流前设置环境变量 `CHIAKI_ENHANCEMENT_CHAIN=nr-first` 可恢复旧顺序（NR 在源分辨率 → VSR），用于对比。VSR 运行库缺失或评估失败时，前置阶段改用普通缩放，NR/FG 继续工作。HDR 不受影响。

同时调整：

- 光流始终在源分辨率上计算，向量按比例换算到 NR 和插帧分辨率。不超过 1080p 时默认使用 NVOFA 中等质量档，更大时保持快速档；`CHIAKI_ENHANCEMENT_FLOW_QUALITY=fast|medium|slow` 可覆盖。耗时显示在浮窗的插帧分项里。
- 每帧的 GPU 阻塞等待由约 8 次减为 3 次：HDR 代理、场景切换缩略图和光流亮度合并为一次提交；缩略图使用常驻回读缓冲；光流参考帧改为交换而不是复制。
- 插帧输出校验只在增强进程启动后进行一次，场景切换只重置时域历史，不再触发全分辨率 CPU 回读。
- 生成帧按微秒级截止时间排帧，间隔取串流标称间隔并做平滑，不再使用取整毫秒。
- 关闭垂直同步且启用应用内 DLSS FG 时，交换链在显示表面支持的情况下使用 MAILBOX（不阻塞、不撕裂），否则保持 IMMEDIATE。该选择在创建交换链时生效，串流中途开关插帧不会切换。

以上调整尚未在 RTX 硬件上构建和实测：提交这些改动的机器没有 MSVC、NGX SDK 和 NVIDIA 驱动。合并前需重新构建 GUI 与增强进程并运行 `enhancement-model-test`、`--self-test` 和桥接测试。

## 同类功能确认

开启 VSR 时，如果已有 NR 的 FSR 重建、FSR/FSRCNNX 自定义超分或含空间超分的画质预设，会先弹出确认。确认后替换重叠处理；取消保持原设置。反向选择 FSR/FSRCNNX 或空间超分预设时同样处理。NR、色彩调整和插帧有不同用途，可以保留。

DLSS FG 与 smooth motion 在启用状态下切换也需确认。Smooth Motion 是外部驱动设置：切回 DLSS FG 时仍须在 NVIDIA App 手动关闭它，应用无法自动关闭或检测驱动的激活状态。

运行库缺失或哈希不匹配时不加载 VSR；普通缩放继续可用，同时保留可用的 NR/FG。运行库评估失败会停用 VSR 并显示原因。当前已有 HDR 限制继续生效。

## 帧率计数修复

旧浮窗只按真实帧 ID 去重：4x 的三张中间帧共用该 ID，错误地只计一次，30 fps 因而显示约 60 fps。现在使用“真实帧 ID + 生成阶段”作为标识，三张中间帧分别计数，重复渲染同一阶段不会增加帧率。

浮窗显示应用提交的真实帧、生成帧及合计。30 fps 串流的 4x 理想稳定输出约为 30 + 90 = 120 fps；首次历史预热、处理开销、实际丢帧和显示节奏会影响读数。不会直接把输入帧率乘四当作实测值，也不计入 Smooth Motion 在驱动内部生成的帧。

## 画面延迟估算

浮窗显示 `网络 + NR + 插帧 + VSR ≈ 合计 ms`，统计值约每 250 ms 更新：

| 分项 | 来源及含义 |
| --- | --- |
| 网络 | 连接阶段 Senkusha RTT 探测的一半；网络非对称会带来误差，并非实时单向测量。探测失败或未运行时显示未知，不采用连接的兜底值 |
| NR | D3D12 GPU 时间戳统计的所有 NR 遍数与重建耗时；没有 FG 时包括 NR 使用的光流 |
| 插帧 | 所有生成阶段的 GPU 时间、光流主机计时，以及生成帧排帧等待上界；因此也是保守估算 |
| VSR | VSR Evaluate 的 D3D12 GPU 时间戳差值 |

合计不包括主机编码、客户端解码、显示器扫描和外部 Smooth Motion 耗时，不能解释为完整的输入到屏幕延迟。网络未知时合计显示未知；选择 Smooth Motion 时额外显示外部耗时不可测。

## 本机验证

RTX 5070 Ti Laptop GPU 上，四档 VSR 的 640×360 → 1280×720 合成测试、NR + VSR + 4x FG 组合及 HDR NR 回归通过。组合测试为 30 张真实帧、87 张生成帧，首张用于预热。单元检查覆盖生成阶段计数、冲突检测、取消保持设置、接受替换、NR 保留及设置持久化；界面检查覆盖 VSR 选项与横向面板布局。

这些检查验证代码和 GPU 路径，不等同于真实 PlayStation 串流画质、屏幕可见帧率、端到端延迟或 GTA6 专项适配验证。
