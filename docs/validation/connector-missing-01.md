# RV1106 单帧解码实测 · 2026-11-23

在 Luckfox Pico Max 上对校准清单中的训练样本 `batch.ZSPG6n-aiq_missing_01.jpg` 执行了原生 C 单帧推理与两类解码。下面是用户回传的终端输出。前三行的范围与此前 Python 远程推理日志在显示精度内一致。

```text
output[0] logical=1x21x80x80 native=NC1HWC2 min=0.000000 max=0.986989
output[1] logical=1x21x40x40 native=NC1HWC2 min=0.000000 max=0.927754
output[2] logical=1x21x20x20 native=NC1HWC2 min=0.003568 max=0.877844
DECODE scanned=25200 above=0 kept=0 score_min=0.25 nms_iou=0.45
BEST_RAW class=0 score=0.05096139 xyxy=327.536469,-74.633026,393.337311,328.779449
PASS native one-frame inference
GT image=batch.ZSPG6n-aiq_missing_01.jpg class=1(pins_exposed) xyxy=897.6,699.0,1029.0,824.9
BEST_RAW_IMAGE class=0(plug_inserted) score=0.05096139 xyxy=1179.1,0.0,1416.0,679.6 iou=0.0000
COMPARE detections=0 max_same_class_iou=0.0000 match=no
```

程序遍历了 25,200 个候选。最高候选得分为 0.05096139，低于 0.25。进入非极大值抑制（NMS）的框数为零，最终检测列表为空。人工标注为裸针（1）；最高分候选类别为已插（0），且映射回原图后的框与标注没有重叠。记录该帧为漏检。此处 IoU 为零只描述日志中这一次对照，不能推断所有原始候选的 IoU 都为零。

在 Ubuntu 工程根目录、使用同一导出目录及校准清单时，可按下列命令复查。

```bash
bash src/connector/run_connector_native.sh --decode
```
