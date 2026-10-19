# eac-arm64-emulation-poc

**概念验证：在 arm64 Linux 机器上通过 FEX-Emu 运行带正版 Easy Anti-Cheat 的 VRChat —— 以及为达成这一目标所做的一切。**

> **提醒：此处有龙（风险较高），请注意相关风险。** 我个人尚未观察到任何问题，但这是在开发者不支持的配置（模拟的 x86 CPU，通常还在虚拟机内）上运行受反作弊保护的游戏的尝试。反作弊或游戏服务条款可能将其视为违规，账号可能被采取行动；请使用你承受得起损失的账号。这些脚本会替换 Steam 的 FEX 工具内的文件（`restore` 会恢复 Valve 的原版文件），此处任何内容均未得到 Valve、Epic Games、FEX-Emu 项目或任何游戏开发商的认可。不提供任何担保；参见[许可协议](#license)。

这是一个独立的仓库。它记录了调查过程以及对 **FEX-Emu**（2610；基于 2609.1 开发）所做的修改，这些修改是在 arm64 主机上启动一款带 Epic 的 Easy Anti-Cheat for Linux/Proton 的 x86-64 游戏所必需的：修改以补丁文件的形式保留，你可以自行应用，并附有说明每个补丁修复了什么问题的测试，以及发现这些问题的诊断工具。

![VRChat 在 Apple M2 上的 Fedora Asahi Remix 中通过打补丁的 FEX-Emu 运行，旁边是主机的 `fastfetch`](docs/img/vrchat-m2.png)

它刻意限定为**一块 SoC 上的一款游戏**（Apple M2 上的 VRChat：在 [muvm](https://github.com/AsahiLinux/muvm) 微虚拟机内的 Fedora Asahi Remix 下开发，也已在 macOS 上验证，见下文）。把它当作起点并进一步推进——另一款游戏、另一块 SoC、另一台主机——但请在此处报告你发现的问题，而不是只在 fork 中修复；参见[Bugs、修复与上游贡献](#bugs-fixes-and-upstreaming)。

**macOS。** 同一套打补丁的 FEX 也能在 macOS 27 的 Apple M2 MacBook Air 上运行带正版 EAC 的 VRChat，环境为 [steamac](https://github.com/fxgl/steamac) libkrun 客户机（Valve 的 ARM64 SteamOS，Steam 自带的 FEX 兼容工具被替换为打补丁的构建）。结果、耗时与限制：[docs/macos-port.md](docs/macos-port.md)。搭建步骤及后续修复（包括用于 Direct3D 11 的 DXMT 图形路径）位于另一个独立的 fork：[StonkGuy/steamac](https://github.com/StonkGuy/steamac)（`main` 及其面向玩家的发行版，`fex-eac` 用于开发）。

**Steam Frame。** 你是看到 Steam Frame 相关帖子来到这里的吗？从 [docs/steam-frame.md](docs/steam-frame.md) 开始：获取 shell，使用 [`scripts/steamos/`](scripts/steamos) 在 Frame 上构建打补丁的 FEX，将其替换到 Steam 的 FEX 工具中，设置启动选项。已有其他用户报告可用；此处未做测试。

困难之处不在于 CPU 速度，而在于**保真度**。反作弊启动器通过 `ptrace` 将其客户端注入游戏，而模拟器必须在 `ptrace`、信号和 `/proc` 方面表现得像一个真正的 Linux 内核，这是其他任何东西都未曾要求过的；后来，VRChat 自身的网络线程又暴露了 FEX 信号处理与 Linux 不同的另外两个地方。

## 需要什么

| 你需要 | 原因 |
|---|---|
| **FEX-Emu 2610 及补丁：[StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) 标签 `eac-2610.2`** | 参见 [docs/patches.md](docs/patches.md)；用 `scripts/build-fex.sh` 构建 |
| **arm64 Linux 主机**（已验证：Apple M2 + Fedora Asahi Remix + muvm，或 macOS 27 + steamac libkrun 客户机） | 16 KB 页面的主机需要 4 KB 页面的客户机，因此需要 muvm 虚拟机；在 4 KB 页面主机上 FEX 可直接运行（未测试） |
| **Proton Experimental 11.0** 和 *Proton EasyAntiCheat Runtime*，Steam 与 FEX 位于虚拟机边界的同一侧 | 此处所有内容均基于这些版本测量 |
| 主机内存调优（`scripts/host-tune.sh`，Asahi，16 GB） | 16 GB 下，约 8 GB 虚拟机加 GPU 缓冲时主机内存耗尽；zswap/zram/水位设置可避免 OOM killer |
| `scripts/` 中的启动选项和 DXVK caps | `scripts/set-launch-options.sh`、`scripts/dxvk.conf` |

补丁系列分为**修复**和**诊断**两类——补丁 0001（`ptrace`）、0002（SMC 热页）和 0007（信号掩码）是运行所必需的；0008 和 0010 是同一领域的正确性修复，其余是性能、内核保真度或工具。[docs/patches.md](docs/patches.md) 解释了每个补丁及其理由：

| 补丁 | 分组 | 用途 |
|---|---|---|
| 0001 `ptrace` 模拟 | 修复 | EAC 启动器用 `ptrace` 注入其客户端；FEX 模拟被跟踪进程（tracee）的 x86-64 视角。没有它，启动器会失败（`Unexpected error. (#1)`） |
| 0007 信号掩码 | 修复 | 客户机处理程序以 Linux 信号掩码运行。没有它，约每两次会话就会出现一次 Photon 超时 |
| 0008 系统调用信息 | 修复 | 从系统调用进入的处理程序会保留其设置的寄存器 |
| 0010 seccomp 陷阱 | 修复 | `SECCOMP_RET_TRAP` 的 `SIGSYS` 报告的是系统调用之后的指令，与 Linux 一致；没有它，Wine 的系统调用陷阱过滤器会永远重新执行被陷阱捕获的系统调用 |
| 0004 廉价失效 | 性能 | 约 200 个线程时，数据页上的 `mmap`/`mprotect` 便宜约 22 倍；没有它，join 需要 60 多秒 |
| 0002 SMC 热页 | 修复 | 持续触发自修改代码缺页的页面不再被写保护。没有它，反作弊客户端永远加载不完，游戏无法启动 |
| 0003/0005/0006/0009 | **诊断** | 统计、采样器和线程快照（0009 在快照中加入了 futex 字）——是工具，不是修复 |
| 0011–0020 内核保真度 | 保真度 | 真实内核在 `arch_prctl`（0011）、调试寄存器（0012）、`/proc/<pid>/status`（0013）、未知 regset（0014）、`restart_syscall`（0015）和信号帧（0016–0020）上给出的答案。不在 VRChat 的路径上；其他游戏或 Wine 会观察到它们 |
| 0022–0026 第二波 | 保真度 | 每次 seccomp 陷阱的主机栈泄漏（0022）、客户机读取 FEX 的 call/ret 影子栈保护页（0023）、IRETQ 目标上的 TF 陷阱（0025）、线程在信号备用栈上退出时的信号备用栈（0026）；0024 是分析器选项。在第二款游戏（THE FINALS）上发现，其反篡改引导程序会观察它们；仅涉及陷阱、故障和退出路径 |
| 0033 `/proc/<pid>/maps` | 修复 | maps 文件只列出被模拟进程的映射（低于 2^47），而不列出 FEX 及其 AArch64 主机库。没有它，EAC 启动器中从子进程 maps 里挑选库并注入的模块会选择主机库并以 `210 Unexpected error. (#1)` 失败（在 Mini Royale 通过 Proton 时出现；VRChat 的部署仍提供较旧的模块，尚未受影响） |
| 0035 SMC，远程写入 | 修复 | 客户机设为可写（`mprotect` RX 到 RWX）的代码页对其他进程保持可写，因此用 `NtWriteVirtualMemory` 写入的钩子不再以 `STATUS_PARTIAL_COPY` 失败。基于 yungtry 的补丁 0028；使用者为 The Division 2（据其作者）；仅在 `SMCChecks=mtrack` 下生效 |

## 各处的改动

涉及三种不同类型的改动。只有第一种是新代码；Wine 或 Proton 中没有任何东西被修补。

| 层 | 内容 | 类型 | 位置 |
|---|---|---|---|
| **FEX-Emu（模拟器）** | 0001 `ptrace` 模拟，0007/0008/0010 信号与 seccomp 保真度，0004 失效，0002 SMC 热页，0011–0020 内核保真度缺口（调试寄存器、`/proc`、`arch_prctl`、regset、`restart_syscall`、信号帧），0022/0023/0025/0026 主机栈、故障与单步保真度，0033 `/proc/<pid>/maps`，0035 SMC 远程写入，0024 分析器选项，0003/0005/0006/0009 诊断 | **新源代码**，FEX-2610 上每个补丁一个提交 | [StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) 分支 `eac-2610`（测试在 `eac-tests/` 中），由 `scripts/build-fex.sh` 构建；`patches/` 是生成的快照 |
| **Proton / Wine** | `WINE_CPU_TOPOLOGY=16:…`（报告 16 个 CPU；修复 join 前的卡顿） | **原版 Proton 设置**，无补丁 | 启动选项，`scripts/set-launch-options.sh` |
| | `EAC_LAUNCHERDIR`、`PROTON_EAC_RUNTIME`（加载 EAC 运行时）、`WINEDEBUG=-all`、`PROTON_USE_XALIA=0` | 原版 Proton 设置 | 启动选项 |
| | `DXVK_CONFIG_FILE` → `dxgi.maxDeviceMemory`/`maxSharedMemory` = 3072 | 原版 DXVK 设置 | `scripts/dxvk.conf` |
| **虚拟机和主机** | 客户机 NIC MTU 限制为 1500；≥ 8 GB 客户机内存；muvm 4 KB 页客户机 | 环境 | `scripts/vm/steam-vm.sh`、`scripts/vm/guest-setup.sh` |
| | 16 GB 主机上的 zswap/zram/水位/预读调优 | 环境 | `scripts/host-tune.sh` |
| | 可选的 SMBIOS/DMI/PCI/主机名拟真层 | 环境，可选 | `scripts/vm/realism.sh` |

## 涉及的内容

1. **EAC 启动器**需要其被跟踪进程的 x86-64 `ptrace` 视角、客户机二进制文件的 `/proc/<pid>/exe` 以及客户机 `int3` 的信号投递停止；整个过程由一个独立测试复现（`eac-tests/ptrace-inject`，[docs/how-it-works.md](docs/how-it-works.md)）。
2. **耗时一分钟的 join** 是约 200 个线程进程中的代码失效开销（补丁 0004，借助补丁 0003 的统计发现）。
3. **主机 OOM 杀掉整个虚拟机**：内存调优和虚拟机大小（[docs/tuning.md](docs/tuning.md)）。
4. **Photon 超时**（约每两次会话一次）是 Wine 等待协议中丢失的唤醒：FEX 以错误的信号掩码运行客户机信号处理程序（补丁 0007）。找到它需要一个不对反作弊做 `ptrace` 的线程快照设施、一个独立的 Wine 复现程序，以及对 wineserver 的 epoll 注册的检查（[docs/disconnects.md](docs/disconnects.md)）。测试 0007 时又发现了同族的第二个 FEX 错误（信号处理程序内寄存器丢失，*补丁 0008*）。
5. **某些启动时约 60 秒的冻结**（Photon 区域查找超时，然后进入 `Disconnecting` 重连循环）是游戏的 IL2CPP 线程池在 4 vCPU 虚拟机上饿死。不是 FEX 或 Wine 的错误：用 Proton 的 `WINE_CPU_TOPOLOGY` 报告 16 个 CPU 即可消除。通过进程内线程快照和 wineserver 套接字状态的被动读取器发现（[docs/disconnects.md](docs/disconnects.md)）。

## 快速开始（已验证平台：Fedora Asahi Remix）

在 Steam Frame（或其他 arm64 SteamOS 设备）上，请改用 [docs/steam-frame.md](docs/steam-frame.md)。

```sh
sudo dnf install steam muvm fex-emu && steam          # 基础安装，登录，安装 VRChat，启动一次
scripts/build-fex.sh                                  # 将 StonkGuy/FEX 标签 eac-2610.2 克隆并构建到 work/FEX
scripts/vm/install-overlay.sh
(cd work/FEX/eac-tests/ptrace-inject && ./build.sh clang)
scripts/vm/run-test-in-vm.sh work/FEX/eac-tests/ptrace-inject/ptrace_inject_test   # 必须输出 RESULT: PASS
scripts/vm/run-test-in-vm.sh <其他 eac-tests 二进制>  # 例如 signal-mask、signal-regs：RESULT: PASS
sudo scripts/host-tune.sh                             # 内存调优（可选，16 GB 强烈建议）
scripts/set-launch-options.sh                         # 关闭 Steam 后运行
REALISM=1 VM_MEM_MB=10240 VM_VRAM_MB=3072 scripts/vm/steam-vm.sh   # 在虚拟机中启动 Steam（已验证配置；REALISM=1 可选），然后启动 VRChat
```

完整的 A 到 Z 指南、精确的已验证版本以及什么会破坏可用配置：[docs/setup-asahi.md](docs/setup-asahi.md)。工具：[docs/tools.md](docs/tools.md)。哪些可用、哪些待定：[docs/status.md](docs/status.md)。

## 范围与限制

* **不是反作弊绕过。** 没有任何东西伪造、重放或短路 EAC 的结果；不包含任何 Epic 或 VRChat 的二进制、密钥或痕迹。补丁只是让 FEX 在 `ptrace`、信号和 `/proc/<pid>/exe` 方面表现得像真正的 Linux 内核，运行的 EAC 客户端是来自 Valve 的 Proton EAC Runtime 的原版、未修改版本。
* **一个坦率说明的告诫。** 可选的 `scripts/vm/realism.sh` **不**触碰 EAC，但它也不是纯粹的模拟保真度：它向客户机提供看似合理的 SMBIOS/DMI 数据，使其看起来不那么像微虚拟机（并且，比 VRChat 的指南更进一步，还包括 PCI 设备列表和主机名）。这是遵循 VRChat 自己发布的指南《在虚拟机中使用 VRChat》的*环境适配*，该指南称 EAC 的 VM 阻止是其 CPUID 管理程序厂商检查，并且**"在某些情况下你可以让虚拟化与 EAC 共存，我们也不介意你这样做。"** 请将其理解为政策声明而非许可：属于"容忍但不受支持"，本仓库将该脚本标记为**可选**：没有它 VRChat 也能运行。它可能对反作弊检查机器硬件身份的其他游戏有用。它不修改游戏或反作弊。
* **VRChat 不支持。** VRChat 不支持虚拟机或模拟；Epic/VRChat 可随时更改任何内容，是否执行由他们决定。自行承担风险，绝不运行修改过的客户端。切勿对运行中的游戏附加调试器或 `perf`：EAC 会报告。

## Bugs、修复与上游贡献

**发现 bug，或发现打补丁的 FEX 在某些地方仍与 x86-64 Linux 行为不同？** 请在此处以 issue 形式报告。如果你已经在自己 fork 中修复，请考虑也在此处发起拉取请求（附带测试），这样大家都能获得修复，测试套件也能覆盖它。FEX 代码改动以拉取请求形式提交到 [StonkGuy/FEX](https://github.com/StonkGuy/FEX)，针对当前的 `eac-<release>` 分支，并在其 `eac-tests/` 中附带测试。

本仓库不向上游 FEX-Emu 或 Proton 发送补丁，也与它们无关（steamac 的改动直接与其维护者讨论）。如果你审查过某个补丁并认为它足够好可以上游，**欢迎你自行提交**：保留其测试，遵循上游项目自己的流程，并注明来源。其他打过补丁的组件（libkrun、Mesa/KosmicKrisp、virglrenderer、DXMT）同样适用，除非该项目自己的规则另有规定——例如 DXMT 不接受 AI 辅助的贡献。

## 维护与支持

我会尽量让打过补丁的组件跟上其上游版本，每次更新都重新运行回归测试，一旦上游覆盖了某个行为就移除对应补丁。对于游戏特有的问题我会尽力帮忙，但我不能保证每款游戏或每个修复都能奏效。

每个 FEX 版本都以同样方式处理：将补丁变基到该版本，作为 StonkGuy/FEX 的 `eac-<release>` 分支，在客户机中运行 `eac-tests` 并在本机运行作为 oracle，检查一次真实 VRChat 会话，移除所有上游现已提供相应行为的补丁，并打上标签；本仓库和 steamac fork 随后固定到新标签。

## 其他 EAC 游戏和 FEX 主机

Proton EAC 运行时对每款提供 Linux EAC 的游戏都是同一套启动器/客户端机制，因此 `ptrace` 工作并非 VRChat 特有；每款游戏仍有自己的 EAC 构建、完整性检查和服务器端策略。VRChat 已端到端验证；Mini Royale 的 EAC 启动器在 0033 下也能到达 `301`（游戏本身未越过其账号提示继续游玩）。不同游戏结果不同：每款游戏自带其 EAC 构建和检查，因此未测试的游戏可能可用也可能不可用。除 Asahi 和上述 macOS 客户机外，其他 FEX 主机均未测试，Steam Frame 除外（来自另一位用户 @VirtuallyNoxide 的发现；搭建见 [docs/steam-frame.md](docs/steam-frame.md)）——参见 [docs/other-platforms.md](docs/other-platforms.md)。

## 许可协议

MIT（参见 [LICENSE](LICENSE)）；补丁修改的是同为 MIT 许可的 FEX-Emu。
