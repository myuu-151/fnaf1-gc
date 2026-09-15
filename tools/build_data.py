"""Builds the SD card folder for the GameCube FNAF1 port.

Reads the original game's images and sounds from source/resources (the HTML5
export) and writes sd/, ready to copy to the root of an SD card:

  sd/FNAF1.dol                      (copied from game/Build after `make`)
  sd/FNAF1/FNAF1.octp               project file Octave loads at boot
  sd/FNAF1/AssetRegistry.txt        engine assets only (the game's data isn't Octave assets)
  sd/FNAF1/Data/manifest.txt        frame counts
  sd/FNAF1/Data/img/*.jpg           backgrounds: baseline 4:2:0 JPEG, decoded to YUV on the GameCube
  sd/FNAF1/Data/spr/*.rgx           sprites with alpha: raw GX_TF_RGBA8 texels
  sd/FNAF1/Data/snd/*.pcm           sounds: 16-bit little-endian mono PCM, 22050 Hz
  sd/Engine/...                     cooked GameCube engine assets (from a packaged Octave project)
"""

import os
import shutil
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "source", "resources")
OUT = os.path.join(ROOT, "sd")
DATA = os.path.join(OUT, "FNAF1", "Data")
FFMPEG = os.environ.get("OCTAVE_FFMPEG", r"C:\Users\NoSig\Documents\octave-libogc\External\ffmpeg\bin\ffmpeg.exe")
ENGINE_PACKAGE = os.environ.get("OCTAVE_GCN_PACKAGE", r"C:\Users\NoSig\Documents\testproj\Packaged\GameCube")
DOL = os.path.join(ROOT, "game", "Build", "FNAF1.dol")

# Backgrounds are 1600x720 in the original; both sizes must be multiples of 16 for the decoder.
BG_SIZE = (992, 448)
STATIC_SIZE = (320, 176)
JPEG_QUALITY = 85

# Sprites are stored smaller than the 720-line original and stretched when drawn.
DOOR_HEIGHT = 224
BUTTON_SCALE = 448 / 720.0
FLIP_SIZE = (256, 144)

BACKGROUNDS = {
    # office (lights: one side at a time)
    "office": 39, "office_light_l": 58, "office_light_r": 127,
    "office_bonnie": 225, "office_chica": 227, "office_dark": 521,
    # cameras
    "cam1a_all": 2, "cam1a_no_bonnie": 68, "cam1a_no_chica": 223, "cam1a_freddy": 224,
    "cam1b_empty": 48, "cam1b_bonnie": 90, "cam1b_chica": 215,
    "cam1c": 66,
    "cam5_empty": 83, "cam5_bonnie": 205,
    "cam7_empty": 41, "cam7_chica": 217,
    "cam2a_empty": 44, "cam2a_bonnie": 241,
    "cam3_empty": 62, "cam3_bonnie": 190,
    "cam2b_empty": 0, "cam2b_bonnie": 188,
    "cam4a_empty": 67, "cam4a_chica": 221,
    "cam4b_empty": 49, "cam4b_chica": 220,
}

JUMPSCARES = {
    "bonnie": [291] + list(range(293, 302)) + [303],
    "chica": list(range(228, 238)) + [239],
    "freddy": list(range(485, 519, 2)),
}
STATIC_FRAMES = [12, 13, 14, 15, 16, 17, 18, 20]
FLIP_FRAMES = [142, 46, 144, 132, 133, 136, 137, 138, 139, 140]

SOUNDS = {
    # name: (sound number, max seconds or None)
    "fan": (2, None),            # Buzz_Fan_Florescent2
    "light": (1, None),          # BallastHumMedium2
    "door": (3, None),           # SFXBible_12478
    "blip": (11, None),          # blip3
    "tablet": (7, None),         # put down
    "scream": (15, None),        # XSCREAM
    "windowscare": (31, None),
    "chimes": (32, 8.0),         # chimes 2
    "steps": (9, None),          # deep steps
    "powerdown": (26, 6.0),
}


def src_png(number):
    return os.path.join(SRC, "%04d.png" % number)


def save_jpeg(number, name, size):
    im = Image.open(src_png(number)).convert("RGB").resize(size, Image.LANCZOS)
    im.save(os.path.join(DATA, "img", name + ".jpg"), "JPEG", quality=JPEG_QUALITY,
            subsampling="4:2:0", optimize=False, progressive=False)


def save_rgx(im, name):
    """Writes RGBA pixels as GX_TF_RGBA8 texels: 4x4 blocks of 16 (A,R) pairs then 16 (G,B) pairs."""
    im = im.convert("RGBA")
    w, h = im.size
    assert w % 4 == 0 and h % 4 == 0, (name, im.size)
    px = im.load()
    out = bytearray(b"RGX8" + struct.pack(">HH", w, h))
    for by in range(h // 4):
        for bx in range(w // 4):
            ar = bytearray(32)
            gb = bytearray(32)
            for py in range(4):
                for pxx in range(4):
                    r, g, b, a = px[bx * 4 + pxx, by * 4 + py]
                    i = (py * 4 + pxx) * 2
                    ar[i], ar[i + 1] = a, r
                    gb[i], gb[i + 1] = g, b
            out += ar + gb
    with open(os.path.join(DATA, "spr", name + ".rgx"), "wb") as f:
        f.write(out)


def round4(v):
    return max(4, int(round(v / 4.0)) * 4)


def find_sprites(sheet):
    """Bounding boxes of opaque regions in a texture atlas (transparent or magenta background)."""
    im = Image.open(os.path.join(SRC, sheet + ".png")).convert("RGBA")
    w, h = im.size
    px = im.load()

    def bg(x, y):
        r, g, b, a = px[x, y]
        return a < 16 or (r > 240 and g < 16 and b > 240)

    seen = bytearray(w * h)
    boxes = []
    for y in range(h):
        for x in range(w):
            if seen[y * w + x] or bg(x, y):
                continue
            stack = [(x, y)]
            seen[y * w + x] = 1
            x0 = x1 = x
            y0 = y1 = y
            while stack:
                cx, cy = stack.pop()
                x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and not bg(nx, ny):
                        seen[ny * w + nx] = 1
                        stack.append((nx, ny))
            boxes.append((x0, y0, x1 + 1, y1 + 1))
    return im, boxes


def opacity(im):
    a = im.getchannel("A").resize((64, 128))
    return sum(1 for v in a.getdata() if v > 128) / float(64 * 128)


def build_doors():
    # The door close animations live in atlases. Two sets, told apart by width:
    # 223 px (left door) and 229 px (right door). Frames are ordered by how much of
    # the doorway they cover.
    frames = {223: [], 229: []}
    for sheet in ["M0003", "M0004", "M0006", "M0007", "M0008", "M0009", "M0010"]:
        im, boxes = find_sprites(sheet)
        for (x0, y0, x1, y1) in boxes:
            w, h = x1 - x0, y1 - y0
            if h < 600 or w not in frames:
                continue
            spr = im.crop((x0, y0, x1, y1))
            frames[w].append((opacity(spr), spr))
    counts = {}
    for width, side in ((223, "l"), (229, "r")):
        ordered = []
        for op, spr in sorted(frames[width], key=lambda t: t[0]):
            if ordered and abs(ordered[-1][0] - op) < 0.004:
                continue
            ordered.append((op, spr))
        size = (round4(width * DOOR_HEIGHT / 720.0), DOOR_HEIGHT)
        for i, (op, spr) in enumerate(ordered):
            save_rgx(spr.resize(size, Image.LANCZOS), "door_%s_%02d" % (side, i))
        counts["door_" + side] = len(ordered)
        print("door %s: %d frames at %dx%d" % (side, len(ordered), size[0], size[1]))
    return counts


def build_buttons():
    # Door/light button panels (about 57x172). Green door button = door closed,
    # bright light button = light on.
    found = {}
    for sheet in ["M0004", "M0003"]:
        im, boxes = find_sprites(sheet)
        for (x0, y0, x1, y1) in boxes:
            w, h = x1 - x0, y1 - y0
            if not (48 <= w <= 72 and 155 <= h <= 190):
                continue
            spr = im.crop((x0, y0, x1, y1)).convert("RGBA")
            top = spr.crop((0, int(h * 0.1), w, int(h * 0.4))).convert("RGB").resize((1, 1)).getpixel((0, 0))
            bottom = spr.crop((0, int(h * 0.6), w, int(h * 0.9))).convert("RGB").resize((1, 1)).getpixel((0, 0))
            closed = 1 if top[1] > top[0] else 0
            light = 1 if sum(bottom) / 3.0 > 90 else 0
            key = "btn_c%d_l%d" % (closed, light)
            if key not in found:
                found[key] = spr
                size = (round4(w * BUTTON_SCALE), round4(h * BUTTON_SCALE))
                save_rgx(spr.resize(size, Image.LANCZOS), key)
                print("%s from %s at (%d,%d) %dx%d -> %dx%d" % (key, sheet, x0, y0, w, h, size[0], size[1]))
    missing = [k for k in ("btn_c0_l0", "btn_c0_l1", "btn_c1_l0", "btn_c1_l1") if k not in found]
    if missing:
        sys.exit("button panels not found: %s" % missing)


def build_sounds():
    for name, (number, max_seconds) in SOUNDS.items():
        src = os.path.join(SRC, "%04d.ogg" % number)
        dst = os.path.join(DATA, "snd", name + ".pcm")
        cmd = [FFMPEG, "-v", "error", "-y", "-i", src, "-ac", "1", "-ar", "22050"]
        if max_seconds:
            cmd += ["-t", str(max_seconds)]
        cmd += ["-f", "s16le", dst]
        subprocess.run(cmd, check=True)
        print("sound %s: %d KB" % (name, os.path.getsize(dst) // 1024))


def build_engine_files():
    shutil.copytree(os.path.join(ENGINE_PACKAGE, "Engine"), os.path.join(OUT, "Engine"), dirs_exist_ok=True)
    registry = []
    for sub in os.listdir(ENGINE_PACKAGE):
        path = os.path.join(ENGINE_PACKAGE, sub, "AssetRegistry.txt")
        if os.path.isfile(path):
            registry = [l.strip() for l in open(path) if ",Engine/" in l]
            break
    if not registry:
        sys.exit("no AssetRegistry.txt found under " + ENGINE_PACKAGE)
    with open(os.path.join(OUT, "FNAF1", "AssetRegistry.txt"), "w", newline="\n") as f:
        f.write("\n".join(registry) + "\n")
    with open(os.path.join(OUT, "FNAF1", "FNAF1.octp"), "w", newline="\n") as f:
        f.write("name=FNAF1\n")


def main():
    for sub in ("img", "spr", "snd"):
        os.makedirs(os.path.join(DATA, sub), exist_ok=True)

    build_engine_files()

    for name, number in BACKGROUNDS.items():
        save_jpeg(number, name, BG_SIZE)
    counts = {}
    for who, numbers in JUMPSCARES.items():
        numbers = [n for n in numbers if os.path.exists(src_png(n))]
        for i, n in enumerate(numbers):
            save_jpeg(n, "jump_%s_%02d" % (who, i), BG_SIZE)
        counts["jump_" + who] = len(numbers)
    for i, n in enumerate(STATIC_FRAMES):
        save_jpeg(n, "static_%02d" % i, STATIC_SIZE)
    counts["static"] = len(STATIC_FRAMES)

    for i, n in enumerate(FLIP_FRAMES):
        save_rgx(Image.open(src_png(n)).resize(FLIP_SIZE, Image.LANCZOS), "flip_%02d" % i)
    counts["flip"] = len(FLIP_FRAMES)

    counts.update(build_doors())
    build_buttons()
    build_sounds()

    with open(os.path.join(DATA, "manifest.txt"), "w", newline="\n") as f:
        for key in sorted(counts):
            f.write("%s %d\n" % (key, counts[key]))

    if os.path.exists(DOL):
        shutil.copy2(DOL, os.path.join(OUT, "FNAF1.dol"))
        print("copied", DOL)
    else:
        print("note: %s not built yet (run make in game/)" % DOL)

    total = 0
    for dirpath, _, files in os.walk(os.path.join(OUT, "FNAF1")):
        total += sum(os.path.getsize(os.path.join(dirpath, f)) for f in files)
    print("counts:", counts)
    print("FNAF1 data: %.1f MB" % (total / 1e6))


if __name__ == "__main__":
    main()
