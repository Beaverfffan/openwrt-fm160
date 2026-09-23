# at-daemon（vendored）

本目录是 **原样复用** 的上游组件，不是本项目原创代码。

| 项 | 值 |
|---|---|
| 来源 | `https://github.com/FUjr/QModem` → `application/ubus_at_daemon/` |
| 取用版本 | `main` @ `86102c2a6f62`（2026-09-11 的 tip） |
| 上游许可证 | MPL-2.0 **+ 附加条款「禁止商业使用」** |
| 本地改动 | 仅 3 处，见下 |
| 本地记录 | 上游文件原样保留其版权头 |
| 一致性 | `src/` 10 个文件 + `files/` 2 个文件，**逐个用 git blob sha1 校验与上游逐字节一致**（哈希表见 `NOTICE.md`） |

## 为什么复用

它已经把「串口独占 + 请求队列 + 行事件分流」这套难点解决了，而这正是本项目最大的风险点（高通 AT 脆弱）。它自带 `tests/test_port_concurrency.c`、`test_reader_flood.c`、`test_line_event_queue.c`，可信度高于我们现写一遍。

## 我们用到它的哪些能力

| 能力 | ubus 接口 | 本项目用途 |
|---|---|---|
| 串行发送 AT | `sendat{at_port,at_cmd,timeout,end_flag,raw_at_content,sendonly}` | `fm160d` 唯一的出网口 |
| 打开/关闭端口 | `open` / `close` / `list` | 探测端口时按需触发（sendat 会自动 open） |
| 端口租约（带 TTL） | `lease_acquire` / `lease_renew` / `lease_release` | 声明「本机 FM160 的 AT 口归 fm160d」 |
| 按前缀订阅 URC | `urc_register{port,owner,urc_id,prefix}` | 订阅 `+CMTI` 等 |
| **逐行全量推送** | `ubus event qmodem.at.line` | `fm160d` 被动收行，不轮询 |
| **URC 事件推送** | `ubus event qmodem.at.urc` | 事件驱动，减少 AT 次数 |
| **correlation 判定** | 事件字段 `correlation` | 区分「命令响应」与「真 URC」——轮询最容易出错的地方 |
| 溢出检测 | 事件字段 `drop_count` | 检测事件队列丢行 |
| 端口掉线重连 | 内部 monitor 线程 | 拔插模块自愈 |

## 本地改动（仅此三处）

1. **移除 feed 依赖**：上游 `Makefile` 里 `include ../../version.mk` 指向 QModem feed 根目录，改为 `include ./version.mk`，并在本目录新增 `version.mk`。
2. **补许可证字段**：加 `PKG_LICENSE:=MPL-2.0` 与 `PKG_MAINTAINER`，让 `make menuconfig` 能正确显示归属。
3. **声明 `conffiles`**：`/etc/config/ubus-at-daemon`。不声明的话 `opkg upgrade` 会直接覆盖设备上改过的配置。

`src/` 与 `files/` 与上游逐字节一致（blob 哈希见 `NOTICE.md`）。

> ⚠️ **行尾必须是 LF。** `files/etc/init.d/ubus-at-daemon` 曾经是 CRLF（386 → 411 字节，每行多一个 `\r`）。这种文件能编译、能打包、能安装，然后在设备上**静默不启动**——内核把解释器读成 `"/bin/sh /etc/rc.common\r"`，那个路径不存在。已修正为 LF（因此现在与上游逐字节一致），并加了 `tools/cccheck/eolcheck.py` 防回归。
>
> ⚠️ 构建时唯一的警告来自上游 `src/main.c:3`：它无条件 `#define ARRAY_SIZE`，而 `libubox/utils.h` 用 `#ifndef` 守卫自己的同名宏。两个定义**逐字相同**，所以只是噪声。**有意不修**——改了就不再是逐字节的 vendor 副本，`NOTICE.md` 的溯源也就失效了，代价换不来收益。

## 两阶段命令（M3 短信）：**无需修改上游**

早先这个文件写着"`sendat` 是一次性模型，必须追加 `prompt`/`payload_hex` 字段"。
**该判断已被上游源码与真机双双推翻**，记录在此以免重犯：

`at_handler.c:166-179`（`is_raw` 分支）把 `raw_at_content` 当作**十六进制字符串**，
解码成裸字节写出，**不追加 `\r`**，并且**照常等待 `end_flag`**：

```c
if (is_raw) {
    size_t hex_len = strlen(cmd);
    if (hex_len == 0 || hex_len % 2 != 0) { ... return -1; }
    send_data = hex_to_string(cmd);
    send_len  = hex_len / 2;
} else {
    send_len = strlen(cmd) + strlen(AT_CMD_TERMINATOR);
    ...
}
```

同时 `sendonly=1` 走 `send_at_command_only()`，彻底不等响应。

⇒ `AT+CMGS=<len>` / `AT+CMGW=<len>` 的两阶段可以直接表达为两次 `sendat`：

```sh
# 1) 等提示符（配合 lease_acquire 独占端口，避免别的属主插队）
sendat { at_cmd:"AT+CMGS=27", end_flag:">", timeout:5000 }
# 2) 送 PDU + Ctrl-Z(0x1A)，等 OK
sendat { raw_at_content:"<PDU hex>1A", end_flag:"OK", timeout:60000 }
```

前提已实测成立：**`sendat` 结束后端口保持打开**（`ubus call at-daemon list`
显示 `is_open:1`），两次调用之间不会断链。

真正要在 M3 补的是 **`fm160d` 侧的调用序列、租约管理与 PDU 编解码**，不是 daemon。

## 与载机上那份旧版的区别（重要）

2026-09-18 在 H69K/iStoreOS 上实测，**载机自带的 `ubus-at-daemon`（May 8 构建）
只有 4 个方法**：`open / sendat / list / close`，其二进制里没有
`ubus_send_event`、`qmodem.at.line`、`qmodem.at.urc`、`correlation`、`drop_count`。

那是 **QModem 的旧版**（脚本回调模型）。本目录复用的是上游 `main` 的**新版**，
有上表 11 个方法与完整行事件。**在载机上做事件联调前，必须先确认跑的是哪一份**，
否则会把"版本差异"误判成"代码 bug"。

## 构建实测

2026-09-18 在 iStoreOS `istoreos-24.10` @ `b1bb87394452` 树里真编通过：

```
ubus-at-daemon_2026.09.18-vendored-r1_aarch64_generic.ipk   16965 B   rc=0
Depends: libc, libubus20250102, libubox20240329, libblobmsg-json20240329, libjson-c5
```

- ELF64 / AArch64，`DT_NEEDED` 里的库版本与 H69K 上已装的**完全一致**。
- `control.tar.gz` 含 `conffiles`（`/etc/config/ubus-at-daemon`），升级时配置受保护。
- 该 ipk 的 `Depends` 与 QModem 那份一致，只多一个 `ubus-at-daemon` 自身的包名归属——
  也就是说装到 H69K 上会**把 QModem 的 3.0.2-r2 换成本地这份**（版本号更大），
  这正是预期的：`fm160d` 需要 `urc_register` 等新方法。

**尚未在设备上装过、跑过。** 上面是编译与打包层面的验证，不含运行期行为。
