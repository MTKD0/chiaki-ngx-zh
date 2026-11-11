# Windows 中文版构建说明

当前开发版使用简体中文作为默认界面，保留英文切换。切换语言后重新启动；该设置独立于主机配置。临时使用英文可运行 `chiaki.exe --language en`，临时使用中文可运行 `chiaki.exe --language zh_CN`，均不覆盖保存的语言选择。

## 本机依赖布局

源码位于 `E:\Documents\AFG\chiaki-ngx`，新增工具、下载、缓存和构建输出均位于 E 盘。复用系统已有的 Visual Studio Build Tools 2022 / MSVC 14.44；未额外安装一份 MSVC。

| 内容 | 路径（相对于 E:\Documents\AFG） |
| --- | --- |
| MSYS2 CLANG64、Clang、CMake、Ninja、Qt 6.12 | `tools/msys64` |
| 锁定的 MSYS2 包与校验值 | `dependencies/msys`；清单为 `chiaki-ngx/scripts/ci/dependencies.json` |
| NVIDIA NGX SDK 头文件及许可证 | `dependencies/nvidia-sdk`，提交 `9981c6350dc58c0ce21f024fd65e31e851c31689` |
| FFmpeg 开发包 | `dependencies/ffmpeg` |
| SM86 插帧兼容包 | `dependencies/sm86` |
| 单独下载的本地 NR 运行库 | `dependencies/dlssnr/310.9.16` |
| GUI / 增强进程 / 测试 / 暂存目录 | `chiaki-ngx/build/zh-gui`、`zh-worker`、`zh-tests`、`zh-stage` |
| 本地可运行版本 | `chiaki-ngx/release/ngx` |

MSYS2 基础环境下载后在上述目录解压，使用 CLANG64 环境安装 `scripts/ci/dependencies.json` 锁定的包，并安装 MSYS 的 git、diffutils、make、unzip、zip。Qt LinguistTools 和中文 Qt 基础翻译包已加入锁定清单。其余依赖构建沿用 `scripts/ci/build-windows.sh` 中的 FFmpeg、SDL2-compat、libplacebo 步骤。

## 已准备环境中的后续编译

在 PowerShell 7 中进入源码目录，运行：

```powershell
./scripts/Build-Windows-Localized.ps1
```

脚本显式重新配置 CMake，然后构建 GUI、增强进程和测试，检查中文目录，部署到 `build/zh-stage`。默认从父目录的 `tools/` 和 `dependencies/` 复用依赖，可用参数修改位置。它不发布版本、不修改已安装的 Chiaki，也不自动打包或复制 NR DLL。

增强进程必须使用 MSVC 编译，以匹配 NGX 的 C++ ABI。GUI 采用 MSYS2 CLANG64。`enhancement/nvngx.dll` 实际上是本项目的独立增强进程，与 NVIDIA 的 `nvngx_dlssnr.dll` 模型运行库不同。

创建可分发 ZIP 时使用现有 `scripts/Package-EnhancementRelease.ps1`，先提供完整暂存目录、匹配的 FG/SM86 运行库与许可证。此脚本始终排除 NR DLL。开发者可以在本地测试目录另行放入匹配的 NR DLL；不要将它加入源码提交或 Release。

## 更新翻译与检查

在 CLANG64 环境中运行：

```bash
lupdate gui/src gui/include gui/enhancement/pipeline.cpp gui/enhancement/runtime_manifest.h \
  -no-obsolete -locations relative -ts gui/translations/chiaki_zh_CN.ts
python scripts/check-translations.py
```

编译时由 Qt `lrelease` 生成并嵌入 QM。应用翻译不依赖外部路径；便携包的 Qt 标准对话框翻译位于 `translations/qtbase_zh_CN.qm`。

`CHIAKI_SETTINGS_DIR` 可指定绝对路径，将设置隔离到该目录的 INI 文件，便于测试时避开已有注册主机和账号配置。`CHIAKI_DIAGNOSTIC_LOG_DIR` 可指定独立日志目录。

## 验证范围

2026-11-11，本机通过核心测试、翻译占位符/HTML 检查、增强与更新界面检查、插帧设置持久化检查、中英文 Qt/QML 启动检查、MSVC 增强进程模型和 D3D12 自检，以及 RTX 5070 Ti Laptop GPU 上的合成共享纹理测试：DLSS FG 2x/3x/4x、NR 与 4x FG 组合、HDR NR 回归。这些测试不等同于真实游戏串流、输入延迟、画质或显示器扫描输出验证。

当前已接入 DLSS FG 的 2x/3x/4x 选择，以及固定 2x 的 smooth motion 方式。smooth motion 需要用户在 NVIDIA App 中添加主程序并启用，尚未验证驱动实际激活。详细操作和硬件限制见[插帧说明](frame-generation.md)。GTA6 专项优化仍属后续研究；NR 渲染遍数表示重复增强画面，不能当作帧生成倍数。

2026-11-12，补充接入 NVIDIA VSR 四档质量、同类超分确认替换、4x 分阶段帧率统计及分项延迟估算。操作、运行库身份与验证范围见 [VSR 与性能统计](vsr-and-statistics.md)。
