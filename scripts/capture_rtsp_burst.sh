#!/usr/bin/env bash
# Capture three decodable I-frames from the board's existing RTSP stream.
# Usage: ADB_SERIAL=<board-serial> [CAPTURE_DIR=data/connector_detection] bash scripts/capture_rtsp_burst.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
serial="$(printenv ADB_SERIAL || true)"
if [[ -z "$serial" ]]; then
    echo "请设置 ADB_SERIAL，例如 ADB_SERIAL=<开发板序列号> bash $0" >&2
    exit 2
fi
dest_parent="$(printenv CAPTURE_DIR || true)"
if [[ -z "$dest_parent" ]]; then
    dest_parent="$root/data/connector_detection"
fi
mkdir -p "$dest_parent"
dest_parent=$(cd -- "$dest_parent" && pwd)

adb -s "$serial" get-state >/dev/null
out=$(mktemp -d "$dest_parent/scene_XXXXXX")
port=$(adb -s "$serial" forward tcp:0 tcp:554 | tr -d '\r\n')
[[ "$port" =~ ^[0-9]+$ ]]
trap 'adb -s "$serial" forward --remove "tcp:$port" >/dev/null 2>&1 || true' EXIT

timeout 40 ffmpeg -nostdin -hide_banner -loglevel error -rtsp_transport tcp \
    -i "rtsp://127.0.0.1:$port/live/0" -map 0:v:0 -an \
    -vf 'select=eq(pict_type\,I),select=gte(n\,2)' \
    -fps_mode vfr -frames:v 3 -threads 1 "$out/frame_%02d.png"

test -s "$out/frame_03.png"
file "$out"/frame_*.png
echo "现场三帧已保存：$out"
