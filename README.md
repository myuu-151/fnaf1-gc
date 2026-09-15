# FNAF1 for GameCube

A private, personal port of Five Nights at Freddy's (all credit to Scott Cawthon) to the Nintendo GameCube, running on the [Octave](https://github.com/myuu-151/Octave-libogc) engine.

It is a code-only Octave game: there is no editor project or scene. The game builds its UI at runtime and loads its own data files from the SD card.

**Status:** Night 1 prototype. Not yet tested on hardware.

## Layout

| Path | Contents |
|---|---|
| `source/` | The original game's HTML5 (Clickteam Fusion) export: numbered images and sounds, plus `Runtime.js` |
| `data/` | `Application.ccj` extracted from `source/resources/FNAF1HTML5.cc1` (game logic and object/sound names), and its UTF-16 strings |
| `tools/build_data.py` | Converts `source/` into `sd/` |
| `game/` | The GameCube game: `Makefile_GCN` and C++ sources |
| `sd/` | Ready-to-copy SD card contents (built) |

## Building

Requirements:
- devkitPro with devkitPPC
- [Octave-libogc](https://github.com/myuu-151/Octave-libogc), checked out next to this repo (`Documents/octave-libogc`), with its GameCube engine library built (`Engine/Makefile_GCN`)
- Python 3 with Pillow
- ffmpeg (the script uses Octave's `External/ffmpeg/bin/ffmpeg.exe`, or `OCTAVE_FFMPEG`)
- A packaged Octave GameCube project to copy the cooked engine assets from. The default is `testproj/Packaged/GameCube`; set `OCTAVE_GCN_PACKAGE` to use another.

Steps:

```sh
# 1. Build the game (from a devkitPro shell)
cd game
make -f Makefile_GCN          # -> game/Build/FNAF1.dol

# 2. Convert the data and assemble the SD folder (copies the DOL in too)
cd ..
python tools/build_data.py    # -> sd/
```

## Running

1. Copy everything inside `sd/` to the root of the SD card: `FNAF1.dol`, `FNAF1/` and `Engine/`.
2. Boot `FNAF1.dol` from Swiss.

## Controls

| Input | Action |
|---|---|
| Control stick | Look around the office |
| L / R | Close or open the left / right door |
| D-pad left / right | Left / right hall light |
| A | Raise or lower the camera tablet |
| D-pad left / right (tablet up) | Switch camera |
| Start | Restart after a game over or 6 AM |

## What the prototype has

- Office panning, both doors with their closing animation, and hall lights that show Bonnie or Chica at the door
- Power drain with the usage meter, and the clock from 12 AM to 6 AM
- All 11 cameras, with static and slow panning, and the tablet flip
- Bonnie and Chica moving along their routes
- Jumpscares for Bonnie and Chica, and Freddy after a power-out
- Game over and 6 AM screens

Not in it yet:
- Freddy roaming and Foxy
- The original AI tables (the prototype uses its own difficulty), nights 2–5 and the phone calls

## Data formats (`sd/FNAF1/Data`)

| File | Format |
|---|---|
| `img/*.jpg` | 992x448 (static frames: 320x176) baseline 4:2:0 JPEG, decoded on the GameCube with Octave's `JpegYuvDecoder` into YUV textures |
| `spr/*.rgx` | `"RGX8"` + big-endian u16 width and height, followed by raw `GX_TF_RGBA8` texels (4x4 blocks: 16 A,R pairs, then 16 G,B pairs) |
| `snd/*.pcm` | 16-bit little-endian mono PCM, 22050 Hz |
| `manifest.txt` | Frame counts: doors, jumpscares, static, tablet flip |

The engine boots project `FNAF1` (`FNAF1/FNAF1.octp`) only for its own assets. `FNAF1/AssetRegistry.txt` lists just the engine assets.

## Notes

- **Sound names:** these come from the game's play-sound actions in `Application.ccj` (bytes `88 00 06 00`, then a u16 sound number, then the UTF-16 name). For example, sound 15 is `XSCREAM` and sound 41 is `voiceover1c`.
- **Door animations:** the frames come from the texture atlases. The left door's frames are 223 px wide and the right door's are 229 px, ordered by how much of the doorway they cover. The door and button positions in the office are estimates.
