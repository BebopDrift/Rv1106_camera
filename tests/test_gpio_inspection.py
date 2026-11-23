"""Host-only checks for the GPIO pipeline's image and result boundaries."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

SRC = Path(__file__).resolve().parents[1] / "src" / "connector"


class GPIOInspection(unittest.TestCase):
    def test_nv12_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exe, shot, rgb = (root / name for name in ("convert", "in.nv12", "out.rgb"))
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Werror",
                            SRC / "nv12_to_rgb.c", "-o", exe], check=True)
            w, h = 2304, 1296
            # Four gray luma bands and two chroma halves. Right side is red.
            with shot.open("wb") as f:
                for luma in (0, 76, 180, 255):
                    f.write(bytes([luma]) * (w*h//4))
                uv_line = bytes([128, 128]) * (w//4) + bytes([85, 255]) * (w//4)
                f.write(uv_line * (h//2))
            subprocess.run([exe, shot, rgb], check=True, stdout=subprocess.PIPE)
            out = rgb.read_bytes()
            self.assertEqual(len(out), 640*640*3)
            self.assertEqual(out[:140*640*3], bytes([114])*(140*640*3))
            self.assertEqual(out[500*640*3:], bytes([114])*(140*640*3))

            def pixel(x, y):
                start = (y*640+x)*3
                return tuple(out[start:start+3])

            self.assertEqual(pixel(80, 140), (0, 0, 0))
            self.assertEqual(pixel(80, 250), (76, 76, 76))
            self.assertEqual(pixel(80, 499), (255, 255, 255))
            self.assertEqual(pixel(500, 250), (254, 0, 0))
            rgb.unlink()
            shot.write_bytes(b"short capture")
            self.assertNotEqual(subprocess.run([exe, shot, rgb], capture_output=True).returncode, 0)
            self.assertFalse(rgb.exists())

    def test_decision(self):
        def result(classes, kept=None, pass_seen=True):
            lines = [f"DECODE scanned=25200 above={len(classes)} kept={len(classes) if kept is None else kept}",
                     "BEST_RAW class=0 score=0.05096139 xyxy=0,0,10,10"]
            lines += [f"DETECTION class={c} score=0.52 xyxy=1,2,30,40" for c in classes]
            if pass_seen:
                lines += ["PASS native one-frame inference"]
            return subprocess.run(["awk", "-v", "ids=gpio_seq=7 frame_seq=29", "-f",
                                   SRC / "inspection_result.awk"], input="\n".join(lines)+"\n",
                                  capture_output=True, text=True)
        for classes, expected in (([], "state=unknown reason=no_detection"),
                                  ([0], "state=plug_inserted"), ([1], "state=pins_exposed"),
                                  ([0, 1], "state=unknown reason=conflicting_classes")):
            completed = result(classes)
            self.assertEqual(completed.returncode, 0)
            self.assertIn("gpio_seq=7 frame_seq=29", completed.stdout)
            self.assertIn(expected, completed.stdout)
        for invalid in (result([0], kept=2), result([], pass_seen=False), result([3])):
            self.assertNotEqual(invalid.returncode, 0)
            self.assertNotIn("INSPECT", invalid.stdout)


if __name__ == "__main__":
    unittest.main()
