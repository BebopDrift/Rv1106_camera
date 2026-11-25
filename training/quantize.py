from pathlib import Path
import cv2
import numpy as np
from rknn.api import RKNN
def canvas(p):
    im=cv2.imread(str(p))
    if im is None: raise ValueError(p)
    h,w=im.shape[:2]
    scale=min(640/w,640/h)
    dw,dh=round(w*scale),round(h*scale)
    im=cv2.resize(im,(dw,dh),interpolation=cv2.INTER_LINEAR)
    image=np.full((640,640,3),114,dtype=np.uint8)
    x,y=(640-dw)//2,(640-dh)//2
    image[y:y+dh,x:x+dw]=im
    return image
if __name__=='__main__':
    import argparse
    parser = argparse.ArgumentParser(description="Quantize connector.onnx for RV1106")
    parser.add_argument("export_dir", type=Path)
    out = parser.parse_args().export_dir.resolve()
    manifest = Path(__file__).resolve().parents[1] / "models/demo/calibration.txt"
    cal = out / "calibration"
    cal.mkdir(exist_ok=True)
    lines=[]
    for i,source in enumerate(manifest.read_text().splitlines()):
        name=cal/f'{i:02}.png'
        assert cv2.imwrite(str(name),canvas(manifest.parent/source))
        lines.append(str(name))
    assert len(lines)==13
    dataset=out/'calibration.txt'
    dataset.write_text('\n'.join(lines)+'\n')
    model=RKNN(verbose=False)
    try:
        for call,args in [
            (model.config,dict(target_platform='rv1106',mean_values=[[0,0,0]],std_values=[[255,255,255]])),
            (model.load_onnx,dict(model=str(out/'connector.onnx'))),
            (model.build,dict(do_quantization=True,dataset=str(dataset))),
            (model.export_rknn,dict(export_path=str(out/'connector_rv1106.rknn')))]:
            rc=call(**args)
            if rc!=0: raise RuntimeError((call.__name__,rc))
    finally:
        model.release()
