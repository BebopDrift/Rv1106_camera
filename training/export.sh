#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
repo=$(realpath "${YOLOV5_DIR:-$root/yolov5-rockchip}")
train_python=${TRAIN_PYTHON:-$repo/.venv/bin/python}
rknn_python=${RKNN_PYTHON:-$root/.venv-rknn/bin/python}
[[ $# == 2 ]] || { echo "Usage: $0 WEIGHTS NEW_EXPORT_DIR" >&2; exit 2; }
weights=$(realpath -e "$1")
mkdir -- "$2"
cd "$2"
cp -- "$weights" connector.pt
"$train_python" "$repo/export.py" --rknpu --weights "$PWD/connector.pt" --include onnx --imgsz 640 --device cpu
test -s RK_anchors.txt
"$rknn_python" "$root/training/quantize.py" "$PWD"
