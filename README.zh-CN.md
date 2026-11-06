# FM1 Open V14

[English](README.md) · 简体中文

8 轨、8 槽采样、16 套 FX DEMO，已含 SDK（二进制库与接口头文件）。

[在线工作台与开发包](https://open.m-vave.com/fm1-workbench) · [官方论坛](https://forum.m-vave.com)

工作台支持固件更新、乐器预览，以及工程、采样和 DX7 音色管理。点击“源码与 SDK”可下载与线上版本对应的开发包。更新前请备份工程、音色和采样，写入期间保持供电。

## V14 功能

- **DX7 音色库：**128 个位置，分为 4 组，每组 32 个，默认沿用 M-VAVE FM1 原厂 V15 的 128 个出厂音色及顺序。已保存的用户音色库优先。支持 DX7 单音色及 32 音色库 SysEx 导入／导出，不处理扩展参数；修改音色库不会改变工程中已经保存的音色。
- **轨道角色：**在 SEQ **8/8 TRACK ROLE**，K1 选择 NORMAL、PLAY 或 OFF，K2 设置输入 MIDI 通道 1–16。PLAY 在切换工程时保留本轨音色和持续按住的音符。沿用原有 MUTE／SOLO 操作，NORMAL 与 PLAY 各自判断独奏。PLAY 音色修改不随工程保存，请导出轨道音色；DX7 也可存入独立库。
- **步进录入：**轨道设为 NORMAL 时，停播后按 REC 演奏，全部按键松开后前进一步，DRUM 808 和 SAMPLER 同样适用。重叠按下的声音可录在同一步；播放时按 REC 则按时间录入。PLAY 轨用于现场演奏，不参与录入。
- **速度与延迟：**内部速度为 30–300 BPM。可在网页使用 TAP、在 FX DELAY 页连按 SEQ，或按住 HOME 连按 SEQ 设置速度。延迟支持速度同步和约 30–2040 毫秒的固定时间。

## MIDI

FM1 接收 DIN、USB、BLE MIDI 输入，支持 USB／BLE 输出。默认输入通道 1–7 对应轨道 1–7，通道 10 对应轨道 8；可在 TRACK ROLE 修改各轨输入通道。

- **5/8 SYNC / LOOP：**设置内部／外部时钟、速度、循环长度和时钟来源。
- **6/8 MIDI TRACK：**设置 SEQ／ARP 的 LOCAL、BOTH 或 MIDI 输出模式、输出通道、输出端口和实体演奏 KEY OUT。
- **7/8 MIDI ROUTING：**设置 LOCAL、CLK OUT 和 MIDI Thru。使用 Local Off 由 DAW 监听时，请开启 KEY OUT 并选择输出端口。

MIDI 偏好独立于工程保存。浏览器模拟器仅预览这些设置，不向外部端口发送演奏 MIDI；实机请通过机身设置。支持 MIDI Clock、Start、Continue、Stop 和 Song Position Pointer 输入；时钟丢失时停止同步播放。

## 更新与兼容

V13 可以升级到 V14。工作台支持刷回原厂 V15，也支持从兼容的第三方固件迁移；标准 FM1 升级协议仍然可用。兼容性已针对部分固件版本验证，不代表所有第三方版本均已测试。

互刷不等于工程和采样格式互通，请保留导出的备份以便回退。旧固件可能不支持 V14 轨道偏好、独立音色库以及同步／扩展延迟设置。

备份独立音色库时，请分别读取并导出 A–D 四组。项目文件不包含采样音频；恢复自定义采样需使用对应的采样库备份，写入内置采样库会替换现有采样。

8 槽共享约 19.55 秒的新采样空间。原有较大采样库仍可读取；写入 DX7 独立库前，可能需要先备份并缩小采样库。

## 编译

可以在 **Windows** 或 **macOS／Linux** 上编译。两条路线执行相同的三种构建，输出到相同的文件夹：

| 构建 | Windows | macOS / Linux |
|---|---|---|
| 固件及其升级包（`build/dx7/`） | `build.bat` | `./build.sh` |
| 网页预览（`build/preview/index.html`） | `build.bat --web` | `./build.sh --web` |
| 固件与完整工作台网页（`build/official-web/`） | `build.bat --package` | `./build.sh --package` |

**获取源码：直接克隆本仓库**，放在任意文件夹均可。下面所有命令都在仓库根目录（含 `build.bat` 的文件夹）中运行，不依赖仓库所在位置：

~~~
git -c core.autocrlf=false clone https://github.com/m-vave/fm1_open.git
cd fm1_open
~~~

（`-c core.autocrlf=false` 可避免 Git 改动换行符：构建会逐字节校验 SDK 文件。）详细说明、故障排查，以及两条路线如何比对，见[构建指南](docs/BUILDING.md)（英文）。

### 方案 A：Windows

需要 Windows x64、PowerShell、`curl.exe` 和 `tar.exe`，以及 **杰理 Windows 工具链 2.5.3**（内含 PI32v2 clang 4.0.1；FM1 继续使用 `r3` CPU 目标）。构建会按 `sdk/manifest.json` 校验全部 22 个工具链文件；旧版 V14 开发包固定使用 2.5.2，会拒绝 2.5.3，升级时请一并更新完整源码和 SDK 开发包，并保留清单校验。杰理安装程序依赖 Code::Blocks，请按以下顺序安装工具链：

1. 安装 [Code::Blocks](https://www.codeblocks.org/downloads/binaries/)。
2. 启动一次 Code::Blocks，然后**彻底关闭**：只关窗口不够，请在任务管理器中结束所有 `codeblocks.exe` 进程。
3. [下载杰理 2.5.3 安装包](https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe)并运行，保持默认目录 `C:\JL`。杰理官方的[安装指南](https://doc.zh-jieli.com/Tools/zh-cn/dev_tools/dev_env/jl_toolchain.html)（中文）可作参考。

本版本 `pi32/bin/clang.exe` 的预期 SHA-256 为 `d4bc97cf421c1ccc373dd8924ce88720ff7457b3f1478d15df4aeda6ae923bce`。然后在 PowerShell 中、于仓库根目录执行：

~~~powershell
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
.\build.bat                  # 固件
.\build.bat --web            # 网页预览
$env:FM1_SOURCE_URL = "https://github.com/<你的账号>/fm1_open/archive/$(git rev-parse HEAD).zip"
.\build.bat --package        # 固件与完整工作台网页
~~~

首次执行 `--package` 会下载 WASI SDK 27（可设置 `FM1_WASI_SDK` 复用已有安装）。仅在本地编译固件、不制作分发包时，可执行 `./sdk/Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'`，无需设置分发源码地址。

### 方案 B：macOS 与 Linux（Docker）

需要 Docker 和 Node 18 或更新版本，无需 PowerShell，也无需 Windows。在仓库根目录执行：

~~~bash
./build.sh --setup            # 仅需一次：下载杰理 Linux 工具链（26 MB）和 WASI SDK 27，均校验哈希
./build.sh                    # 固件：编译、SDK 检查、生成升级包
./build.sh --web              # 网页预览
./build.sh --package --source-url "https://github.com/<你的账号>/fm1_open/archive/$(git rev-parse HEAD).zip"
~~~

它在容器中使用与 Windows 路线相同的编译器版本（clang 4.0.1）、相同的编译参数和相同的检查，约需 20 秒。Windows 构建仍是基准；已做过哪些比对见[构建指南](docs/BUILDING.md#how-the-two-routes-compare)。

### 分发构建

正式分发时，`FM1_SOURCE_URL`（或 `--source-url`）须为本次构建对应源码包的 HTTPS 地址，包内须含本次修改、许可声明及编译／安装脚本。GitHub 上持续变化的 `tree/main` 页面不是对应的源码包。

[逐轨调音接口](app/instrument.h)支持自定义调音，暂未提供专用调音编辑界面。

## 许可

APP 采用 [GPLv3＋FM1 SDK 链接例外](LICENSE.md)。设备引擎以二进制 SDK 提供，遵循单独条款。分发时请保留适用的 [SDK 条款、第三方许可和贡献者声明](THIRD_PARTY.md)。
