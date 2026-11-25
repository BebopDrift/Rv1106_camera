"""Train the demo candidate from the preserved fit15 checkpoint."""
import os
import subprocess
import sys
from pathlib import Path

import yaml

root = Path(__file__).resolve().parents[1]
repo = Path(os.environ.get("YOLOV5_DIR", root / "yolov5-rockchip")).resolve()
revision = subprocess.check_output(["git", "-C", repo, "rev-parse", "HEAD"], text=True).strip()
if revision != "d25a07534c14f44296f9444bab2aa5c601cdaaab":
    raise SystemExit("YOLOV5_DIR must point to the revision listed in README.md")
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
data = root / "training/dataset"
config = {"path": str(data), "train": str(data / "train.txt"), "val": "images/val",
          "nc": 2, "names": ["plug_inserted", "pins_exposed"]}
(out / "data.yaml").write_text(yaml.safe_dump(config))
trainer = repo / "train.py"
source = trainer.read_text()
# The small-batch run used one optimizer update per batch.
original = "nbs = 64  # nominal batch size"
if source.count(original) != 1:
    raise SystemExit("Unexpected upstream train.py")
source = source.replace(original, "nbs = batch_size  # nominal batch size")
sys.path.insert(0, str(repo))
os.chdir(repo)
sys.argv = [str(trainer), "--weights", str(root / "training/fit15_start.pt"),
            "--data", str(out / "data.yaml"), "--hyp", str(root / "training/hyp.yaml"),
            "--epochs", "10", "--batch-size", "2", "--imgsz", "640", "--optimizer", "Adam",
            "--device", "cpu", "--workers", "0", "--cache", "ram",
            "--project", str(out), "--name", "fit"]
exec(compile(source, str(trainer), "exec"), {"__name__": "__main__", "__file__": str(trainer)})
