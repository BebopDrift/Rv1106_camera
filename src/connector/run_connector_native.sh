#!/usr/bin/env bash
# 在 Ubuntu 运行。把本脚本和 connector_one_frame.c 放在同一目录。
set -euo pipefail
project="${CONNECTOR_PROJECT:-$HOME/embedded_linux/luckfox/rv1106_inspection}"
sdk="${LUCKFOX_SDK:-$HOME/embedded_linux/luckfox/sdk}"
export_dir="$project/connector_export.hDk3XV"
model="$export_dir/connector_rv1106.rknn"
calibration="$export_dir/calibration.txt"
python="$project/.venv-rknn/bin/python"
src="$(cd -- "$(dirname -- "$0")" && pwd)/connector_one_frame.c"
work="$project/native_connector"
cc_name=arm-rockchip830-linux-uclibcgnueabihf-gcc
cc="${CONNECTOR_CC:-$sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/$cc_name}"
if [[ ! -x "$cc" ]]; then
    cc="$(command -v "$cc_name" || true)"
fi
[[ -x "$cc" ]] || { echo "找不到 $cc_name，请把 CONNECTOR_CC 设为编译器的绝对路径。" >&2; exit 1; }
for f in "$src" "$model" "$calibration" "$python"; do
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
adb pull /oem/usr/lib/librknnmrt.so "$work/librknnmrt.so"
"$cc" -std=gnu11 -O2 -Wall -Wextra -I"$work" "$src" \
    -L"$work" -lrknnmrt -o "$work/connector_one_frame"
file "$work/connector_one_frame"

# 读取上次校准清单中的第一张图片。缩放、居中、填灰边，输出 RGB 字节。
env -u PYTHONPATH "$python" - "$calibration" "$work/frame.rgb" <<'PY'
import sys
from pathlib import Path
import cv2
import numpy as np

listing = Path(sys.argv[1])
first = next((s.strip() for s in listing.read_text().splitlines() if s.strip()), "")
if not first:
    raise SystemExit(f"空校准清单 {listing}")
image_path = listing.parent / first
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
print(f"输入图片 {image_path}\nRGB 输入 {rgb.nbytes} 字节")
PY

# adb 只用于传文件和启动程序。下面的 C 程序在板上直接调用 librknnmrt.so。
board=/tmp/connector-native
adb shell "mkdir -p $board"
adb push "$work/connector_one_frame" "$model" "$work/frame.rgb" "$board/"
adb shell "cd $board && chmod 755 connector_one_frame && LD_LIBRARY_PATH=/oem/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} ./connector_one_frame connector_rv1106.rknn frame.rgb"
