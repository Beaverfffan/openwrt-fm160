# image-files/ — H69K 整机镜像的 rootfs overlay

构建整机镜像时，把本目录内容拷进 OpenWrt 源码树的 `files/`（与构建树
`files/` 合并）后再 `make`：

```sh
cp -r image-files/* /mnt/data4t/istoreos-h69k/src/files/
```

## etc/uci-defaults/99-beaver-wifi

首启（first boot）时执行一次（执行后自删）：所有射频启用、双频同名开放
SSID `istoreos-beaverfffan`、无密码。

⚠️ 两个已踩过的坑：
- config_generate 生成的是**命名** wifi-device section（`radio0`…），
  匹配 section 类型后缀（`=wifi-device`），不要猜 `wifi-deviceN` 名字。
- uci-defaults 只在**清配置刷机**（`sysupgrade -n`）后执行；
  保留配置升级不会重跑，改这里对新刷机才生效。
