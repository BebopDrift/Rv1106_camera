#!/usr/bin/env bash
# 在工程中执行。--decode 用同一张图运行框解码和标注对照。
set -euo pipefail
here="$(cd -- "$(dirname -- "$0")" && pwd)"
project="${CONNECTOR_PROJECT:-$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || printf '%s' "$HOME/embedded_linux/luckfox/rv1106_inspection")}"
gpio=0
decode=0
case "${1:-}" in
    "") ;;
    --decode) decode=1 ;;
    --gpio) gpio=1 ;;
    *) echo "usage: bash $0 [--decode | --gpio]" >&2; exit 2 ;;
esac
(( $# <= 1 )) || { echo "too many arguments" >&2; exit 2; }
sdk="${LUCKFOX_SDK:-$HOME/embedded_linux/luckfox/sdk}"
export_dir="${CONNECTOR_EXPORT_DIR:-}"
if [[ -z "$export_dir" ]]; then
    shopt -s nullglob
    models=("$project"/connector_export.*/connector_rv1106.rknn)
    if (( ${#models[@]} != 1 )); then
        echo "请将 CONNECTOR_EXPORT_DIR 设为本次模型与 RK_anchors.txt 所在目录。" >&2
        exit 1
    fi
    export_dir="$(dirname -- "${models[0]}")"
fi
model="$export_dir/connector_rv1106.rknn"
calibration="$export_dir/calibration.txt"
python="$project/.venv-rknn/bin/python"
src="$here/connector_one_frame.c"
work="$project/build/connector"
cc_name=arm-rockchip830-linux-uclibcgnueabihf-gcc
cc="${CONNECTOR_CC:-$sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/$cc_name}"
if [[ ! -x "$cc" ]]; then
    cc="$(command -v "$cc_name" || true)"
fi
[[ -x "$cc" ]] || { echo "找不到 $cc_name，请把 CONNECTOR_CC 设为编译器的绝对路径。" >&2; exit 1; }
inputs=("$src" "$model")
if (( gpio )); then
    inputs+=("$project/src/gpio_v4l2_capture.c" "$export_dir/RK_anchors.txt")
else
    inputs+=("$calibration" "$python")
fi
for f in "${inputs[@]}"; do
    [[ -s "$f" ]] || { echo "缺少文件：$f" >&2; exit 1; }
done
adb get-state >/dev/null
mkdir -p "$work"

# 编译时使用 Rockchip v2.3.2 头文件，并链接从这块板上取回的运行库。
header="$work/rknn_api.h"
expected=c48e11a6f41b451a5fd1e4ad774ea60252d3d94f78bee9b21ea3d21b21deba9a
check_header() { printf '%s  %s\n' "$expected" "$1" | sha256sum --check --status; }
if [[ ! -s "$header" ]] || ! check_header "$header"; then
    tmp_header="$(mktemp "$work/rknn-api.XXXXXX")"
    trap 'rm -f -- "$tmp_header"' EXIT
    url=https://raw.githubusercontent.com/airockchip/rknn-toolkit2/v2.3.2/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h
    curl --fail --location --retry 2 --connect-timeout 12 --output "$tmp_header" "$url"
    check_header "$tmp_header" || { echo "rknn_api.h 校验失败" >&2; exit 1; }
    mv -- "$tmp_header" "$header"
    trap - EXIT
fi
if (( gpio )); then
    work="$(mktemp -d "$work/gpio.XXXXXX")"
    echo "本轮 Ubuntu 日志 $work/board.log"
fi
lib="${CONNECTOR_RKNN_LIB:-}"
if [[ -z "$lib" ]]; then
    lib="$work/librknnmrt.so"
    adb pull /oem/usr/lib/librknnmrt.so "$lib"
fi
"$cc" -std=gnu11 -O2 -Wall -Wextra -I"$(dirname -- "$header")" "$src" \
    "$here/connector_decode.c" "$lib" -lm -o "$work/connector_one_frame"
file "$work/connector_one_frame"

if (( gpio )); then
    "$cc" -std=gnu11 -O2 -Wall -Wextra "$project/src/gpio_v4l2_capture.c" -o "$work/gpio_v4l2_capture"
    "$cc" -std=gnu11 -O2 -Wall -Wextra "$here/nv12_to_rgb.c" -o "$work/nv12_to_rgb"
    board="$(adb shell 'mktemp -d /tmp/connector-gpio.XXXXXX' | tr -d '\r')"
    [[ "$board" =~ ^/tmp/connector-gpio\.[[:alnum:]]{6}$ ]] || { echo "invalid board work directory" >&2; exit 1; }
    cleanup_board() { timeout 5 adb shell "rm -rf '$board'" >/dev/null 2>&1 || :; }
    trap cleanup_board EXIT
    adb push "$work/connector_one_frame" "$work/gpio_v4l2_capture" "$work/nv12_to_rgb" \
        "$model" "$export_dir/RK_anchors.txt" "$here/gpio_one_shot.sh" "$here/inspection_result.awk" "$board/"
    timeout 55 adb shell "cd '$board' && chmod 755 connector_one_frame gpio_v4l2_capture nv12_to_rgb && sh ./gpio_one_shot.sh" \
        | tee "$work/board.log"
    # Also check the marker for adb versions that omit the remote shell exit status.
    grep -q '^INSPECT ' "$work/board.log" || { echo "未收到本轮 INSPECT，请看 $work/board.log" >&2; exit 1; }
    exit 0
fi

# 读取上次校准清单中的第一张图片。缩放、居中、填灰边，输出 RGB 字节。
env -u PYTHONPATH "$python" - "$calibration" "$work/frame.rgb" "$work/frame.json" <<'PY'
import json
import os
import sys
from pathlib import Path
import cv2
import numpy as np

listing = Path(sys.argv[1])
first = next((s.strip() for s in listing.read_text().splitlines() if s.strip()), "")
if not first:
    raise SystemExit(f"空校准清单 {listing}")
image_path = Path(os.environ.get("CONNECTOR_IMAGE") or listing.parent / first).absolute()
bgr = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
if bgr is None:
    raise SystemExit(f"无法读取 {image_path}")
height, width = bgr.shape[:2]
scale = min(640 / width, 640 / height)
w, h = max(1, round(width * scale)), max(1, round(height * scale))
resized = cv2.resize(bgr, (w, h), interpolation=cv2.INTER_LINEAR)
rgb = np.full((640, 640, 3), 114, dtype=np.uint8)
x, y = (640 - w) // 2, (640 - h) // 2
rgb[y:y+h, x:x+w] = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
rgb.tofile(sys.argv[2])
meta = dict(image=str(image_path), width=width, height=height,
            resized_width=w, resized_height=h, left=x, top=y)
Path(sys.argv[3]).write_text(json.dumps(meta))
print(f"输入图片 {image_path}\nRGB 输入 {rgb.nbytes} 字节")
PY

# adb 只用于传文件和启动程序。下面的 C 程序在板上直接调用 librknnmrt.so。
board=/tmp/connector-native
adb shell "mkdir -p $board"
adb push "$work/connector_one_frame" "$model" "$work/frame.rgb" "$board/"
board_args=""
if (( decode )); then
    adb push "$export_dir/RK_anchors.txt" "$board/RK_anchors.txt"
    board_args="RK_anchors.txt"
fi
adb shell "cd $board && chmod 755 connector_one_frame && LD_LIBRARY_PATH=/oem/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} ./connector_one_frame connector_rv1106.rknn frame.rgb $board_args" | tee "$work/board.log"
if (( decode )); then
    labels=()
    if [[ -n "${CONNECTOR_LABEL:-}" ]]; then labels+=("$CONNECTOR_LABEL"); fi
    env -u PYTHONPATH "$python" "$here/compare_frame.py" "$work/frame.json" "$work/board.log" "${labels[@]}"
fi
