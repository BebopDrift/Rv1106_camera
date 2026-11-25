#!/usr/bin/env bash
# Sample a decodable I-frame from the board's existing RTSP stream.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build/live"
out=$(mktemp -d "$root/build/live/shot.XXXXXX")
port=$(adb forward tcp:0 tcp:554 | tr -d '\r\n')
[[ $port =~ ^[0-9]+$ ]]
trap 'adb forward --remove "tcp:$port" >/dev/null' EXIT
timeout 40 ffmpeg -nostdin -hide_banner -loglevel error -rtsp_transport tcp \
    -i "rtsp://127.0.0.1:$port/live/0" -map 0:v:0 -an \
    -vf 'select=eq(pict_type\,I),select=gte(n\,3)' -frames:v 1 -threads 1 "$out/input.png"
test -s "$out/input.png"
CONNECTOR_IMAGE="$out/input.png" bash "$root/src/connector/run_connector_native.sh" --detect | tee "$out/run.log"
echo "现场画面和日志 $out"
