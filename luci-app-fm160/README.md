# luci-app-fm160 — FM160 LuCI 应用

`fm160d` 的图形前端，单一 ubus 对象 `fm160` 是全部后端通信面。
许可证：GPL-2.0-or-later。翻译：zh_Hans（含完整 i18n 门禁，见 `../tools/`）。

## 页面

| 页面 | 内容 |
|---|---|
| 模组概览 | 身份/ICCID/SIM、注网、信号、服务小区、流量；「无线（AT 交叉验证）」区块常显——运营商（COPS）、PCC 频段/CA、速率（GTSTATIS）、CQI/RANK/MCS、QCI，30s 自动抓取 + 手动刷新，拿不到数据显示 `-` 不消失 |
| 信号质量 | 实时信号、邻区（GTCCINFO?）、手动/自动 30s 抓取 |
| 小区与锁定 | 频段锁、小区锁（LTE/NR、PCI/频点）、CA 视图 |
| 拨号 | 三模式（ECM/QMAP/MBIM）统一控制面：APN、v4/v6 开关、连接/断开、keepalive（间隔/重载/放弃轮数可配）、拨号自启 |
| USB 模式 | GTUSBMODE 配置对切换、白名单与回滚状态机展示 |
| GNSS | 引擎开关、定位状态、多星座 SNR |
| MWAN3 | 有线和 FM160 接口自动注册进 mwan3：优先级（metric 分层=故障转移，同层权重=分流）、双栈探测目标、探测节奏；hotplug 自动重注册，只管理带 `fm160_managed` 标记的段 |
| 短信 | 收发、PDU 长短信、存储管理 |
| 操作日志 | 语义化日志 tail（所有页面操作都记录） |
| AT 控制台 | 原始 AT 调试 |

## 权限模型

`root/usr/share/rpcd/acl.d/luci-app-fm160.json` 白名单：ubus `fm160` 读写
两组、file exec（`fm160-dial-ctl`/`fm160-oplog`/`fm160-mwan3-sync`/`mwan3`/
prefix 脚本）、uci `fm160` 读写 + `mwan3` 只读。前端不持有任何 root 能力之外的通道。

## 依赖

`+luci-base +fm160d +fibocom-dial`。mwan3 为可选依赖——没装时注册配置照常
写入但提示安装，不影响其余功能。

## 翻译工作流

`po/zh_Hans/fm160.po` 由 `../test`-侧脚本从 JS/menu/daemon 标签提取同步，
CI 门禁保证：提取基逐文件下限、跨目录碰撞（msgctxt 只许用在真撞名上）、
真实 po2lmo 往返、lmo 键哈希与 cbi.js `sfh()` 一致。新增字符串先进 ZH 字典再同步。
