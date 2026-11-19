# Darwin 27 USB 端口上限：现状与诊断入口

日期：2026-11-19 · 目标机：TUF GAMING Z690-PLUS WIFI D4 / i7-12700K / macOS 27.2 (26B123)

## 1. 本次实测事实

| 项 | 值 | 来源 |
|---|---|---|
| 启动 ESP | disk3s1（Partition UUID `F20BD7C9-60E1-4F27-B1AC-3826B8EB9A67`） | `iorg` boot-path + `diskutil info` |
| 启动 OC | `DBG-108-2026-11-18`，sha256 `11646086…` = `out/DEBUG/OpenCore.efi` | NVRAM `opencore-version`、文件哈希 |
| fork 产物 | sha256 `2884f859…`（含 `part 2 on Tahoe`），**未部署** | 文件比对 |
| 注入 port-count | 25（`<19000000>`） | ioreg |
| UTBMap 条目 | 21（HS01–HS12 + SS01–SS09） | Info.plist |
| 实际根端口 | **15 × `AppleUSB20XHCIPort`（0x14100000–0x14F00000）** | ioreg |
| 实际 SS 端口 | **0** | ioreg |

## 2. 现有 quirk 的命中点（已逐字节确认）

- `IOUSBHostFamily` → `AppleUSBHostPort::setPortLocation()` +0x42：
  `41 83 ff 0f | 0f 87`（`cmp r15d,0xf; ja` 返错 0xE00002C2），补丁 `0F 0F 87 → 40 0F 87`。
- `AppleUSBXHCI` → `createPorts()` +0x518（27.2）：
  `83 f8 0f | 0f 83`（`cmp eax,0xf; jae` 走 "ports limit reached"），补丁 `0F 0F 83 → 40 0F 83`。

两条补丁在 11-18 的 OC 日志里都报 `[OK] … replace count - 1`，但结果仍是 15 口。

## 3. 26 与 27 的 createPorts 指令级对比

`diff` 两版 `__ZN12AppleUSBXHCI11createPortsEv`（1079 vs 1076 条指令）后，只有寄存器分配、
kalloc_type_view 名字、ivar 偏移不同（26: `0x5bc/0x4d0`，27: `0x634/0x550`），**端口上限判断
逻辑本身没有变化**。所以「27 换了一个新判断」这一假设，目前没有指令级证据。

27 新增/相关的字符串：

- `UsbHostControllerTierLimit`（setPortLocation 里读取，默认 7）
- `speed index %d has inconsistent speed IDs (%d and %d)`（本次启动实际出现：index 4，5/6）
- `discarding type %d port %d status %d`
- `HCSParams1 0x%08x (MaxPorts %d MaxIntrs %d MaxSlots %d)`

本次启动日志中**没有** `ports limit reached`，也**没有** `extended capability defines >maxPorts`。
这两条日志各有独立的使能位，未使能时不会输出，因此不能据此断定分支未执行。

## 4. 诊断入口：IOUSBHostFamily-debug

`IOUSBHostFamily` 的 `USBHostLoggingBootArgs` 支持（来自 27.2 反汇编）：

- `IOUSBHostFamily-debug=<int>`：整型掩码 OR 进 `_debugLoggingMask`
- `IOUSBHostFamily-debug-error=<int>`：置 bit1
- `IOUSBHostFamily-debug-assert=<int>`：置 bit31（会触发断言，勿用）
- `IOUSBHostFamily-debug-level=<int>`：`gUSBHostKernelDebugLevel`

`createPorts` 里 `testb $0x8, 0x534` 即该掩码的 bit3。打开后应能拿到：
MaxPorts 实际值、每个 `XHCI_PORT[i].PortSC`、以及 `discarding type/port/status` 的丢弃原因。

## 5. 已部署的诊断配置

- 文件：`EFI/OC/Configs/config27-usbdebug.plist`
- 内容：`config27-sbcm-103.plist` 原样复制，仅 boot-args 追加
  `IOUSBHostFamily-debug=2147483647`（0x7FFFFFFF，避开 bit31 断言位）
- sha256：`bb05412093a3fc7968bb984927bc6d7d3742482bd4447eb8ad0523dc21f3f0ce`
- 不影响默认项：NVRAM `OC-ConfigDefault` 仍为 `config27-sbcm-103`

### 回滚

删除该文件即可，其它配置与二进制均未改动：

```sh
rm /Volumes/EFI/EFI/OC/Configs/config27-usbdebug.plist
```

## 6. 重启后取证命令

```sh
# 1) 根端口分布
ioreg -p IOService -w0 -r -c AppleUSBXHCI | grep -c AppleUSB20XHCIPort
ioreg -p IOService -w0 -r -c AppleUSBXHCI | grep -c AppleUSB30XHCIPort

# 2) MaxPorts 与端口状态
log show --last 10m --info --debug \
  --predicate 'process == "kernel" AND eventMessage CONTAINS "HCSParams1"'
log show --last 10m --info --debug \
  --predicate 'process == "kernel" AND eventMessage CONTAINS "XHCI_PORT"'

# 3) 端口被丢弃的原因
log show --last 10m --info --debug \
  --predicate 'process == "kernel" AND eventMessage CONTAINS "discarding"'
```

## 7. 判读

| 观察 | 结论 | 下一步 |
|---|---|---|
| MaxPorts = 25 且出现 `discarding … port 16…25` | 端口被主动丢弃，问题在创建/定位阶段 | 针对丢弃分支做补丁 |
| MaxPorts = 15 | 驱动只拿到 15 个端口 | 改 `createPorts` 取数来源（port-count）并复验 |
| MaxPorts = 25 且完全没有 port 16+ 的任何日志 | 循环提前结束 | 在循环边界上找第二处判断 |

## 8. 上游 PR 计划（待验证后执行）

1. 证据确定后，把候选补丁写成 `Kernel > Patch` 条目，先在 `config27-usbdebug` 同级的
   实验配置里验证一轮。
2. 通过后回收为 `OcAppleKernelLib/CommonPatches.c` 的 Darwin 27 分支（新 Find/Replace
   数组 + `OcMatchDarwinVersion` 分支），保持 pre-Tahoe / Tahoe 行为不变。
3. 用 `Utilities/TestUsbPortLimit` 做离线回归（字节级、单变量），再提交 PR 到
   `acidanthera/OpenCorePkg`，PR 只含该补丁，不夹带 OCMC 改动。

## 9. 2026-11-19 实测结论（重要，推翻前文方向）

| 实验 | 配置 | 结果 | 含义 |
|---|---|---|---|
| A | `config27-sbcm-103`（quirk 0x40） | 15 USB2 / 0 SS | 基线 |
| B | 自定义 patch：`createPorts` 计数上限改 12 | **12 USB2 / 0 SS** | **`Kernel > Patch` 对 KC 内 AppleUSBXHCI 确实生效** |
| C | 自定义 patch：放行 `i <= maxPorts-1` 守卫到 64 | 15 USB2 / 0 SS | 守卫不是瓶颈；SS 组没进端口创建循环 |

结论：

1. 15 端口上限不是当前瓶颈。SS 那一组协议在 `createPorts` 里没有被遍历到，
   无论怎么放宽端口数/端口号判断，都不会出现 `AppleUSB30XHCIPort`。
2. `26.7.4` 与 `27.2.0` 的 `createPorts` 结构几乎一致（仅寄存器分配与 kalloc 名不同），
   但 `start()` 在 27 里多出约 120 条指令，协议/端口初始化路径被重写过。
3. 继续在 `XhciPortLimit` 上做功是错误方向；要么定位 27 新增的协议遍历逻辑，
   要么先确认控制器是否真的上报了 SS 协议能力。

控制器属性 `UsbHostControllerProtocolRevision = "2.0"` 由 capability 的 revision 字段生成；
若 SS 能力缺失，该值会停在 2.0，此时任何端口补丁都无效。

## 10. 进一步定位：SS 端口为什么建不出来

`createPorts` 的外层循环会遍历所有 capability（`0x2d78d54` → `*0xc70`/`*0xc80` → `jne 0x2d78590`），
对每个 `Supported Protocol`（名字 `"USB "`）按 `bPortOffset`/`bPortCount` 建端口：

```
0x2d78678: r13d = capability[8] & 0xff      ; bPortOffset
0x2d78683: ebx  = (capability[8] >> 8) & 0xff ; bPortCount
0x2d7868c: ecx  = offset + count - 1
0x2d78692: cmp  ecx, offset-1
0x2d78694: jae  skip                        ; count<=0 时跳过
0x2d786af: cmp  i, maxPorts-1 ; jbe create  ; 超界只记日志并跳过
0x2d78d3c: incl r13d; addl $0x10, r12d; incq r14; decl ebx; jne 0x2d786af
```

USB2 组：offset=1、count=15 → 端口 1–15。USB3 组：offset=17、count=9 → 端口 17–25。

关键在 `setPortLocation()`（Darwin 27.2 实测）：

```
r15d = portNumber
if (portNumber > 0xF) return 0xE00002C2        ; OC part 1 打这里 0xF→0x40
...
shift = 20
while ((locationId >> shift) & 0xF) shift -= 4  ; 找空 nibble
locationId |= (portNumber << shift)             ; 没有 &0xF 掩码（part 2 在 Tahoe+ 无落点）
```

- 端口 16 → `0x15000000`，17 → `0x15100000` … 25 → `0x15900000`
- **locationID 的 AA 字节是控制器编号（0x14），B nibble 才是端口号，共 4 bit**
- 17–25 需要 5 bit，`<<20` 后进位污染 AA 字节，产生的 locationID 不以 0x14 开头
- 这类端口在 27 上不会被注册成 `AppleUSB30XHCIPort`（26 容忍了这种重复/越界 ID，27 不再容忍）

这解释了全部实验现象：

| 现象 | 解释 |
|---|---|
| 12 口补丁生效 | 端口 1–12 的 locationID 合法，创建即注册 |
| guard 放宽到 64 无效 | 端口 17+ 建出来也拿不到合法 locationID，照样不注册 |
| 27 上永远 15 口 | 4 bit 端口号 + AA 字节固定，物理上只能表示 15 个根端口 |

## 结论：根因是 macOS 27 删除了 AppleUSBXHCISPT

**证据链**

1. 开机日志（两次启动都有）：
   `kernel: Couldn't alloc class "AppleUSBXHCISPT"`
2. `XHCI-unsupported.kext` 的 personality 表里有：
   ```
   AppleUSBXHCISPT 600   IOClass=AppleUSBXHCISPT   IOPCIPrimaryMatch=0x51ed8086 0x7ae08086   score=900
   ```
   `0x7ae0` 正是本机 Z690 PCH 的 XHCI（`8086:7AE0`）。它用 score 900 抢在通用
   `AppleUSBXHCIPCI`（无 IOPCIPrimaryMatch，score 0）之前匹配。
3. 类名在 kernel collection 里的存在情况：
   | 类 | macOS 26.7.4 | macOS 27.2.0 |
   |---|---|---|
   | `AppleUSBXHCISPT` | 10 处 | **0 处** |
   | `AppleUSBXHCILPTH` | 有 | 无 |
   | `AppleUSBXHCIWPT` | 有 | 无 |
   | `AppleUSBXHCIAR` / `TR` | 有 | 有 |
   | `AppleUSBXHCIPCI` | 有 | 有 |

   macOS 27 保留了 Thunderbolt（AR/TR）、AMD、ASMedia 的子类，**只删掉了 Intel PCH 的
   SPT/LPTH/WPT**。这与 27 不再支持 Intel 平台的方向一致。

**因果链**

```
XHCI-unsupported 用 AppleUSBXHCISPT 匹配 8086:7AE0
  → 27 里没有这个类，personality 分配失败
  → 设备回落到通用 AppleUSBXHCIPCI / AppleUSBXHCI
  → PCH 专用的 SS 端口枚举逻辑随 SPT 一起消失
  → 只按 USB2 协议组建 15 个端口，SS 一个都没有
```

**已证事实**

1. `Kernel > Patch` 对 KC 内 `AppleUSBXHCI` 生效（计数改 12 → 实际 12 口）。
2. 放宽 `i <= maxPorts-1` 守卫到 64 无效：SS 组没有产生任何端口对象。
3. 所以这是“缺驱动子类”，不是“某个比较被卡住”，内核补丁方向无解。

**可行路径**

| 方案 | 代价 | 说明 |
|---|---|---|
| macOS 26 上用 USB3 | 0 | 26.7.4 的 SPT 仍在，USB3 正常 |
| 27 上接受只有 USB2 | 0 | 15 个 USB2 口，音视频/键鼠/蓝牙不受影响 |
| 移植 SPT（写一个同名 kext 子类） | 大 | 需要从 26 的 `AppleUSBXHCIPCI` 二进制里还原 SPT 覆写的方法（`createPorts` 等），是独立驱动工程 |
| 注入 device-id 伪装成 AR/TR | 中 | 那两类是 Thunderbolt xHCI（端口模型不同），预期不可用，不建议 |

**顺带修复**：`/Volumes/EFI` 的 FAT 曾被标记 dirty（多次写配置后直接重启），已用
`fsck_msdos -y` 标记为 clean；下次重启可正常挂载。建议之后每次写完配置先
`sync && diskutil unmount` 再重启。

## 11. 2026-11-19 06:30 A 方案（伪装 device-id）实测：失败，且已证明不可能成功

配置 `config27-usbar`：给 `PciRoot(0x0)/Pci(0x14,0x0)` 注入 `device-id = 0x15B6`，
目标是匹配 27 中仍存在的 `AppleUSBXHCIAR`（score 5000）。

| 项 | 结果 |
|---|---|
| `device-id` 注入 | 成功，节点上为 `<b6150000>` |
| 绑定驱动 | 仍是 `AppleUSBXHCIPCI`（class match，score 0） |
| 端口 | 15 × `AppleUSB20XHCIPort`，0 × `AppleUSB30XHCIPort` |

为什么必然失败（静态证据）：

1. `AppleUSBXHCIAR` / `AppleUSBXHCITR` / `AppleUSBXHCIFL1100` 在 26 和 27 里
   **都没有实现** `getProtocolCapabilityOverride`，也没有 `Usb3Protocol*` 常量。
   它们只改电源状态、命令环、请求池，与端口枚举无关。
2. 27 全库：`getProtocolCapabilityOverride` 覆写 0 处；`Usb3Protocol` 符号 0 处；
   描述符字节 `02 08 00 03 55 53 42 20 11 0a` 0 处。
3. 26 全库：`AppleUSBXHCISPT` 43 个符号 + `_Usb3ProtocolForSPT` / `_Usb3ProtocolForSPTLP` 两份常量。

## 12. 根因闭环

控制器属性 `UsbHostControllerProtocolRevision = "2.0"`：这颗 PCH 的 xHCI
扩展能力链里**只有 USB 2.0 一个 Supported Protocol 组**（offset 1，count 15）。
USB3 组是 SPT 用一段硬编码描述符补出来的：

```
02 08 00 03   dword0: capID=2, next=8(dwords), major=3, minor=0
55 53 42 20   "USB "
11 0a 00 10   dword2: portOffset=17, portCount=10, wProtocolDefined=0x1000
00 00 00 00   dword3: PSI = 0
```

27 删除 SPT、删除两份常量、删除全部协议覆写 → 通用 `AppleUSBXHCIPCI`
只能按硬件真实上报建 15 个 USB2 口。既不是 15 口限制，也不是 locationID 位宽问题。

## 13. B 方案（移植 SPT）的可行路径与已核对 ABI

27 的 `AppleUSBXHCIPCI` 虚表（`__ZTV15AppleUSBXHCIPCI` = `0xffffff8002dc70a8`，
实例 vptr = `__ZTV+0x10`）逐槽解码确认：

| 运行期槽位 | 目标 | 说明 |
|---|---|---|
| `+0xc70` | `AppleUSBXHCI::getNextCapability` (0x2d7192a) | 扩展能力链步进，内部走 `regRead32` |
| `+0xc80` | `AppleUSBXHCI::getNextProtocolCapability` (0x2d719f8) | createPorts 用它取协议组 |
| `+0xce0` | `AppleUSBXHCIPCI::regRead32` (0x2db4dba) | 读 MMIO |

`createPorts` 入口：`rsi = this->0x650; call *0xc80`，即从第一个扩展能力开始找协议组。

最小实现 = 一个不绑定设备的 kext：

1. `waitForMatchingService("AppleUSBXHCIPCI")` 拿实例；
2. 复制整张虚表到可写内存，替换 `+0xc70`、`+0xc80`，把实例首 8 字节指向副本；
3. `+0xc80` 包装：`cur == &fake` → NULL；否则调原函数，返回 NULL 时返回 `&fake`；
4. `+0xc70` 包装：`cur == &fake` → NULL（否则 `regRead32` 会按 MMIO 偏移读到非法地址）；
5. `stop()` / `start()` 让控制器重建端口。

风险：`stop/start` 期间 USB 键鼠短暂失效；守卫写错会 panic。
回退：重启在 OCMC 菜单选 `config27-sbcm-103`，删掉 kext 即可复原。

## 14. B 方案实现：AppleUSBXHCISPTCompat.kext（2026-11-19 06:50 部署）

工程：`usb27-work/xhci-spt-compat/`（`src/AppleUSBXHCISPTCompat.cpp` + `build.sh`，CLT SDK 直接编译，
无 Xcode/KDK）。产物 sha256 `79a68024…`，已装入 `EFI/OC/Kexts/`，配置 `config27-spt.plist`
（`MinKernel 27.0.0`，26 不受影响）。

工作方式：

1. 作为 `IOResources` 服务启动，起一个内核线程等 `AppleUSBXHCIPCI` 出现；
2. 复制该实例的 vtable（0xd50 字节）到可写内存，替换 `+0xc70`、`+0xc80` 两槽；
3. 把实例首 8 字节（vptr）指向副本；
4. `stop()` + `start()` 让控制器重建端口。

两个包装的语义（对应 `cap = getNextProtocolCapability(getNextCapability(cap))`）：

| 包装 | 行为 |
|---|---|
| `+0xc70` | 入参是假描述符 → 返回 NULL；否则透传原函数 |
| `+0xc80` | 入参是假描述符 → NULL；原函数返回非空则透传；仅在**调用方传 NULL**（真实链走完）且未注入过时返回假描述符 |

注入的假描述符与 26 的 `_Usb3ProtocolForSPT` 逐字节一致（capID=2 / rev 3.0 / offset 17 / count 10 / PSI=0）。

自检属性（`ioreg -l -n AppleUSBXHCISPTCompat`）：
`State`（waiting/restarted/restart-failed/…）、`USB2Ports`、`USB3Ports`、
`OriginalNextCapability`、`OriginalNextProtocolCapability`。

回退：重启在 OCMC 菜单选 `config27-sbcm-103`（不加载该 kext），或删除 `EFI/OC/Kexts/AppleUSBXHCISPTCompat.kext`。
