# 数据目录说明

本目录保存现场采集原图、独立验证场景和板端回放记录。它不是训练入口。

当前唯一正式训练数据集位于 training/dataset/。其中包含训练、验证图片、YOLO 标签、训练清单和 SHA256 校验。训练时只使用该目录，不要直接把本目录下所有现场样本混入训练集，否则会造成训练集与验证集污染。

## 保留的现场数据

| 目录 | 内容 | 使用说明 |
|---|---|---|
| connector_detection/background_20261125_RN5yQs/ | 纯背景、黑色物体和针座可见等场景的三帧采集、推理日志 | 用于分析背景误报和漏检；scene_0uVqWS 实际可见裸针，不能当作纯背景负样本；scene_PTBE4p 才是针座移出后的背景场景 |
| connector_detection/check_20261125_D2FcBW/ | 插入状态、裸针状态的三帧实测图像、板端日志、标注框和对比图 | 独立现场检查，不自动加入训练 |
| connector_detection/connector_field13_scr0dfsp/ | burst_check/ 与 refocus_check/ 现场样本及对应标注 | 保留作独立回放检查；其中原 images/train/ 和 labels/train/ 已与正式训练集重复，已删除重复副本 |
| connector_detection/field_20261124_0Q2ZmE/ | 现场连续帧采集及 samples.csv 状态记录 | 保留原始采集上下文；其中重复进入训练集的图片仍以 training/dataset/ 为准 |
| connector_detection/paired_20261125_yEKUJ1/ | 两类状态配对采集、重复拍摄、板端检测 CSV、参考框和复核图 | 用于记录现场配对样本和复测；pair_train.txt 使用相对路径，正式训练副本在 training/dataset/ |

## 已清理的重复或过期数据

- connector_yolo_ja6o95w4/：原始 YOLO 训练/验证图片和标签已逐文件与 training/dataset/ 核对，内容重复。
- to-label/：16 张待标注暂存图片均与正式数据集图片完全相同。
- connector_plus_view01/：新增视角图片和标签均已进入正式训练集。
- 旧的 data/part_presence/：属于早期 ROI/像素模板分类实验，不属于当前 YOLOv5 + RKNN 流程，已从正式项目移除。
- 早期 check_live.py、check_background.py 等一次性脚本引用已经清理的 runs/ 实验目录，不再作为项目脚本保留。采集 RTSP 连续帧请使用 scripts/capture_rtsp_burst.sh。

## 校验现场数据

在项目根目录执行：

    cd data/connector_detection
    sha256sum -c SHA256SUMS

现场图像、标注和回放日志属于实验依据。除非明确重新采集并复核，不要覆盖或修改原始文件。
