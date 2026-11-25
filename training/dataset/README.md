# Canonical connector dataset

This directory is the self-contained training and validation dataset used by the current RV1106 connector release candidate.

- \`train.txt\`: 33 training entries, 29 unique images.
- \`images/train/\` and \`labels/train/\`: 29 images and 29 matching YOLO label files.
- \`images/val/\` and \`labels/val/\`: 6 validation images and 6 matching labels.
- \`SHA256SUMS\`: checksums for all files in this dataset, excluding the checksum file itself.

The repeated entries in \`train.txt\` are intentional oversampling, not duplicate-file mistakes:

- \`exposed_side.png\` appears three times.
- \`bg_PTBE4p_02.png\` appears three times.

The label file \`labels/train/bg_PTBE4p_02.txt\` is intentionally empty. This image is a negative/background sample and contains no target bounding box.

The 33-entry training manifest, all unique image contents, all label contents, the six validation images, and the six validation labels were compared with the final training run. They match.


## 数据源约定

本目录是唯一正式训练数据源。data/connector_detection/ 保存原始现场采集、独立验证图像和板端回放证据；这些现场数据不会自动进入训练。需要新增训练样本时，应先完成标注、复核类别与框，再显式复制到本目录并更新 train.txt 和 SHA256SUMS。

早期 connector_yolo_ja6o95w4、to-label 和 connector_plus_view01 暂存目录中的图片/标签已逐文件核对并合并到本目录，因此正式仓库不再保留这些重复副本。finetune.py 会在每次训练运行目录生成临时 data.yaml，不需要在本目录保存指向开发机绝对路径的 data.yaml。
