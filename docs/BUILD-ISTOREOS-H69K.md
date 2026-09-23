# 在 iStoreOS 24.10 上为 H69K 构建含 FM160 套件的完整系统镜像

本文记录**怎么编、为什么这么编、以及哪些坑会让镜像"看起来正常但其实是错的"**。
面向两台机器：Windows 工作站（无 C 编译器、无 sshpass）与 Ubuntu 编译机。

结论先行：

* 目标 profile = **`hinlink_opc-h6xk`**（H66K / H68K / H69K 三合一），target = `rockchip/armv8`。
* **官方 24.10-config.seed 不能直接用**：它是 `CONFIG_TARGET_MULTI_PROFILE=y` + `TARGET_ALL_PROFILES=y`，
  直接套用会编**全部 53 个 rockchip 机型**。
* 镜像里的 FM160 套件来自 `package/{fm160d,ubus-at-daemon,luci-app-fm160}`，是我们自己的源码树**拷贝**进去的，
  不是 feed。改了源码必须重新拷进树，否则编出来的是旧代码。
* **内核 `option.c` 决定 USB 模式能不能切**，不是厂商补丁表。详见 §5。

---

## 1. 为什么不能直接用官方 seed

`fw.koolcenter.com/iStoreOS/h6xk/` 下的 `24.10-config.seed` 是官方构建机的完整配置。
它同时含：

```
CONFIG_TARGET_MULTI_PROFILE=y
CONFIG_TARGET_ALL_PROFILES=y          ← 关键
```

而生成的 `tmp/.config-target.in` 里每个机型都是：

```
menuconfig TARGET_DEVICE_rockchip_armv8_DEVICE_hinlink_opc-h6xk
	default y if TARGET_ALL_PROFILES
```

⇒ `ALL_PROFILES=y` 时 53 个设备**全部 default y**。必须显式关掉它，并把其余 52 个设备写成
`# ... is not set`（只关 `ALL_PROFILES` 不够，因为 seed 里它们本来就带 `=y`）。

**注意 `CONFIG_TARGET_ALL_PROFILES` 在现代树里根本不存在**（`grep -rn TARGET_ALL_PROFILES` 在
`include/`、`target/` 全无命中）——它是配置里的**遗留死符号**。所以机型选择只由
`MULTI_PROFILE` + 每设备布尔决定；但 `tmp/.config-target.in` 里的 `default y if TARGET_ALL_PROFILES`
仍然生效，因为那是生成出来的 kconfig。

变换脚本：`_tools/istoreos-h69k/mkconfig.py <seed> <out>`。它**原位替换**而不是追加键
（追加会产生重复键，kconfig 取最后一个，行为诡异）。

## 2. feed 集：14 个，不是 6 个

`feeds.conf.default` 只有 6 个。官方 `24.10-feeds.conf` 追加 8 个
（`h69k_oled`、`lcdsimple`、`third_party`、`diskman`、`oaf`、`linkease_nas`、
`linkease_nas_luci`、`jjm2473_apps`）。

**这不是可选装饰**：seed 选中了 `lcdsimple`、`luci-app-oled`、`luci-app-linkease`、
`luci-app-diskman`、`luci-app-oaf`、`luci-app-quickstart`、`luci-app-store`
（`luci-app-istorex` 是 `=m`）。少了那些 feed，`defconfig` 会**静默丢弃**这些包，
镜像就没有 H69K 的屏和 iStore UI。

⚠️ 官方 `24.10-feeds.buildinfo` 把 14 个 feed 全钉在 SHA 上，但那是 **24.10.8（2026073111）** 那次构建的
快照。本树在 `istoreos-24.10` 分支尖端，比它新，所以**跟随分支尖端**（`feeds.conf.default` 声明的就是分支）
才是自洽的组合。实测漂移：`packages`/`telephony`/`oaf`/`jjm2473_apps` 与钉版相同，
其余 7 个已前进（如 `lcdsimple` `16723f9` → `b96c291`）。

⚠️ `scripts/feeds` 支持 `^<sha>` 钉版，**但已有克隆且带 hash 时会跳过更新**（"don't update the feed"）。
要真钉版必须先清 `feeds/`。

## 3. 构建阶段与耗时（7 GB 内存的机器）

| 阶段 | 说明 |
|---|---|
| `make -j8 download` | ⚠️ **遍历整棵树**，未选中的包也下（日志里会出现 `containerd`、`coova-chilli`） |
| `make -j8` | `tools/compile` → `toolchain/compile` → `target/compile` → `package/compile` → `target/install` |
| `tools/llvm-bpf` | **最大单块**：LLVM 3807 个目标，约 20 分钟 + 一小时级 |
| 产物 | `bin/targets/rockchip/armv8/*h6xk*.img.gz` |

* 内存 7 GB 必须配 **swap**（39 GB 实测够）；`-j8` 是这台机的合理上限，别上 `-j16`。
* `CONFIG_CCACHE=y` 是开着的，重编非常快。
* **`make download` 里无关包（别的机型的 u-boot 变体）失败是无害的** —— 它属于别的设备，
  主构建根本不会碰。但别把"下载阶段 rc≠0"当成失败，`09-full-image-build.sh` 会记下并继续。

## 4. ⚠️ 绝不在树里同时跑两个 make

**实测事故**：一次手动 `make -j8 V=s`（tmux）与一次 `make -j8 download` 同时在同一棵树里跑。
两者都会写 `tmp/`，会互相破坏。更隐蔽的是：**其中一个在扫描包列表（`prepare-tmpinfo`）时，
另一个正在改 `feeds/`**——得到的包列表"看起来正常但是缺包"，
`make` 会一路成功并**产出一份少了几个包的镜像**。

启动任何构建前先查：

```sh
pgrep -fc '^make -j[0-9]+ V=s$'      # 必须 <= 1
```

只有 `run-full-build.sh` 的 pid 文件在做互斥，它挡不住外来 make。

**补救成本很低**：`tools/` 的进度靠 ninja 增量状态存在磁盘上，停掉 make 不丢；重启只是重扫一遍依赖。

## 5. ★ 内核 `option.c` 决定哪些 USB 模式可切（比厂商补丁表重要）

`drivers/usb/serial/option.c` **先按 VID:PID 匹配**，匹配不上就不会 probe 任何接口。
所以某个 PID 没有条目 ⇒ 整个设备不会出现 `ttyUSB*`，**与 USB 描述符是否健康无关**。

厂商文档（拨号集成指导 §2.3）给了 `option_blacklist_info` / `RSVD(n)` 的推荐表，
但**真正生效的是内核里已经有什么**。实测 6.6.127 源码（构建机 toolchain 里解出的那份），
2cb7（Fibocom）族只有这些条目：

| PID | mode | 内核条目 | 结论 |
|---|---|---|---|
| 0x0104 | 17 / 32 | `USB_DEVICE` + `RSVD(4)\|RSVD(5)` | ✓ 与厂商建议逐位一致 |
| 0x0105 | 18 / 33 | `USB_DEVICE_INTERFACE_CLASS(…,0xff)` + `RSVD(6)` | ✓ 4/5 是 ECM 类，靠 class 已挡住 |
| 0x0106 | 19 | 有，无 driver_info | ✓（本项目不提供该模式） |
| 0x010A | 23 | 有 | ✓（不提供） |
| 0x010B | 24 | 两条 `AND_INTERFACE_INFO`（Diag / AT） | ✓ 无 AT，已拉黑 |
| 0x0111 | 30 | 有，无 driver_info | ✓ |
| 0x0107 / 0x0108 / 0x0109 / 0x010C / 0x010D / 0x010E / 0x010F / **0x0110** | 20/21/22/25/26/27/28/**29** | **无条目** | ✗ |

清零验证：

```sh
F=<build_dir>/…/linux-6.6.127/drivers/usb/serial/option.c
for p in 0104 0105 0106 0107 0108 0109 010a 010b 010c 010d 010e 010f 0110 0111; do
    printf '0x%s %s\n' "$p" "$(grep -c "0x2cb7, 0x$p" $F)"; done
grep -rn '0x0110\|0x010f' target/linux/     # 树内没有任何补丁补上它们
```

**结论**：

* 本项目允许切换的 **17 / 18 / 32 / 33 / 30 全部有内核条目**，AT 口照常落在 `ttyUSB2`。
  厂商补丁**不必要**——这也解释了为什么 32 模式（0x0104）在 H69K 真机上 AT 正常。
* ⚠️ **mode 29（0x0110）必须按禁止处理**：模块侧有 AT 口，主机侧没有。切过去 = 永久失去管理通道，
  且没有系统侧复位路径。已写入前端 `USB_MODE_NO_KERNEL_DRIVER`，MBIM 候选只剩 30。
* mode 21（0x0108）同理；它的 AT 口只会出现在 `ttyUSB1`。
* 若将来要 21 / 29，每个 PID 一行补丁：
  `{ USB_DEVICE_INTERFACE_CLASS(0x2cb7, 0x0110, 0xff), .driver_info = RSVD(0) | RSVD(1) },`
  但必须重新真机验证。

## 6. 改了包源码之后怎么办

树里的包是**拷贝**（`cp -r` 自源码树或 `/mnt/data4t/repos/luci-app-fm160`）。
`build_dir/target-*/<pkg>` 里还留着上次的副本与 stamp，**只改 `htdocs/*.js` 不会触发重新 prepare**，
编出来还是旧代码。

顺序：**等当前构建结束** → 更新源码树对应提交 → 重新拷贝进 `package/<pkg>` → 清掉
`build_dir/target-*/<pkg>` 与相关 stamp → 重编包 → 重跑 `make` 重组镜像（rootfs 组装，分钟级）。

⚠️ 绝不在 make 运行期间改树。

## 7. 产物验证（不要只看"构建成功"）

`_tools/istoreos-h69k/11-verify-image.sh` 检查：

1. **只有 h6xk 一个机型的镜像**（多一个就说明 profile 没选对）。
2. manifest 里有 `fm160d` / `ubus-at-daemon` / `luci-app-fm160`。
3. **拨号内核模块齐全**（`kmod-usb-serial-option` 等）—— 缺了它 AT 口根本不会出现，
   而下游所有症状都会看起来像我们自己的代码有 bug。
4. 解开 `.ipk`，**逐个 payload 文件**去 squashfs 里核对（manifest 只能证明"包管理器以为装上了"）。
5. 关键文件在位：`/etc/init.d/ubus-at-daemon`、`/etc/config/fm160`、LuCI 的 menu/ACL json。
6. `unsquashfs -cat` 读 `/etc/openwrt_release` 确认版本。

## 8. 官方配置来源与陷阱

* 官方目录同时挂 24.10.x 与 25.12.5。**裸名文件（`commit.buildinfo`、`version.index.v2`）属于最新版**
  = 25.12.5（`ac468db`，2026-09-10），**不是** 24.10。`24.10-*` 前缀的才是 24.10（`24.10.8`，2026073111）。
* 没有 `istoreos-24.10.8` 分支，发布分支只到 `istoreos-24.10.4`；`istoreos-24.10` 分支尖端
  领先设备当前提交 124 个提交，其中 17 个动 `rockchip`，内核 **6.6.127 → 6.6.144**。
* `CONFIG_IB_STANDALONE` 与 `CONFIG_PACKAGE_linkmount` 会被 `defconfig` 丢掉（前者是 ImageBuilder 开关，
  后者在尖端已移除）——**与拨号无关**，属预期。

## 9. ⚠️ 改包源码后 `make package/X/compile` 可能什么都不做

这是本项目踩过最贵的一个坑：**改了 `fm160d/src/sched.c`，`make` 返回 0，镜像里却是旧二进制**。

机制（`include/package.mk` 第 103–160 行）：

* `Build/Prepare` 把 `./src/*` **拷贝**进 `$(PKG_BUILD_DIR)`；
* 这一步由 stamp `.prepared_<md5(find ${CURDIR} 带 mtime)>_<confvar>` 把守；
* 只有 `CONFIG_AUTOREBUILD=y` 时才启用逐文件跟踪（`rdep` → `.dep_files`）。

后果：`make` 打印一片祥和，`bin/packages/.../*.ipk` 里装的还是**上一版**的二进制。
这与"修复没生效"在设备上完全无法区分，而每次误判的代价是一整个刷机/部署周期。

**判据**（`_tools/istoreos-h69k/06-rebuild-packages.sh` 已内置）：
比较 `$(PKG_BUILD_DIR)/*.c,*.h` 与 `package/<name>/**/*.c,*.h` 的内容指纹（md5），
一致才算真的编进去了；不一致就删掉 build dir 重来。该脚本每轮打印
`build <fp> -> <fp> (package <fp>)`，三个值必须相等。

修法：`FORCE=1 sh 06-rebuild-packages.sh`（删 build dir 后重编）。
**不要**用 `make package/X/clean`——在本树里它可能连带触发内核整核重编。

## 10. ★ 日常迭代不要刷整机：只升三个包

整机 `sysupgrade` 的代价是**丢包**。实测：设备原有 **1117** 个包，刷入自制镜像后剩 **908**，
**216 个包消失**（`passwall` / `xray-core` / `tailscale` / `adguardhome` / `qmodem` / `openclash` …）。
`/etc/config` 里的**配置**活了下来（`etc/ssrplus/*`、`etc/xray/`、`etc/tailscale/tailscaled.state`、
`usr/share/passwall/rules/proxy_host` 等仍在），但**二进制没了**，那些页面就是坏的。

板子已经在跑自制固件之后，迭代 fm160d 只需要三个 `.ipk`：

```sh
sh _tools/istoreos-h69k/17-upgrade-packages.sh
```

它做的事：build machine → 本机 → 设备，**三跳都校验 sha256**；在设备上先备份
`/tmp/fm160-rollback/`（含 squashfs 里的旧二进制，可原地回滚）；`opkg install --force-reinstall`
三个包；重启两个 daemon；最后断言 `/usr/sbin/fm160d` 的 sha256 == `.ipk` 里那个 payload 的 sha256。

两个必须知道的细节：

* **覆盖运行中的二进制不会改变已在运行的进程**（旧 inode 还在）。不重启 = 新代码不生效，
  而所有表面检查都会显示"没修好"。所以脚本第 6 步的重启不是可选项。
* **`.ipk` 里的二进制是 strip 过的，构建目录里的不是**，两者 sha256 天然不同
  （本包 81096 → 66377 B）。要比就得跟"从 `.ipk` 里解出来的那份"比，不能跟构建目录比。
* 设备的 busybox **没有 `base64` applet**，也没有 `timeout`。往设备推二进制只能用 `scp`；
  想给前台运行的进程设时限只能用「后台启动 + sleep + kill + wait」。

