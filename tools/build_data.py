"""Converts the original game's images and sounds into FNAF1/Scripts/Data.

The Octave packager copies the project's Scripts folder (subfolders included) into
the package and the ISO, and only runs .lua files from it, so the game data rides
along there:

  FNAF1/Scripts/Data/manifest.txt     frame counts
  FNAF1/Scripts/Data/img/*.jpg        backgrounds: baseline 4:2:0 JPEG, decoded to YUV on the GameCube
  FNAF1/Scripts/Data/spr/*.rgx        sprites with alpha: raw GX_TF_RGBA8 texels
  FNAF1/Scripts/Data/snd/*.pcm        sounds: 16-bit little-endian mono PCM, 22050 Hz

Then package with Octave: Octave.exe -headless -project FNAF1/FNAF1.octp -build GameCube
"""

import math
import os
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "source", "resources")
DATA = os.path.join(ROOT, "FNAF1", "Scripts", "Data")
FFMPEG = os.environ.get("OCTAVE_FFMPEG", r"C:\Users\NoSig\Documents\octave-libogc\External\ffmpeg\bin\ffmpeg.exe")

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
    "office_bonnie": 225, "office_chica": 227,
    # power out: dark office, and Freddy's face lit in the left doorway (flickers with the music box)
    "office_dark": 304, "office_freddy_dark": 305,
    # cameras
    # Show Stage: 19 = normal pose (2 is the rare everyone-stares frame); alone, Freddy faces forward (224; 355 is him staring).
    "cam1a_all": 19, "cam1a_no_bonnie": 68, "cam1a_no_chica": 223, "cam1a_freddy": 224,
    "cam1a_freddy_stare": 355,      # rare variant ("random for pic" in the original)
    # Bonnie and Chica have two poses on some cameras (the original re-rolls it on every move):
    # 1B Bonnie 90 / 120, 1B Chica 222 / 215, 4A Chica 221 / 226, 7 Chica 217 / 219.
    "cam1b_empty": 48, "cam1b_bonnie": 90, "cam1b_bonnie2": 120, "cam1b_chica": 215, "cam1b_chica2": 222,
    "cam1c_0": 66, "cam1c_1": 211, "cam1c_2": 338, "cam1c_3": 240,   # Pirate Cove: closed, peeking, stepping out, gone
    "cam5_empty": 83, "cam5_bonnie": 205,
    "cam5_rare": 354, "cam5_bonnie_stare": 555,     # "random for pic" <= 5 empty, <= 10 with Bonnie
    "cam7_empty": 41, "cam7_chica": 217, "cam7_chica2": 219,
    # West Hall: dark (43), and lit on the light's flicker frames, empty (44) or with Bonnie (206)
    "cam2a_dark": 43, "cam2a_empty": 44, "cam2a_bonnie": 206,     # (241/244/340 are Foxy running)
    "cam3_empty": 62, "cam3_bonnie": 190,
    "cam2b_empty": 0, "cam2b_bonnie": 188,
    "cam4a_empty": 67, "cam4a_chica": 221, "cam4a_chica2": 226,
    "cam4b_empty": 49, "cam4b_chica": 220,
    # rare posters on empty cameras ("random for pic" in the original)
    "cam2b_rare_freddy": 571,       # (540, the golden Freddy poster, is only for his rare event)
    "cam4b_rare_news0": 549, "cam4b_rare_news1": 550, "cam4b_rare_news2": 551, "cam4b_rare_news3": 552,
    "cam4a_rare_faces": 546, "cam4a_rare_itsme": 554,
    "cam1c_rare_itsme": 553,
    # Golden Freddy: his poster on CAM 2B once his event is armed, the hallucination flashes
    # (the original's "Active 21": Freddy, IT'S ME, Bonnie, IT'S ME) and his face on the "creepy end" frame
    "cam2b_golden": 540,
    "hallucination_0": 525, "hallucination_1": 543, "hallucination_2": 520, "hallucination_3": 544,
    "golden_end": 548,
    # main menu: Freddy's face (431 normal, 440/441 twitches, 442 the endoskeleton glitch)
    "menu_freddy0": 431, "menu_freddy1": 440, "menu_freddy2": 441, "menu_freddy3": 442,
    "newspaper": 539,               # help-wanted ad shown on New Game
    "gameover": 358,                # after the static: Freddy in the backstage room
}

# Menu text in the texture atlases: (atlas, x, y, w, h). Stored at their size on a 640x480
# screen (the menu is 1280x720 in the original).
MENU_TEXT = {
    # Rows are trimmed to the text: the atlas packs other sprites a few pixels above and below.
    "title": ("M0001", 4, 762, 202, 212),       # Five Nights at Freddy's
    "newgame": ("M0004", 814, 886, 202, 33),
    "continue": ("M0004", 813, 848, 204, 34),
    "arrows": ("M0004", 944, 923, 54, 26),      # >>
    "clock": ("M0005", 7, 726, 224, 31),        # 12:00 AM
    "first": ("M0005", 471, 792, 72, 31),       # 1st
    "night": ("M0005", 572, 792, 125, 31),      # Night
    "copyright": ("M0002", 794, 1000, 224, 14), # (c)2014 Scott Cawthon
    "gameover": ("M0004", 224, 972, 206, 34),   # Game Over
    # 6 AM screen ("next day" frame): the 5 scrolls up out of view and the 6 in, next to "AM"
    "six_5": ("M0003", 406, 608, 53, 72),
    "six_6": ("M0003", 463, 608, 53, 72),
    "six_am": ("M0001", 884, 602, 113, 72),
}
MENU_SCALE = (640 / 1280.0, 480 / 720.0)

# Frame lists copied from the animations in Application.ccj (u16 image numbers, in playback
# order; the file numbering interleaves animations, so it isn't the order).
JUMPSCARES = {
    "bonnie": [301, 291, 303] + list(range(293, 301)),
    "chica": [279, 65, 281, 69, 216] + list(range(228, 238)) + [239],
    "freddy": [326, 307, 348] + list(range(308, 326)),    # power-out: lunging out of the dark (1280x720, drawn full screen)
    "foxy": [413, 242, 415, 243] + list(range(396, 413)) + [412] * 4,     # lunging in from the left doorway, then holds
}
# Foxy running down the West Hall (CAM 2A), far to past the camera.
# Every West Hall frame with Foxy (found by matching the hall's poster wall); the
# numbers interleave with other animations (e.g. 292 and 302 sit inside Bonnie's).
FOXY_RUN = [241, 241, 241, 340] + list(range(244, 251)) + [280] + list(range(282, 291)) + [292, 302, 306, 327] + list(range(329, 338))   # 337 = last frame (empty hall)
STATIC_FRAMES = [12, 13, 14, 15, 16, 17, 18, 20]
FLIP_FRAMES = [142, 46, 144, 132, 133, 136, 137, 138, 139, 140]

SOUNDS = {
    # name: (sound number, max seconds or None)
    "light": (1, None),          # BallastHumMedium2
    "door": (3, None),           # SFXBible_12478
    "blip": (11, None),          # blip3
    "tablet": (7, None),         # put down
    "scream": (15, None),        # XSCREAM
    "windowscare": (31, None),
    "steps": (9, None),          # deep steps
    "run": (10, None),           # run: Foxy down the West Hall (#39; 55 "running fast3" is Freddy's)
    "knock": (39, None),         # DOOR_POUNDING_ME: the faint random knock (channel volume 10)
    "foxybang": (27, None),      # knock2: Foxy banging on the closed left door (loud)
    "honk": (36, None),          # PartyFavorraspyPart_AC01__3: Freddy poster's nose
    "camup": (6, None),          # CAMERA_VIDEO_LOA (raising the tablet)
    "camhum": (8, None),         # COMPUTER_DIGITAL (monitor hum while it's up)
    "garble1": (12, None),       # camera garbles when someone moves on camera
    "garble2": (13, None),
    "garble3": (14, None),
    "pots1": (16, None),         # OVEN-DRA: Chica in the kitchen
    "pots2": (17, None),
    "pots3": (19, None),
    "pots4": (18, 3.0),          # OVEN-DRA_7 (the original's 5 kitchen actions use it twice); trimmed, it's long
    "error": (4, None),          # error: door/light buttons while someone is in the office
    "giggle": (38, None),        # Laugh_Giggle_Girl_1: Golden Freddy's poster on CAM 2B
}

# Long sounds, streamed from the disc at runtime instead of loaded into RAM.
STREAMS = {
    "call": (41, None),          # voiceover1c: the night 1 phone call
    "ambience": (28, None),      # ambience2: replaces the dark ambience when the power runs out
    "eerie": (37, None),         # EerieAmbienceLargeSca: loops all night, volume rises as they get close
    "darkambience": (0, None),   # ColdPresc B: loops from the start of the night
    "powerdown": (26, None),     # power out: 18.7 s, winds down until the music box
    "breath1": (22, None),       # Vocals_Breaths: someone got into the office
    "breath2": (23, None),
    "breath3": (24, None),
    "breath4": (25, None),
    "deadstatic": (20, None),    # static: the game over screen
    "minidv": (5, None),         # MiniDV_Tape_Eject: plays when the cameras open (stereo)
    "musicbox": (30, None),      # music box (power out)
    "menumusic": (35, None),     # darkness music (main menu)
    "menustatic": (34, None),    # static2 (main menu)
    "piratesong": (21, None),    # pirate song2: Foxy humming in Pirate Cove
    "circus": (29, None),        # circus: faint carnival tune, rare, any time of the night
    "fan": (2, None),            # Buzz_Fan_Florescent2 (loops all night)
    "chimes": (32, None),        # chimes 2 (6 AM): its fade-out tail plays until the next frame starts (~10.2 s)
    "cheer": (33, None),         # CROWD_SMALL_CHIL (6 AM)
    "laugh": (56, None),         # Laugh_Giggle_Girl_1d (Freddy, power out)
    "xscream2": (46, None),      # XSCREAM2: Golden Freddy's "creepy end"
    "robotvoice": (40, None),    # robotvoice: plays under the hallucination flashes
}

# Streamed sounds kept in stereo (the original files are stereo; everything else is made mono).
# Streamed sounds kept in stereo. Empty for now: a stereo stream froze Dolphin (the tape sound)
# and crashed hardware right away (the phone call), while mono streams are stable.
STEREO_SOUNDS = set()

# Sample gain for sounds that need to be louder than the mixer's maximum volume (2.0). Empty:
# the original recordings' levels are right.
SOUND_GAIN = {}


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
    return sum(1 for v in a.get_flattened_data() if v > 128) / float(64 * 128)


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
    # Door/light button panels (about 58x174): one set of four per wall. Green door
    # button = door closed, bright light button = light on. The side shows in the
    # buttons' white edge highlight: on the left edge for the left-wall panel, on
    # the right edge for the right-wall panel (checked in Dolphin).
    found = {}
    for sheet in ["M0004", "M0003"]:
        im, boxes = find_sprites(sheet)
        for (x0, y0, x1, y1) in boxes:
            w, h = x1 - x0, y1 - y0
            if not (48 <= w <= 72 and 155 <= h <= 190):
                continue
            spr = im.crop((x0, y0, x1, y1)).convert("RGBA")
            rgb = spr.convert("RGB")
            top = rgb.crop((0, int(h * 0.1), w, int(h * 0.4))).resize((1, 1)).getpixel((0, 0))
            bottom = rgb.crop((0, int(h * 0.6), w, int(h * 0.9))).resize((1, 1)).getpixel((0, 0))
            closed = 1 if top[1] > top[0] else 0
            light = 1 if sum(bottom) / 3.0 > 90 else 0

            # Brightest column band across the door button: highlight on the left or right?
            band = rgb.crop((0, int(h * 0.12), w, int(h * 0.35))).convert("L")
            columns = [sum(band.getpixel((x, y)) for y in range(band.size[1])) for x in range(w)]
            left_peak = max(columns[int(w * 0.1):int(w * 0.35)])
            right_peak = max(columns[int(w * 0.65):int(w * 0.9)])
            side = "r" if right_peak > left_peak else "l"

            key = "btn_%s_c%d_l%d" % (side, closed, light)
            if key not in found:
                found[key] = spr
                size = (round4(w * BUTTON_SCALE), round4(h * BUTTON_SCALE))
                save_rgx(spr.resize(size, Image.LANCZOS), key)
                print("%s from %s at (%d,%d) %dx%d -> %dx%d" % (key, sheet, x0, y0, w, h, size[0], size[1]))
    expected = ["btn_%s_c%d_l%d" % (s, c, l) for s in "lr" for c in (0, 1) for l in (0, 1)]
    missing = [k for k in expected if k not in found]
    if missing:
        sys.exit("button panels not found: %s" % missing)


def build_fan():
    # The office desk fan: three 138x196 frames in M0001, drawn over the office at (780, 303).
    im, boxes = find_sprites("M0001")
    frames = sorted([b for b in boxes if 130 <= b[2] - b[0] <= 145 and 190 <= b[3] - b[1] <= 200], key=lambda b: b[1])
    if len(frames) != 3:
        sys.exit("expected 3 fan frames in M0001, found %d" % len(frames))
    for i, (x0, y0, x1, y1) in enumerate(frames):
        spr = im.crop((x0, y0, x1, y1))
        size = (round4((x1 - x0) * BUTTON_SCALE), round4((y1 - y0) * BUTTON_SCALE))
        save_rgx(spr.resize(size, Image.LANCZOS), "fan_%02d" % i)
    print("fan: %d frames" % len(frames))
    return {"fan": len(frames)}


def build_golden():
    # Golden Freddy slumped in the office (image 573): 541x521 at (2, 2) in atlas M0003, drawn
    # with its top-left at (390, 218) in the 1600x720 office. Stored at 3/4 of the office scale
    # (~240 KB instead of 425 KB, stretched when drawn) so it fits in RAM with everything else.
    im = Image.open(os.path.join(SRC, "M0003.png")).convert("RGBA").crop((2, 2, 2 + 541, 2 + 521))
    size = (round4(541 * BUTTON_SCALE * 0.75), round4(521 * BUTTON_SCALE * 0.75))
    save_rgx(im.resize(size, Image.LANCZOS), "golden_office")
    print("golden freddy: %dx%d" % size)


def build_menu_text():
    for name, (atlas, x, y, w, h) in MENU_TEXT.items():
        im = Image.open(os.path.join(SRC, atlas + ".png")).convert("RGBA").crop((x, y, x + w, y + h))
        size = (round4(w * MENU_SCALE[0]), round4(h * MENU_SCALE[1]))
        save_rgx(im.resize(size, Image.LANCZOS), "menu_" + name)
    print("menu text: %d sprites" % len(MENU_TEXT))


# Looping sounds whose recordings don't join up (the fan's 9.6 s hum jumps back audibly).
# Their last N seconds are crossfaded into the start so the loop is seamless.
LOOP_CROSSFADE = {"fan": 0.75, "light": 0.5, "camhum": 0.5, "menustatic": 0.75}


def crossfade_loop(path, seconds):
    with open(path, "rb") as f:
        samples = list(struct.unpack("<%dh" % (os.path.getsize(path) // 2), f.read()))
    fade = min(int(seconds * 22050), len(samples) // 3)
    body = samples[:len(samples) - fade]
    tail = samples[len(samples) - fade:]
    # Equal-power curve: these hums are noise-like, so a linear blend would dip ~3 dB in
    # the middle; sin/cos weights keep the loudness steady through the join.
    for i in range(fade):
        t = i / float(fade) * math.pi / 2.0
        body[i] = int(max(-32768, min(32767, round(body[i] * math.sin(t) + tail[i] * math.cos(t)))))
    with open(path, "wb") as f:
        f.write(struct.pack("<%dh" % len(body), *body))


def build_sounds(table):
    sizes = {}
    for name, (number, max_seconds) in table.items():
        src = os.path.join(SRC, "%04d.ogg" % number)
        dst = os.path.join(DATA, "snd", name + ".pcm")
        channels = "2" if name in STEREO_SOUNDS else "1"
        cmd = [FFMPEG, "-v", "error", "-y", "-i", src, "-ac", channels, "-ar", "22050"]
        if max_seconds:
            cmd += ["-t", str(max_seconds)]
        if name in SOUND_GAIN:
            # Louder than the mixer's maximum allows: boost the samples, with a limiter against clipping.
            cmd += ["-af", "volume=%.2f,alimiter=limit=0.95" % SOUND_GAIN[name]]
        cmd += ["-f", "s16le", dst]
        subprocess.run(cmd, check=True)
        if name in LOOP_CROSSFADE:
            crossfade_loop(dst, LOOP_CROSSFADE[name])
        sizes[name] = os.path.getsize(dst)
        print("sound %s: %d KB" % (name, sizes[name] // 1024))
    return sizes


def main():
    for sub in ("img", "spr", "snd"):
        os.makedirs(os.path.join(DATA, sub), exist_ok=True)

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
    foxy_run = [n for n in FOXY_RUN if os.path.exists(src_png(n))]   # the numbering has gaps (e.g. no 0249)
    for i, n in enumerate(foxy_run):
        save_jpeg(n, "foxyrun_%02d" % i, BG_SIZE)
    counts["foxyrun"] = len(foxy_run)

    for i, n in enumerate(FLIP_FRAMES):
        save_rgx(Image.open(src_png(n)).resize(FLIP_SIZE, Image.LANCZOS), "flip_%02d" % i)
    counts["flip"] = len(FLIP_FRAMES)

    counts.update(build_doors())
    counts.update(build_fan())
    build_buttons()
    build_golden()
    build_menu_text()
    build_sounds(SOUNDS)
    for name, size in build_sounds(STREAMS).items():
        counts["size_" + name] = size

    with open(os.path.join(DATA, "manifest.txt"), "w", newline="\n") as f:
        for key in sorted(counts):
            f.write("%s %d\n" % (key, counts[key]))

    total = 0
    for dirpath, _, files in os.walk(DATA):
        total += sum(os.path.getsize(os.path.join(dirpath, f)) for f in files)
    print("counts:", counts)
    print("data: %.1f MB in %s" % (total / 1e6, DATA))


if __name__ == "__main__":
    main()
