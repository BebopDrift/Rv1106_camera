# RV1106 GPIO 触发式连接器视觉检测

基于 Luckfox Pico Max（Rockchip RV1106）和 SC3336 摄像头的嵌入式目标检测项目。GPIO 事件触发现场抓拍，板端调用 RKNN Runtime 使用 YOLOv5n 模型进行 NPU 推理，并输出连接器状态。

## 项目目标与检测类别

- **plug_inserted（类别 0）**：连接器已插入。
- **pins_exposed（类别 1）**：针脚暴露、连接器未正确插入。

模型输入为等比例缩放并填充灰边的 640×640 RGB 图像。GPIO 现场流程使用 SC3336 输出的 2304×1296 NV12 图像。

## 硬件与软件环境

| 项目 | 配置 |
|---|---|
| 开发板 | Luckfox Pico Max |
| SoC | Rockchip RV1106 |
| 摄像头 | Waveshare SC3336 |
| Linux | 5.10.160 |
| 根文件系统 | Buildroot 2023.02.6 |
| 模型 | YOLOv5n，两类目标 |
| 部署格式 | RKNN INT8 |
| NPU Runtime | Rockchip RKNN Runtime 2.3.2 |

## 系统流程

    GPIO 上升沿
        ↓
    hello-platform GPIO 事件
        ↓
    V4L2 从 /dev/video11 获取 SC3336 NV12 帧
        ↓
    NV12 → RGB，缩放到 640×360，上下填充 140 行
        ↓
    RKNN Runtime 在 RV1106 NPU 上推理
        ↓
    YOLOv5 三检测头解码、置信度筛选、NMS
        ↓
    INSPECT 输出 plug_inserted / pins_exposed / unknown

## 主要目录

| 路径 | 内容 |
|---|---|
| src/gpio_v4l2_capture.c | GPIO 事件与 V4L2 帧匹配、抓拍 |
| src/connector/ | 单帧 RKNN 推理、YOLOv5 解码、NV12 预处理和 GPIO 现场脚本 |
| training/ | 微调、导出、INT8 量化脚本及唯一正式训练数据集 |
| data/connector_detection/ | 原始现场采集、独立验证场景和板端回放记录 |
| models/demo/ | 当前板端实测候选模型、锚框、量化校准清单和 SHA256 |
| scripts/ | RTSP 单帧与连续帧采集辅助脚本 |
| tests/ | C 解码回归测试和 GPIO 流程脚本测试 |
| hardware/ | 板级配置补丁及硬件相关材料 |
| docs/validation/ | 分阶段验证记录 |
| docs/validation/release/ | 当前候选模型的现场日志、场景统计和训练记录 |

注意：training/dataset/ 是唯一正式训练数据源；data/connector_detection/ 保存原始现场样本和独立验证证据，不应直接整体并入训练集。详细说明见 data/README.md。

## 模型文件与校验

当前部署候选位于 models/demo/：

- connector.pt：对应的 PyTorch 权重。
- connector_rv1106.rknn：RV1106 INT8 部署模型。
- RK_anchors.txt：YOLOv5 三检测头使用的 9 组 Anchor 宽高。
- calibration.txt：13 张量化校准图片的相对路径清单。
- SHA256SUMS：模型权重、RKNN 模型和 Anchor 文件的校验值。

从项目根目录检查：

    cd models/demo
    sha256sum -c SHA256SUMS

校准清单中的图片路径相对于 models/demo/ 解析，图片保存在 training/dataset/images/train/。重新量化时，training/quantize.py 使用这份清单，并要求清单包含 13 张图片。

## Ubuntu 主机环境与部署前提

在 Ubuntu 项目根目录执行部署脚本。需要：

- Luckfox ARM uClibc 交叉工具链（arm-rockchip830-linux-uclibcgnueabihf-gcc）。
- ADB 能识别开发板。
- 板端 /oem/usr/lib/librknnmrt.so 可用。
- Ubuntu 具备 bash、adb、curl、ffmpeg、sha256sum、file 等命令；RTSP 现场采集脚本需要 ffmpeg。
- 单帧图片预处理和标注对照需要项目 .venv-rknn/ 中的 Python、OpenCV、NumPy。
- GPIO 现场模式还要求板端已有 hello-platform GPIO 驱动、/dev/hello-gpio-events、/dev/video11 和 SC3336/AIQ 采集环境。

脚本默认从项目内 models/demo/ 选择模型；如果需要使用其他导出目录，可设置 CONNECTOR_EXPORT_DIR。也保留旧版 connector_export.* 目录的兼容回退（仅在恰好找到一个时使用）。

常用环境变量：

| 变量 | 作用 |
|---|---|
| CONNECTOR_PROJECT | 指定项目根目录 |
| LUCKFOX_SDK | 指定 Luckfox SDK 根目录 |
| CONNECTOR_EXPORT_DIR | 覆盖默认模型目录 |
| CONNECTOR_CC | 指定 ARM 交叉编译器 |
| CONNECTOR_RKNN_LIB | 指定与目标板 ABI/版本匹配的 librknnmrt.so |
| CONNECTOR_IMAGE | 指定单帧推理/对照使用的图片 |
| CONNECTOR_LABEL | 指定该图片对应的 YOLO 标注文件 |

## 现场样本采集

开发板 RTSP 服务已启动且 ADB 可连接时，可采集连续三帧 I-frame 作为现场复核样本：

    ADB_SERIAL=<开发板序列号> bash scripts/capture_rtsp_burst.sh

默认保存在 data/connector_detection/scene_XXXXXX/。也可通过 CAPTURE_DIR 指定保存目录。采集后应人工记录场景状态，并将是否参与训练与独立验证明确区分。

## 运行方式

以下命令均从项目根目录执行。

### 1. 单帧 NPU 推理

默认使用 models/demo/，从 calibration.txt 读取第一张图片，进行预处理并在开发板上运行原生 C 推理：

    bash src/connector/run_connector_native.sh

### 2. 单帧解码与人工标注对照

    bash src/connector/run_connector_native.sh --decode

默认通过图片路径推导 labels/<split>/<同名>.txt。自定义图片时，通常同时指定 CONNECTOR_IMAGE 和 CONNECTOR_LABEL：

    CONNECTOR_IMAGE=/path/to/frame.png \
    CONNECTOR_LABEL=/path/to/frame.txt \
    bash src/connector/run_connector_native.sh --decode

对照程序将检测框映射回原图坐标并计算 IoU。COMPARE match=yes 表示至少有一个正式检测框类别正确且 IoU ≥ 0.5。BEST_RAW 是阈值前最高分候选，只用于诊断，不计入正式检测框。

### 3. GPIO 触发现场检测

先确认 GPIO 回环接线、驱动和相机环境正常，确保本次程序可以独占 /dev/video11。然后执行：

    bash src/connector/run_connector_native.sh --gpio

脚本会交叉编译并部署采集、预处理和推理程序，等待 CAPTURE_READY，预热后触发 GPIO 上升沿，检查 TRIGGER 与 MATCH 是否属于同一事件，并处理本轮新抓取的 NV12 图像。日志保存在 build/connector/gpio.XXXXXX/board.log。

GPIO 模式只需要部署模型和 RK_anchors.txt，不依赖校准图片或人工标签。若板端原本没有运行 rkaiq_3A_server，脚本会按现场条件尝试临时启动；若已有其他相机服务占用设备，则会停止并报告冲突。

### 4. 结果含义

| 输出 | 含义 |
|---|---|
| TRIGGER / MATCH | GPIO 事件及其匹配的视频帧序号 |
| SAVED | 本轮 NV12 快照已保存 |
| PREPROCESS | NV12 已转换为 640×640 RGB |
| DECODE | 三个检测头共遍历 25,200 个候选框 |
| DETECTION | 通过置信度筛选和 NMS 的正式检测框 |
| BEST_RAW | 最高分原始候选，仅供排查 |
| INSPECT | 汇总状态；无框或类别冲突时为 unknown |
| PASS | 原生单帧推理流程完成 |

当前解码参数：置信度阈值 0.25，同类别 NMS IoU 阈值 0.45，最多保留 300 个检测框。每个 Anchor 对应 7 个值：边界框 4 个、objectness 1 个、类别分数 2 个。RKNN 导出图已包含 sigmoid；最终分数为 objectness 乘以较高的类别分数。

### 5. 图像预处理说明

nv12_to_rgb.c 针对本项目已验收的紧凑 NV12 帧（2304×1296，4,478,976 字节）处理。将画面缩放到 640×360，上下各填充 140 行灰色，生成 640×640 RGB。

颜色转换代码采用全范围 BT.601 系数；当前采集日志的颜色编码字段报告为 Default，因此该转换约定应在更换摄像头、驱动或采集配置后重新核对。

## 训练与模型导出

训练数据和脚本位于 training/。当前数据集是独立整理后的完整训练集：

- 训练清单 33 条、29 张不同图片；exposed_side.png 和 bg_PTBE4p_02.png 各重复 3 次，是有意过采样。
- bg_PTBE4p_02.txt 是有意保留的空标签，表示负样本背景图。
- 验证集 6 张图片、6 份标签。
- 数据集校验：在项目根目录执行 (cd training/dataset && sha256sum -c SHA256SUMS)。

### 1. 准备 YOLOv5 训练环境

微调脚本要求 airockchip/yolov5 固定 revision：

    d25a07534c14f44296f9444bab2aa5c601cdaaab

首次配置：

    git clone https://github.com/airockchip/yolov5.git yolov5-rockchip
    git -C yolov5-rockchip checkout d25a07534c14f44296f9444bab2aa5c601cdaaab
    git -C yolov5-rockchip apply ../training/pillow.patch
    python3 -m venv yolov5-rockchip/.venv
    source yolov5-rockchip/.venv/bin/activate
    python -m pip install --index-url https://download.pytorch.org/whl/cpu torch==2.5.1+cpu torchvision==0.20.1+cpu
    python -m pip install -r training/requirements-train.txt

pillow.patch 修复 Pillow 13 移除旧字体尺寸接口造成的兼容问题。requirements-train.txt 对应 Python 3.12、PyTorch 2.5.1 CPU 环境。

### 2. 训练

在项目根目录、激活 yolov5-rockchip/.venv 后执行：

    YOLOV5_DIR="$PWD/yolov5-rockchip" \
      python training/finetune.py "$PWD/runs/train/my_finetune"

输出目录必须尚不存在。脚本使用 training/fit15_start.pt 作为初始权重，使用 training/dataset/ 和 training/hyp.yaml，训练 10 个 epoch，batch size 2，输入 640，Adam，CPU，workers=0，RAM cache。训练脚本将 nominal batch size 设为实际 batch size，因此每个 batch 都执行一次 optimizer update。

训练输出位于指定目录的 fit/weights/，包含 best.pt 和 last.pt。二者含义不同：best.pt 是训练验证指标最好的 checkpoint；last.pt 是最后一个 epoch 的 checkpoint。

当前 models/demo/connector.pt 对应最后一个正式实验的 last.pt，而不是 best.pt。当前 release 训练和板端验证依据见 docs/validation/release/model-provenance.md。不要在未重新做板端验证的情况下把 best.pt 直接替换到 release 模型。

### 3. ONNX 导出与 RKNN INT8 量化

先准备 RKNN 工具环境：

    python3 -m venv .venv-rknn
    .venv-rknn/bin/python -m pip install -r training/requirements-rknn.txt

导出并量化时，目标目录必须尚不存在。若要按当前 release 的 checkpoint 选择方式复现，使用 last.pt：

    bash training/export.sh \
      "$PWD/runs/train/my_finetune/fit/weights/last.pt" \
      "$PWD/connector_export.my_finetune"

若要单独比较训练验证最优权重，可把命令中的 last.pt 改为 best.pt，但生成的模型不等于当前 models/demo release。

导出脚本生成 ONNX、RK_anchors.txt 和 RV1106 RKNN 文件。量化使用 models/demo/calibration.txt 中的 13 张校准图片。训练和量化依赖分别记录在 training/requirements-train.txt 与 training/requirements-rknn.txt。

## 自动化测试

主机侧 YOLOv5 解码回归测试：

    mkdir -p build/connector
    cc -std=c11 -O2 -Wall -Wextra -Werror \
      src/connector/connector_decode.c \
      tests/connector_decode_test.c -lm \
      -o build/connector/test_decode
    build/connector/test_decode

GPIO 流程脚本测试：

    python3 tests/test_gpio_inspection.py

这些测试覆盖解码布局、候选框处理和 GPIO 检测脚本逻辑；它们不能替代开发板上的摄像头、GPIO、NPU 实机验收。

## 当前验证状态与限制

models/demo/ 是当前实战候选模型，不应将其描述为已经达到量产或完整泛化验收：

- models/demo/SHA256SUMS 已核对通过。当前 PyTorch 权重、RKNN 模型和 Anchor 的来源及 SHA256 见 docs/validation/release/model-provenance.md。
- docs/validation/release/live.log 记录了一次板端 pins_exposed 正式检测，置信度为 0.45702428。
- docs/validation/release/scenes.csv 记录的初始背景 3 帧和移动背景 3 帧均无正式检测；此前裸针场景 3 帧中有 2 帧检出、1 帧漏检。
- docs/validation/release/board/board_eval.log 记录了批量板端回放：新训练样本和同一连拍样本通过；refocus 组检出 1/2；旧验证组检出 5/6 但有 6 个额外框；两个 hold 状态组均出现额外检测。
- docs/validation/release/training/ 保存训练日志、训练参数、曲线和混淆矩阵；docs/validation/release/board/ 保存模型导出、量化和板端回放日志。

当前 models/demo/connector.pt 对应最后一个 epoch 的 last.pt；训练验证指标较高的 best.pt 另存为 training/checkpoints/fit_bg_best.pt。两者没有被混为同一个模型。当前模型仍有误报和漏报，训练集及验证集规模较小，不能仅凭训练日志中的 mAP 判断泛化能力。

因此，当前版本证明了 GPIO—摄像头—NPU—解码链路可运行，并在部分现场样本上正确识别；仍需继续降低误报、漏报，并使用更多独立现场样本完成验证。

## 相关实现说明

- connector_one_frame.c：RKNN 模型加载、Tensor 属性查询、内存绑定、NPU 推理。
- connector_decode.c/.h：三检测头解码、置信度筛选和 NMS。
- compare_frame.py：与 YOLO 标注进行 IoU 对照。
- nv12_to_rgb.c：现场 NV12 转 RGB。
- gpio_one_shot.sh：板端 GPIO 触发、采集、预处理和检测流程。
- inspection_result.awk：根据正式检测框生成 INSPECT 状态。
- run_connector_native.sh：主机交叉编译、部署和启动入口。
