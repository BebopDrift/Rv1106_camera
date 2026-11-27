#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "用法：bash scripts/setup-training-env.sh [--record | --with-rknn]"
    echo "  --record      记录当前训练环境、转换环境和 YOLO 源码补丁"
    echo "  无参数        恢复 YOLOv5 CPU 训练环境"
    echo "  --with-rknn   同时恢复 RKNN 模型转换环境"
}

die() {
    echo "错误：$*" >&2
    exit 1
}

[[ $# -le 1 ]] || { usage >&2; exit 2; }
mode=${1:-}
case "$mode" in
    --help|-h) usage; exit 0 ;;
    ''|--record|--with-rknn) ;;
    *) usage >&2; exit 2 ;;
esac

root=$(git -C "$(dirname -- "$0")" rev-parse --show-toplevel)
config_dir="$root/training"
yolo_dir="$root/yolov5-rockchip"
train_env="$yolo_dir/.venv"
rknn_env="$root/.venv-rknn"

check_python() {
    "$1" - <<'PY'
import platform
import sys

if sys.version_info[:2] != (3, 12):
    sys.exit(f"需要 Python 3.12，当前是 {sys.version.split()[0]}")
if platform.system() != "Linux" or platform.machine() != "x86_64":
    sys.exit("此脚本用于 Ubuntu x86_64 主机")
PY
}

check_yolo_repo() {
    [[ -e "$yolo_dir/.git" ]] || die "找不到 YOLO 仓库：$yolo_dir"
    [[ "$(git -C "$yolo_dir" rev-parse --show-toplevel)" == "$yolo_dir" ]] ||
        die "YOLO 仓库路径异常：$yolo_dir"
}

record_packages() {
    local python=$1
    local output=$2

    [[ -x "$python" ]] || die "找不到原环境的 Python：$python"
    check_python "$python"
    "$python" -m pip freeze > "$output"
    [[ -s "$output" ]] || die "包清单为空：$python"

    # 本地路径和可编辑安装不能直接在另一台电脑上恢复。
    if grep -Evq '^[[:alnum:]_.-]+==[[:alnum:]_.+!-]+$' "$output"; then
        die "包清单中有本地路径或非固定版本依赖，请先检查：$python -m pip freeze"
    fi
}

if [[ "$mode" == "--record" ]]; then
    check_yolo_repo
    mkdir -p "$config_dir"

    # 先在临时目录生成全部文件，检查通过后再写入项目。
    temp_dir=$(mktemp -d)
    trap 'rm -rf -- "$temp_dir"' EXIT

    record_packages "$train_env/bin/python" "$temp_dir/requirements-train.txt"
    record_packages "$rknn_env/bin/python" "$temp_dir/requirements-rknn.txt"

    git -C "$yolo_dir" rev-parse HEAD > "$temp_dir/yolo-revision.txt"

    # 相对 HEAD 导出，包含 plots.py 已暂存和未暂存的修改。
    git -C "$yolo_dir" diff --no-ext-diff --binary HEAD -- utils/plots.py \
        > "$temp_dir/pillow.patch"

    other_changes=$(git -C "$yolo_dir" status --porcelain \
        --untracked-files=normal -- . ':(exclude)utils/plots.py')

    if [[ -n "$other_changes" ]]; then
        printf '%s\n' "$other_changes" >&2
        die "YOLO 仓库还有上述文件需要单独保存"
    fi

    for name in requirements-train.txt requirements-rknn.txt \
                yolo-revision.txt pillow.patch; do
        cp -- "$temp_dir/$name" "$config_dir/$name"
    done

    echo "记录完成：两套环境的依赖、YOLO 提交号、plots.py 补丁均已保存到 training/"
    exit 0
fi

# 以下是在新克隆的项目里恢复环境。
python=${PYTHON:-python3.12}
command -v "$python" >/dev/null || die "请先安装 Python 3.12 和对应的 venv 模块"
check_python "$python"

for name in requirements-train.txt yolo-revision.txt pillow.patch; do
    [[ -f "$config_dir/$name" ]] || die "缺少 training/$name，请先在旧项目运行 --record"
done

revision=$(cat "$config_dir/yolo-revision.txt")
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || die "YOLO 提交号格式错误"

if [[ ! -e "$yolo_dir" ]]; then
    git clone --no-checkout https://github.com/airockchip/yolov5.git "$yolo_dir"
    git -C "$yolo_dir" checkout --detach "$revision"
fi

check_yolo_repo
[[ "$(git -C "$yolo_dir" rev-parse HEAD)" == "$revision" ]] ||
    die "现有 YOLO 仓库与记录的提交号不同，请检查 $yolo_dir"

patch="$config_dir/pillow.patch"
if [[ -s "$patch" ]]; then
    if git -C "$yolo_dir" apply --reverse --check "$patch" 2>/dev/null; then
        echo "plots.py 补丁已经存在"
    else
        git -C "$yolo_dir" apply --check "$patch"
        git -C "$yolo_dir" apply "$patch"
    fi
fi

install_env() {
    local env_dir=$1
    local requirements=$2
    local env_python
    local -a cpu_packages

    [[ -s "$requirements" ]] || die "缺少依赖清单：$requirements"

    if [[ ! -e "$env_dir" ]]; then
        "$python" -m venv "$env_dir"
    fi
    [[ -f "$env_dir/pyvenv.cfg" ]] || die "目标目录不是 Python 虚拟环境：$env_dir"

    env_python="$env_dir/bin/python"
    check_python "$env_python"

    # 原训练环境使用 2.5.1+cpu 这类版本，先从 PyTorch CPU 源安装。
    mapfile -t cpu_packages < <(
        grep -E '^(torch|torchvision|torchaudio)==[^ ]+\+cpu$' "$requirements" || true
    )

    if (( ${#cpu_packages[@]} > 0 )); then
        "$env_python" -m pip install --no-deps \
            --index-url https://download.pytorch.org/whl/cpu \
            "${cpu_packages[@]}"
    fi

    "$env_python" -m pip install -r "$requirements"
    "$env_python" -m pip check
}

install_env "$train_env" "$config_dir/requirements-train.txt"

(
    cd "$yolo_dir"
    "$train_env/bin/python" -c \
        'import torch, torchvision, cv2, utils.plots; print("YOLO imports OK, torch", torch.__version__)'
)

if [[ "$mode" == "--with-rknn" ]]; then
    install_env "$rknn_env" "$config_dir/requirements-rknn.txt"
    "$rknn_env/bin/python" -c \
        'from rknn.api import RKNN; import onnx; print("RKNN import OK, ONNX", onnx.__version__)'
fi
