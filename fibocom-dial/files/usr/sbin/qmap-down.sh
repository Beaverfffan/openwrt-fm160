#!/bin/sh
# FM160 原厂 QMAP 拨号拆除：按 pid 停 fibocom-dial，再停 fibo_qmimsg_server
pids=$(pgrep -f 'fibocom-dial' || true)
[ -n "$pids" ] && kill $pids 2>/dev/null
sleep 2
/usr/bin/fibocom-dial -k $(pgrep -x fibocom-dial 2>/dev/null || echo 0) 2>/dev/null || true
pkill -x fibocom-dial 2>/dev/null || true
sleep 1
if pidof fibo_qmimsg_server >/dev/null 2>&1; then
	pkill -x fibo_qmimsg_server 2>/dev/null || true
fi
echo "qmap 拨号已停止"
