#!/bin/sh
# FM160 原厂 MBIM 拨号拆除（文档步骤 6）
DEV=/dev/cdc-wdm0
mbimcli -p -d "$DEV" --disconnect="0" || true
ip addr flush dev wwan0 2>/dev/null || true
ip -6 addr flush dev wwan0 scope global 2>/dev/null || true
echo "mbim 拨号已断开"
