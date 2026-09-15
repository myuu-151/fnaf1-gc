# FNAF1 for GameCube

A private, personal port of Five Nights at Freddy's (all credit to Scott Cawthon) to the Nintendo GameCube, running on the [Octave](https://github.com/myuu-151/Octave-libogc) engine.

`FNAF1/` is an Octave C++ project packaged with Octave's own packager. The game has no scenes and uses no editor-imported assets: its C++ builds the UI at runtime and loads its own data files. The packager includes those files and builds the GameCube disc image.

**Status:** Night 1 prototype. Boots and runs from `FNAF1.iso` in Dolphin and on a real GameCube (from SD through Swiss).

## Layout

| Path | Contents |
|---|---|
| `FNAF1/` | The Octave project: `FNAF1.octp`, `Config.ini`, `Source/` (game code), `Makefile_GCN`, `Assets/` (empty) |
| `FNAF1/Scripts/Data/` | Converted game data (built by `tools/build_data.py`). It lives under `Scripts/` because the packager copies that folder (with subfolders) into the package and the disc, and only runs `.lua` files from it. |
| `tools/build_data.py` | Converts `source/` into `FNAF1/Scripts/Data/` |
| `source/` | The original game's HTML5 (Clickteam Fusion) export: numbered images and sounds, plus `Runtime.js` |
| `data/` | `Application.ccj` extracted from `source/resources/FNAF1HTML5.cc1` (game logic and object/sound names), and its UTF-16 strings |

## Building

Requirements:
- devkitPro with devkitPPC
- [Octave-libogc](https://github.com/myuu-151/Octave-libogc) at `Documents/octave-libogc` (the makefile's `OCTAVE` variable), with its GameCube engine library built. Use commit `73155d6` or later. That commit makes the SD Gecko driver write at 13.5 MHz; before it, writes at 27 MHz corrupted files on passive SD adapters.
- An `Octave.exe` whose packager builds a project's own `Source/` + `Makefile_GCN`. This is a local change in `ActionManager.cpp` (`BuildData`), marked `LOCAL FNAF1` and not in the Octave repo. Without it the packager compiles Octave's Standalone game instead.
- Python 3 with Pillow
- ffmpeg (the script uses Octave's `External/ffmpeg/bin/ffmpeg.exe`, or `OCTAVE_FFMPEG`)

Steps:

```sh
# 1. Convert the game data (only needed when source/ or the converter changes)
python tools/build_data.py

# 2. Package with Octave (from the octave-libogc folder, with DEVKITPRO/DEVKITPPC set
#    and devkitPro's make on PATH)
Octave.exe -headless -project <path>/fnaf1-gc/FNAF1/FNAF1.octp -build GameCube
```

The packager compiles `FNAF1/Makefile_GCN` and writes `FNAF1/Packaged/GameCube/`, including `FNAF1.dol` and `FNAF1.iso`.

To only compile the DOL: `make -f Makefile_GCN` in `FNAF1/`, which writes `Build/GCN/FNAF1.dol`.

## Running

- **Dolphin:** open `FNAF1.iso`.
- **GameCube (SD):** put `FNAF1.iso` on the SD card root and boot it from Swiss. The engine finds `/FNAF1.iso` and reads everything from it.
  - **With two SD adapters:** the card in serial port 2 mounts first and becomes the default, so the ISO has to be on that card. Otherwise the engine falls back to the disc drive and hangs on a green screen.

Booting just the DOL with loose files on the SD isn't supported. Without the ISO, the engine tries the disc drive and stalls on a green screen.

## Controls

| Input | Action |
|---|---|
| Control stick | Look around the office |
| L / R | Close or open the left / right door |
| D-pad left / right | Left / right hall light |
| A | Raise or lower the camera tablet |
| D-pad left / right (tablet up) | Switch camera |
| B | Mute the phone call |
| Z (tablet down) | Honk the Freddy poster's nose |
| Start | Restart after a game over or 6 AM |

## What the prototype has

- Office panning, both doors with their closing animation, and hall lights that show Bonnie or Chica at the door
- Power drain with the usage meter, and the clock from 12 AM to 6 AM
- All 11 cameras, with static and slow panning, and the tablet flip
- Bonnie and Chica moving along their routes
- Jumpscares for Bonnie, Chica and Foxy, and Freddy after a power-out
- Foxy: Pirate Cove stages, the West Hall run on CAM 2A, banging on the closed door (which costs power), and his lunge
- Game over and 6 AM screens
- Rare camera pictures: Freddy staring on the Show Stage, and rare posters on empty cameras (Pirate Cove "IT'S ME" sign, West Hall Corner Freddy and Golden Freddy posters, East Hall drawings, East Hall Corner newspapers)
- The Freddy poster's nose honk
- Sound:
  - **Streamed from the disc:** the night 1 phone call, background ambience, the fan, the rare music (Foxy's pirate song, loud only while watching Pirate Cove, and the faint circus tune), the power-out music box and Freddy's laugh, and the 6 AM chimes and cheering
  - **Short sounds (kept in RAM):** doors, lights, tablet and camera hum, camera garbles when someone moves on camera, footsteps, window scare, Chica's pots in the kitchen, Foxy's running and banging

Not in it yet:
- Freddy roaming
- The original AI tables (the prototype uses its own difficulty), and nights 2–5 with their phone calls
- The exact odds of the rare camera pictures (placeholder: 1 in 20 per picture)
- Golden Freddy in the office (only his rare poster is in)

## Data formats (`FNAF1/Scripts/Data`)

| File | Format |
|---|---|
| `img/*.jpg` | 992x448 (static frames: 320x176) baseline 4:2:0 JPEG, decoded on the GameCube with Octave's `JpegYuvDecoder` into YUV textures |
| `spr/*.rgx` | `"RGX8"` + big-endian u16 width and height, followed by raw `GX_TF_RGBA8` texels (4x4 blocks: 16 A,R pairs, then 16 G,B pairs) |
| `snd/*.pcm` | 16-bit little-endian mono PCM, 22050 Hz |
| `manifest.txt` | Frame counts: doors, jumpscares, static, tablet flip |

## Notes

- **Logging:** with Octave's local SD logger enabled, the game writes its startup steps (`FNAF1: ...`) to `/octiso.log` alongside the engine's file loads. Every 5 s it also writes a `perf` line with frame times, time spent on streams, and free memory.
- **Memory:** the GameCube's 24 MB fills up quickly. Only the office pictures, static frames, sprites and short sounds are loaded up front. Camera pictures, jumpscares and Foxy's run are read from the disc when shown, and long sounds are streamed.
- **Streaming from the SD:**
  - Each streamed sound opens its own handle on `FNAF1.iso` at the sound's offset (found from the ISO's file table). On FAT, a backwards seek walks the file's cluster chain from the start, so sounds sharing the engine's one handle took up to 180 ms per read.
  - The reads run on a low-priority background thread, because even straight-through reads take 50–100 ms per chunk.
  - Disc boots (Dolphin, or a real disc) read on the main thread instead: the engine's whole-file DVD reads aren't locked against other threads.
- **Sound names:** these come from the game's play-sound actions in `Application.ccj` (bytes `88 00 06 00`, then a u16 sound number, then the UTF-16 name). For example, sound 15 is `XSCREAM` and sound 41 is `voiceover1c`.
- **Door animations:** the frames come from the texture atlases. The left door's frames are 223 px wide and the right door's are 229 px, ordered by how much of the doorway they cover. The door and button positions in the office are estimates.
