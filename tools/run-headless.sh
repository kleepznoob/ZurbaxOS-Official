#!/bin/sh
# Starts a headless Zurbax desktop for development: tools/run-headless.sh [WxH] [seconds]
# Control it with tools/zctl.py /tmp/zx/ctl.sock 'command' ...
set -e
cd "$(dirname "$0")/.."
SIZE=${1:-1600x900}
SECS=${2:-60}
mkdir -p /tmp/zx/data/system
[ -f /tmp/zx/zdisplay.pid ] && kill "$(cat /tmp/zx/zdisplay.pid)" 2>/dev/null && sleep 0.2 || true
mkdir -p /tmp/zx/home
# the desktop and the apps it starts share /tmp/zx/home, like a real session
HOME=/tmp/zx/home ZURBAX_HOME=/tmp/zx/home ZURBAX_ROOT=$PWD/build/root/system ZURBAX_DATA=/tmp/zx/data ZUI_SOCKET=/tmp/zx/display.sock \
	ZOS_WAYLAND_SOCKET=/tmp/zx/wayland-0 \
	timeout "$SECS" build/bin/zdisplay --headless "$SIZE" --control /tmp/zx/ctl.sock > /tmp/zx/zdisplay.log 2>&1 &
echo $! > /tmp/zx/zdisplay.pid
sleep 1
