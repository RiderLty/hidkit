#!/bin/sh
# tests/linux/probe.sh —— 构建并以 root 运行 hidkit_probe（验证真实键鼠）
#
#   ./tests/linux/probe.sh --list
#   ./tests/linux/probe.sh --vidpid 046d:c08b --dump-desc
#   SKIP_BUILD=1 ./tests/linux/probe.sh --index 0
#
# /dev/hidraw* 默认 root only，所以脚本用 sudo 运行 probe（构建不需要 root）。

set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
BIN="$DIR/hidkit_probe"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "[build] $BIN"
    cc -O2 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" \
       -o "$BIN" "$DIR/hidkit_probe.c" "$ROOT"/src/*.c
fi

if [ "$(id -u)" -eq 0 ]; then
    exec "$BIN" "$@"
fi
exec sudo "$BIN" "$@"
