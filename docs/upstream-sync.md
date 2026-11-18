# 同步上游 OpenCorePkg 更新

OCMC 不是 `Acidanthera/OpenCorePkg` 的 GitHub fork,而是一个独立的发行仓库。这不需要改:BSD-3-Clause 从不要求派生作品必须 fork,fork 只是 GitHub 的社交机制。真正的同步能力来自**本地 git 里的 upstream remote**,而不是 GitHub 的 "Sync fork" 按钮。

## 仓库结构

```
04-EFI与引导/
├── OC-多配置/                     # 发行仓库(push 到 GeekAudio/OpenCore-MultiConfig)
│   ├── patches/ocmc-1.0.8.patch   # 完整改动(8 文件 / ~2900 行)
│   ├── src/                       # 三个新增源文件的副本,便于阅读
│   ├── out/{DEBUG,RELEASE}/        # 构建产物
│   └── tools/sync-upstream.sh     # 升级脚本
└── 源码/
    ├── ocmc-git/                  # ★ 带上游历史的源码仓库(不是发行仓库)
    │   ├── tag upstream-1.0.8     # 纯净 OpenCorePkg 1.0.8
    │   ├── tag ocmc-1.0.8         # 上游 + OCMC 改动
    │   └── remote upstream        # → github.com/acidanthera/OpenCorePkg
    ├── OpenCorePkg-1.0.8/         # 实际编译树(含 UDK / Build / openssl)
    └── toolchain/bin/             # nasm / mtoc / iasl
```

`源码/ocmc-git` 只跟踪 OpenCorePkg 源码,**不含** `UDK/`(375 MB 的 EDK2 环境)和 `Build/`,所以只有 78 MB。UDK 可跨版本复用,升级时复制过去即可。

## 改动清单(8 个文件)

| 文件 | 类型 | 说明 |
|---|---|---|
| `Library/OcMainLib/OpenCoreConfigMenu.c` | 新增 | 菜单 + boot-args 编辑器主体 |
| `Library/OcMainLib/OpenCoreConfigGfx.c` | 新增 | 图形渲染 |
| `Library/OcMainLib/OpenCoreConfigFont.h` | 新增 | 内嵌字体 |
| `OpenCorePkg-mc.dsc` | 新增 | 只含 OpenCore 模块的裁剪构建描述 |
| `Library/OcMainLib/OpenCoreMisc.c` | 修改 | 读配置前插入 `OcConfigMenuSelect` |
| `Library/OcMainLib/OcMainLib.inf` | 修改 | 注册三个新源文件 |
| `Include/Acidanthera/Library/OcMainLib.h` | 修改 | 导出接口声明 |
| `Application/OpenCore/OpenCore.c` | 修改 | 调用点 |

只有这 8 个文件需要跟着上游走。`build_oc.tool`(改成读本地 `efibuild.sh`)和 `Binaries/`、`Utilities/ocvalidate/ocvalidate` 属于本机构建环境,不参与同步。

## 升级流程

设上游发布了 `1.1.0`:

### 1. 重放改动

```sh
cd /Users/geek/Documents/Macintosh/04-EFI与引导/OC-多配置
./tools/sync-upstream.sh 1.1.0
```

脚本会:拉取上游 tag → 打 `upstream-1.1.0` → 从它建 `sync-1.1.0` 分支 → cherry-pick 最新的 `ocmc-*` 提交 → 成功则打 `ocmc-1.1.0` 并生成 `patches/ocmc-1.1.0.patch`。

### 2. 有冲突时

脚本会停下并列出冲突文件。手工解决后:

```sh
cd ../../源码/ocmc-git        # 脚本会打印绝对路径
git status
# 编辑冲突文件
git add -A && git cherry-pick --continue
../../OC-多配置/tools/sync-upstream.sh 1.1.0   # 重新运行以完成打 tag 和出补丁
```

冲突最可能出现在 `OcMainLib.h`、`OpenCoreMisc.c`、`OpenCore.c`——前两个上游改动频繁,第三个只有 5 行调用。

### 3. 建编译树

```sh
cd ../../源码
mkdir -p OpenCorePkg-1.1.0
git -C ocmc-git archive ocmc-1.1.0 | tar -x -C OpenCorePkg-1.1.0
cp -R OpenCorePkg-1.0.8/UDK OpenCorePkg-1.1.0/
```

UDK 里的 `OpenCorePkg` 是指向父目录的软链,复制后自动指向新树,无需重建。

### 4. 编译

```sh
export PATH="/Users/geek/Documents/Macintosh/04-EFI与引导/源码/toolchain/bin:\
/Users/geek/Documents/Macintosh/04-EFI与引导/源码/OpenCorePkg-1.1.0/UDK/BaseTools/BinWrappers/PosixLike:$PATH"
SRC=/Users/geek/Documents/Macintosh/04-EFI与引导/源码/OpenCorePkg-1.1.0
export WORKSPACE="$SRC/UDK" EDK_TOOLS_PATH="$SRC/UDK/BaseTools"
export PACKAGES_PATH="$SRC/UDK:$SRC" CONF_PATH="$SRC/UDK/Conf"
cd "$SRC/UDK" && . ./edksetup.sh
build -a X64 -b DEBUG   -t XCODE5 -p OpenCorePkg/OpenCorePkg.dsc -m OpenCorePkg/Application/OpenCore/OpenCore.inf -n 8
build -a X64 -b RELEASE -t XCODE5 -p OpenCorePkg/OpenCorePkg.dsc -m OpenCorePkg/Application/OpenCore/OpenCore.inf -n 8
```

`build` 不在 PATH 时,补上 `BaseTools/BinWrappers/PosixLike`(上面已含)。

### 5. 验证后发布

产物拷到 `out/{DEBUG,RELEASE}/`,更新 `src/` 副本与 `README`,提交、push、打 `v0.5.x` tag、`gh release create` 上传双 zip。

## 一次性验证补丁可复现

```sh
mkdir -p /tmp/verify && tar xzf ../../源码/oc108.tar.gz -C /tmp/verify --strip-components=1
cd /tmp/verify && git apply ../../OC-多配置/patches/ocmc-1.0.8.patch
```

应用后应与 `源码/OpenCorePkg-1.0.8/` 的源码部分逐字节一致(已于 2026-11-19 验证通过)。

## 为什么不直接 fork

- 本仓库定位是**发行仓库**:补丁 + 产物 + 文档,不是 OpenCorePkg 的分支快照。
- fork 会把上千个上游文件搬进仓库,补丁淹没其中;而且 GitHub 的 sync 只能快进合并,处理不了我们对 `OcMainLib.h` 的 128 行改动。
- 需要"上游基线"时,`源码/ocmc-git` 里的 `upstream-*` tag 就是,随时可以 `git diff upstream-1.1.0..ocmc-1.1.0` 看差异。

## 许可证

改动与上游同为 BSD-3-Clause。`LICENSE` 保留 Acidanthera 各版权人的原始声明,详见仓库根目录 `LICENSE`。
