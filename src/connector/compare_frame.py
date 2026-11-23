"""Compare native C detections against the YOLO annotation for the exact input frame."""
import argparse
import json
import math
import re
from pathlib import Path

NAMES = ("plug_inserted", "pins_exposed")
BOX = re.compile(r"(BEST_RAW|DETECTION) class=([01]) score=(\S+) xyxy=(\S+)")


def original_box(coords, frame):
    width, height = frame["width"], frame["height"]
    w, h = frame["resized_width"], frame["resized_height"]
    x, y = frame["left"], frame["top"]
    return tuple(max(0, min(limit, (value - pad) * limit / used))
                 for value, limit, pad, used in zip(coords, (width, height)*2, (x, y)*2, (w, h)*2))


def overlap(a, b):
    w = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    h = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    intersection = w*h
    union = (a[2]-a[0])*(a[3]-a[1]) + (b[2]-b[0])*(b[3]-b[1]) - intersection
    return intersection / union if union > 0 else 0.0


def compare(frame, lines, label=None):
    image = Path(frame["image"])
    if label is None:
        if image.parent.parent.name != "images":
            raise ValueError("set CONNECTOR_LABEL for images outside images/<split>/")
        label = (image.parents[2] / "labels" / image.parent.name / image.name).with_suffix(".txt")
    rows = [line.split() for line in label.read_text().splitlines() if line.strip()]
    if len(rows) != 1 or len(rows[0]) != 5 or rows[0][0] not in ("0", "1"):
        raise ValueError(f"{label}: expected one connector annotation (class cx cy w h)")
    cls = int(rows[0][0])
    cx, cy, bw, bh = map(float, rows[0][1:])
    if not all(0 <= v <= 1 for v in (cx, cy, bw, bh)) or bw <= 0 or bh <= 0:
        raise ValueError(f"{label}: invalid normalized coordinates")
    w, h = frame["width"], frame["height"]
    truth = (max(0, (cx-bw/2)*w), max(0, (cy-bh/2)*h),
             min(w, (cx+bw/2)*w), min(h, (cy+bh/2)*h))
    print(f"GT image={image.name} class={cls}({NAMES[cls]}) xyxy={fmt(truth)}")
    best_same, kept, have_raw = 0.0, 0, False
    reported = None
    for line in lines:
        if line.startswith("DECODE "):
            parts = dict(kv.split("=", 1) for kv in line.split()[1:])
            if int(parts["scanned"]) != 25200:
                raise ValueError("incomplete head traversal")
            reported = int(parts["kept"])
        if not line.startswith(("BEST_RAW ", "DETECTION ")):
            continue
        match = BOX.fullmatch(line.strip())
        if not match:
            raise ValueError(f"invalid box line: {line}")
        kind, predicted, score, coords = match.groups()
        coords = tuple(map(float, coords.split(",")))
        if len(coords) != 4 or not all(map(math.isfinite, (*coords, float(score)))):
            raise ValueError(f"non-finite or incomplete box: {line}")
        box = original_box(coords, frame)
        iou = overlap(box, truth)
        print(f"{kind}_IMAGE class={predicted}({NAMES[int(predicted)]}) score={score} "
              f"xyxy={fmt(box)} iou={iou:.4f}")
        if kind == "BEST_RAW":
            have_raw = True
        else:
            kept += 1
            if int(predicted) == cls:
                best_same = max(best_same, iou)
    if not have_raw or reported != kept or "PASS native one-frame inference" not in lines:
        raise ValueError("incomplete native decode log")
    print(f"COMPARE detections={kept} max_same_class_iou={best_same:.4f} "
          f"match={'yes' if best_same >= 0.5 else 'no'}")
    return best_same >= 0.5


def fmt(coords):
    return ",".join(f"{v:.1f}" for v in coords)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("meta", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("label", type=Path, nargs="?")
    args = parser.parse_args()
    compare(json.loads(args.meta.read_text()), args.log.read_text().splitlines(), args.label)
