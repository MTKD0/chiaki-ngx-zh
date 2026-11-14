# AFG 自建插帧

0.2.0-zh.4 将 Claude 分支 `claude/independent-test-module-fps-hcksa9`（`399053ad`）的自建光流方案接入 Chiaki。进入“设置 → 视频 → RTX 设置”，在插帧方式中选择 **AFG**，启用插帧并选择 **2x / 3x / 4x**。串流中的增强面板也使用同一组选项。

AFG 与 DLSS FG、smooth motion 共用插帧开关。启用时更换方式先确认，取消保留原选择。Smooth Motion 仍由 NVIDIA App 控制；使用 AFG 或 DLSS FG 前请先在 NVIDIA App 关闭 Smooth Motion。

## 实现与限制

- AFG 使用 NVOFA 计算源分辨率光流，将向量投影到中间时刻，填补空洞、平滑场并按匹配误差处理遮挡，再插值生成画面。默认沿用上游 `projected` 路径；反向光流、2 像素网格等诊断开关维持上游默认关闭。
- AFG 不加载、校验或创建 DLSS FG 特征，无需 `nvngx_dlssg.dll` 和 SM86 代理，3x / 4x 不使用 DLSS 对 RTX 50 系列的倍率限制。仍需要支持 NVOFA 的兼容 NVIDIA RTX 显卡和 Vulkan，其他型号尚待实测。
- AFG 在有效前后帧的 GPU 插值完成后启用输出；静止或黑色画面的生成帧可以与真实帧相同。DLSS 的输出差异与驱动路径检查只适用于 DLSS FG。
- 当前仅支持 SDR。HDR 保留原画面；NR 的 HDR 路径保持可用。
- 默认链路为 VSR → NR → AFG，可以分别关闭 VSR 或 NR。它们各自仍需要对应运行库。
- 设置通过 IPC 10 明确传递后端选择；更换方式会重建增强进程和历史。GUI、增强进程与录像实验台须成套更新，不能混用 IPC 9 的旧文件。
- 录像实验台继续使用 `--fg-backend flow` 选择 AFG；Chiaki 界面的选择不受旧 `CHIAKI_ENHANCEMENT_FG_BACKEND` 环境变量干扰。
- 性能浮窗会将生成帧与处理耗时标记为 AFG。延迟合计仍是估算，网络未知时合计显示未知，不能视作完整输入到屏幕延迟。

## 验证

Windows、RTX 5070 Ti Laptop GPU：构建、中文翻译、设置持久化和互斥、QML 菜单、D3D12 自检通过。空运行库目录下 AFG 2x / 3x / 4x 的 30 张真实帧分别产生 29 / 58 / 87 张生成帧，首帧用于预热；VSR + NR + AFG 4x、重置和调整分辨率检查通过，DLSS FG 和 HDR NR 回归通过。

实际应用渲染诊断以合成画面送入真实调度、增强进程与交换链：30 FPS 的 AFG 2x / 3x / 4x 分别约为 60 / 91 / 121 FPS；60 FPS 的 AFG 3x 约为 177 FPS。它们是应用提交帧率，不能解释为显示器扫描输出帧率或真实 PlayStation、GTA6 的画质和延迟结果。

另测 2x / 3x / 4x 的静止图案和纯黑场景，逐张读回全部生成相位，RGB 与输入相差不超过 1 级，30 张真实帧对应 29 / 58 / 87 张生成帧。可用增强诊断程序复测，例如：

```powershell
./enhancement-bridge-test.exe --afg --no-fsr --static --black --multiplier 4 --verify-phases
```

本机一段 120 FPS 录像降至 640×360，以 30 FPS 输入 AFG 4x：开头静止片段的 12 张真实帧输出 33 张生成帧；12 秒处的 60 张真实帧输出 177 张生成帧，逐相位评分完成。后者平均 PSNR 34.54 dB、SSIM 0.9386，相比线性混合基线平均高 1.30 dB；该结果仅适用于这段缩小后的录像，不能代表其他场景或真实串流。

本版同时修正设置窗口的滚轮事件转发，较小窗口可滚动到插帧选项。启动无响应已由用户确认自行消失，本次未修改启动逻辑，也未清理用户配置。
