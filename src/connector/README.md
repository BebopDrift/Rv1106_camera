# RV1106 连接器单帧检测

在 Luckfox Pico Max 上运行两类 YOLOv5 模型。类别 0 为已插（plug_inserted），类别 1 为裸针（pins_exposed）。模型输入为一张等比例缩放、灰边填充的 640×640 RGB 图。基础程序已收到用户实机输出 `PASS native one-frame inference`。

## 在 Ubuntu 项目根目录运行

连接一块 ADB 可见的开发板，准备 Luckfox ARM uClibc 工具链、RKNN 2.3.2 运行库以及含 OpenCV 和 NumPy 的 Python 环境。沿用已跑通的工程布局，导出目录含 connector_rv1106.rknn、calibration.txt、RK_anchors.txt。默认选取校准清单第一张图片；目录中存在多个模型时，显式指定对应的导出目录。

```bash
bash src/connector/run_connector_native.sh
CONNECTOR_EXPORT_DIR="$PWD/connector_export.hDk3XV" bash src/connector/run_connector_native.sh --decode
```

可通过 CONNECTOR_IMAGE 和 CONNECTOR_LABEL 指定另一对样本和 YOLO 标签。默认根据 images/<split>/<图名> 找 labels/<split>/<同名>.txt。本次对照支持每图一个人工框。CONNECTOR_CC 接受交叉编译器的路径，CONNECTOR_RKNN_LIB 可指定编译环境中匹配目标板 ABI 和版本的 librknnmrt.so；未设置库路径时，Ubuntu 取回板上的库用于链接。板端运行时仍使用 /oem/usr/lib 下的运行库。Ubuntu 的构建文件、输入与单帧日志保存在 build/connector/。

## 从张量读到框

1. connector_one_frame.c 查询并校验三个检测头的布局。板端 C 在 --decode 对应的模式下把已验证的缓冲区和量化参数交给 connector_decode.c。
2. decode_head 对每个网格的三个 anchor（锚框）读取各 7 个数。前四个用于还原中心、宽、高。后面三个分别是目标概率、已插分数、裸针分数。RKNN 导出图已经包含 sigmoid。最终分数为目标概率乘较高的类别分数。
3. 解析实际导出的 9 对锚框。保留分数至少 0.25 的非退化框；同类别内做非极大值抑制（NMS），IoU 大于 0.45 时去掉重叠的低分框，最多输出 300 个。BEST_RAW 单独输出最高分原始候选，方便诊断低分模型。
4. compare_frame.py 使用预处理时记录的实际缩放尺寸和填边，把框还原到原图像素坐标，再与对应标注计算交并比（IoU）。COMPARE 的 match=yes 表示至少一个正式检测框类别正确且 IoU ≥ 0.5。match=no 也可以是一次完整的解码运行。单张校准图片的结果不能代替独立验证集指标。

终端的 DECODE 行应有 scanned=25200，即 3×(80²+40²+20²)。GT 和 BEST_RAW_IMAGE 均使用原图像素；其中只有 DETECTION_IMAGE 计入正式检测。此前这份连接器模型的验证指标为零。

## 主机侧解码回归测试

在仓库根目录执行。合成量化张量覆盖三种内存布局、填充、两类重叠框及解码坐标。

```bash
mkdir -p build/connector
cc -std=c11 -O2 -Wall -Wextra -Werror src/connector/connector_decode.c tests/connector_decode_test.c -lm -o build/connector/test_decode
build/connector/test_decode
```

解码公式参考 [Rockchip YOLOv5 后处理](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov5/python/yolov5.py)。
