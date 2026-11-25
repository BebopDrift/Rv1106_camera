# Release candidate training and model provenance

## Selected release artifacts

The deployed candidate is stored in \`models/demo/\`.

| Artifact | SHA-256 | Meaning |
|---|---|---|
| \`training/fit15_start.pt\` | \`b9d33a9819440a9d7a3c68d724c836f67fbc61472f6728c20e0a1d8503cc2e36\` | Initial checkpoint used for the final 10-epoch fine-tune |
| \`training/checkpoints/fit_bg_best.pt\` | \`c900818df48c326c5eaa5d9f232484f286fe08b6eda4a1526e38e5d6208c3632\` | Best validation checkpoint from the final training run; retained for comparison |
| \`models/demo/connector.pt\` | \`7834df07b661da320905743aa2660f33000214c7654df3465c95258b498a21fe\` | Final epoch \`last.pt\`; this is the checkpoint actually exported for the current release |
| \`models/demo/connector_rv1106.rknn\` | \`3214cde4c71bf2bf1d158f3cc83047c024d85625ff7a9b1559e1fe6f917b98ca\` | RV1106 INT8 model |
| \`models/demo/RK_anchors.txt\` | \`f55a69db7c60b6238794beaee38269ff864ba94abae6952b42e5e59235693acf\` | YOLOv5 anchors |

The \`.pt\` file in \`models/demo/connector.pt\` matches the final run's \`last.pt\`, not its \`best.pt\`. The best checkpoint is retained separately so that it can be compared in a future board-side evaluation. Do not silently substitute one for the other.

## Reproducing the final fine-tuning procedure

The canonical training data is now self-contained in \`training/dataset/\`. Its image and label files were checked against the files used by the final training run.

- Training manifest: 33 entries, 29 unique images.
- Validation set: 6 images and 6 labels.
- \`exposed_side.png\` and \`bg_PTBE4p_02.png\` are each deliberately listed three times to oversample those examples.
- \`bg_PTBE4p_02.txt\` is intentionally empty: it is a negative/background image with no target box.
- \`training/dataset/SHA256SUMS\` records the canonical dataset contents.

The initial checkpoint \`training/fit15_start.pt\` is the exact checkpoint used by the final run. The hyperparameters in \`training/hyp.yaml\` match the final run.

The effective training configuration was:

| Parameter | Value |
|---|---|
| YOLOv5 repository revision | \`d25a07534c14f44296f9444bab2aa5c601cdaaab\` |
| Epochs | 10 |
| Batch size | 2 |
| Image size | 640 |
| Optimizer | Adam |
| Device | CPU |
| Workers | 0 |
| Image cache | RAM |
| Nominal batch size | Equal to batch size; one optimizer update per batch |
| Optimizer updates | 150 |

The final run used the upstream YOLOv5 \`train.py\` with one intentional change: \`nbs = batch_size\` instead of \`nbs = 64\`. The saved experimental training copy also called a counter after each optimizer update; that counter did not change training behavior. \`training/finetune.py\` applies the same effective optimizer setting to the pinned upstream source.

Run the training command from the repository root, with the pinned YOLOv5 environment active:

\`\`\`bash
source yolov5-rockchip/.venv/bin/activate
YOLOV5_DIR="$PWD/yolov5-rockchip" \
  python training/finetune.py "$PWD/runs/train/reproduced"
\`\`\`

The output directory must not already exist. Training writes \`fit/weights/best.pt\` and \`fit/weights/last.pt\`.

To export the same checkpoint selection as the current release, export \`last.pt\`:

\`\`\`bash
bash training/export.sh \
  "$PWD/runs/train/reproduced/fit/weights/last.pt" \
  "$PWD/connector_export.reproduced"
\`\`\`

The command reproduces the training procedure and checkpoint selection. Bit-for-bit identical weights are not guaranteed across different host environments or library builds.

## Training metrics and board evaluation

The retained files are:

- \`training.csv\`: epoch-by-epoch metrics from the final training run.
- \`optimizer-steps.txt\`: confirms 150 optimizer updates.
- \`training/train.log\`: full training log.
- training/opt.yaml: resolved YOLOv5 training options with repository-relative paths; training/opt.original.yaml preserves the original absolute-path record.
- \`training/*.png\`: training curves and confusion matrix.
- \`board/board_eval.log\`: batch board-side replay summary.
- \`board/replay_logs/\`: per-image board-side inference logs.
- \`board/export.log\` and \`board/quantize.log\`: ONNX export and RKNN quantization logs.
- \`board-boxes.csv\`: structured board-side box comparison.

The best row in the final run's \`results.csv\` was epoch 3 (precision 0.7278, recall 1.0000, mAP@0.5 0.8712, mAP@0.5:0.95 0.3421). The final epoch 9 row was precision 0.8221, recall 0.5993, mAP@0.5 0.7612, mAP@0.5:0.95 0.3084. The deployed checkpoint is nevertheless \`last.pt\`, because the current board-side release evaluation was performed with that checkpoint.

Board-side replay remains mixed: the new training pair and same-burst samples passed without extra boxes; the refocus group detected 1 of 2; the old validation group detected 5 of 6 but produced 6 extra boxes; both hold-state groups produced extra detections. The live and scene checks are recorded separately in \`live.log\` and \`scenes.csv\`.

These results are development evidence, not a production accuracy claim. The validation set is small and contains closely related captures; more independent scenes are required before claiming generalization.
