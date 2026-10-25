import glob
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1]
video = "--no-video" not in sys.argv
try:
    FONT = ImageFont.truetype("consola.ttf", 18)
    SMALL = ImageFont.truetype("consola.ttf", 13)
except OSError:
    FONT = SMALL = ImageFont.load_default()
DW, DH = 480, 270
MAX_FLAG_TILES = 200

def load_dump(side, frame):
    for cand in (frame + 1, frame, frame + 2):
        p = os.path.join(out, "ex" + side, f"explorer_{cand:05}.json")
        if os.path.exists(p):
            try:
                j = json.load(open(p))
            except ValueError:
                continue
            if j.get("copyFrame") == frame or cand == frame + 2:
                return j
    return None

def remote_on_screen(j, w, h):
    if j:
        for c in j["copies"]:
            if c["kind"] == "skin" and c["screen"]:
                return c["screen"][0] * w, c["screen"][1] * h
    return None

class Sheet:

    def __init__(self, prefix, cols, rows, tw, th):
        self.prefix, self.cols, self.rows, self.tw, self.th = prefix, cols, rows, tw, th
        self.n = 0
        self.img = None

    def add(self, tile):
        k = self.n % (self.cols * self.rows)
        if k == 0:
            self.flush()
            self.img = Image.new("RGB", (self.cols * self.tw, self.rows * self.th))
        self.img.paste(tile, ((k % self.cols) * self.tw, (k // self.cols) * self.th))
        self.n += 1

    def flush(self):
        if self.img is not None:
            self.img.save(f"{self.prefix}_{(self.n - 1) // (self.cols * self.rows):02}.png")
            self.img = None

def small_of(img):
    return np.asarray(img.resize((DW, DH), Image.BILINEAR), dtype=np.int16)

report = []
for side in ("A", "B"):
    files = sorted(glob.glob(os.path.join(out, side, "shot_*.png")))
    if not files:
        continue
    frames = [int(f[-9:-4]) for f in files]
    W, H = Image.open(files[0]).size
    sx, sy = W / DW, H / DH
    flags = []
    flagSheet = Sheet(os.path.join(out, f"flags_{side}"), 1, 8, 1200, 300)
    remoteSheet = Sheet(os.path.join(out, f"remote_{side}"), 10, 6, 280, 240)
    fullSheet = Sheet(os.path.join(out, f"full_{side}"), 6, 8, 366, 206)
    win = []
    flag_tiles = 0

    def check(prev, cur, nxt):
        global flag_tiles
        i, img, b = cur
        fr = frames[i]
        a, c = prev[2], nxt[2]
        if np.abs(b - a).mean() < 0.05:
            flags.append((fr, "repeat", "same picture as the frame before (stall)"))
        la, lb, lc = a.mean(), b.mean(), c.mean()
        if abs(lb - la) > 6 and abs(lb - lc) > 6:
            flags.append((fr, "brightness", f"{la:.0f} -> {lb:.0f} -> {lc:.0f}"))
        d1 = np.abs(b - a).sum(axis=2)
        d2 = np.abs(b - c).sum(axis=2)
        d3 = np.abs(a - c).sum(axis=2)
        m = (np.minimum(d1, d2) > 90) & (d3 < 15)
        m[: int(DH * 0.12), : int(DW * 0.25)] = False
        m[int(DH * 0.88):, int(DW * 0.85):] = False
        m[: int(DH * 0.3), int(DW * 0.82):] = False
        if m.sum() < 4:
            return
        rem = remote_on_screen(load_dump(side, fr), W, H)
        ys, xs = np.nonzero(m)
        cells = {}
        for y, x in zip(ys, xs):
            cells.setdefault((y // 16, x // 16), []).append((y, x))
        for pts in cells.values():
            if len(pts) < 10:
                continue
            py = np.array([p[0] for p in pts]); px = np.array([p[1] for p in pts])
            box = (int(px.min() * sx), int(py.min() * sy), int((px.max() + 1) * sx), int((py.max() + 1) * sy))
            cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
            near = np.hypot(cx - rem[0], cy - rem[1]) if rem else 1e9
            kind = "flicker" if near < 260 else "stray"
            flags.append((fr, kind, f"{len(pts)} px at {box}" + (f", {near:.0f} px from the other Link" if rem else "")))
            if flag_tiles >= MAX_FLAG_TILES:
                continue
            flag_tiles += 1
            crop = (max(0, cx - 200), max(0, cy - 150), max(0, cx - 200) + 400, max(0, cy - 150) + 300)
            row = Image.new("RGB", (1200, 300))
            for k, (fi, fimg) in enumerate(((prev[0], prev[1]), (i, img), (nxt[0], nxt[1]))):
                t = fimg.crop(crop)
                d = ImageDraw.Draw(t)
                if k == 1:
                    d.rectangle((box[0] - crop[0], box[1] - crop[1], box[2] - crop[0], box[3] - crop[1]), outline=(255, 0, 255), width=2)
                d.text((4, 4), f"{frames[fi]}{'  ' + kind if k == 1 else ''}", fill=(255, 255, 0), font=FONT)
                row.paste(t, (k * 400, 0))
            flagSheet.add(row)

    for i, f in enumerate(files):
        img = Image.open(f).convert("RGB")
        t = img.resize((366, 206))
        ImageDraw.Draw(t).text((3, 2), str(frames[i]), fill=(255, 255, 0), font=SMALL)
        fullSheet.add(t)
        rem = remote_on_screen(load_dump(side, frames[i]), W, H)
        if rem and 0 <= rem[0] < W and 0 <= rem[1] < H:
            cx, cy = int(rem[0]), int(rem[1])
            t = img.crop((cx - 210, cy - 300, cx + 210, cy + 60)).resize((280, 240))
        else:
            t = img.resize((280, 158)).crop((0, -41, 280, 199))
        ImageDraw.Draw(t).text((3, 2), str(frames[i]), fill=(255, 255, 0), font=SMALL)
        remoteSheet.add(t)
        win.append((i, img, small_of(img)))
        if len(win) == 3:
            check(*win)
            win.pop(0)
    for s in (flagSheet, remoteSheet, fullSheet):
        s.flush()
    report.append(f"== {('host' if os.path.isdir(os.path.join(out, 'B')) else 'game') if side == 'A' else 'guest'} ({side}): {len(files)} frames {frames[0]}-{frames[-1]}")
    missing = sorted(set(range(frames[0], frames[-1] + 1)) - set(frames))
    if missing:
        report.append(f"   MISSING frames: {missing}")
    counts = {}
    for fr, kind, note in flags:
        counts[kind] = counts.get(kind, 0) + 1
        report.append(f"   {fr} {kind:10} {note}")
    report.append(f"   totals: {counts}")

open(os.path.join(out, "report.txt"), "w").write("\n".join(report) + "\n")
print("\n".join(line for line in report if line.startswith("==") or "totals" in line or "MISSING" in line))

if video:
    a = sorted(glob.glob(os.path.join(out, "A", "shot_*.png")))
    vid = os.path.join(out, "frames_sbs")
    os.makedirs(vid, exist_ok=True)
    for k, fa in enumerate(a):
        fb = os.path.join(out, "B", os.path.basename(fa))
        if os.path.isdir(os.path.join(out, "B")):
            f = Image.new("RGB", (1920, 540))
            f.paste(Image.open(fa).convert("RGB").resize((960, 540)), (0, 0))
            if os.path.exists(fb):
                f.paste(Image.open(fb).convert("RGB").resize((960, 540)), (960, 0))
            d = ImageDraw.Draw(f)
            d.text((8, 6), f"HOST {fa[-9:-4]}", fill=(255, 255, 0), font=FONT)
            d.text((968, 6), f"GUEST {fa[-9:-4]}", fill=(255, 255, 0), font=FONT)
        else:
            f = Image.open(fa).convert("RGB").resize((1280, 720))
            ImageDraw.Draw(f).text((8, 6), fa[-9:-4], fill=(255, 255, 0), font=FONT)
        f.save(os.path.join(vid, f"{k:05}.png"))
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "60", "-i", os.path.join(vid, "%05d.png"),
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", os.path.join(out, "video.mp4")], check=False)
    print("video:", os.path.join(out, "video.mp4"))
