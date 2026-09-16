# FNAF1 for GameCube

A private, personal remake of Five Nights at Freddy's (all credit to Scott Cawthon) for the Nintendo GameCube, built on the [Octave](https://github.com/myuu-151/Octave-libogc) engine.

It isn't a port: the original is a Clickteam Fusion game with no source to recompile. Its images and sounds are converted for the console, and its logic is reimplemented in C++ to match the events decompiled from `Application.ccj` — the AI rolls, the timers, the camera picture odds and the sound levels.

**Status:** Night 1, matched to those events. Runs in Dolphin and on a real GameCube.

## Running

- **Dolphin:** open `FNAF1.iso`.
- **GameCube:** put `FNAF1.iso` on an SD card's root and start it from Swiss. With two SD adapters, use the card in serial port 2: that one mounts first, and the ISO has to be on it.

The ISO is required; booting the DOL alone isn't supported.

## Controls

| Input | Action |
|---|---|
| Control stick | Look around the office |
| L / R | Close or open the left / right door |
| D-pad left / right | Left / right door light |
| A | Raise or lower the camera tablet |
| D-pad left / right (tablet up) | Switch camera |
| B | Mute the phone call |
| Z | Honk the Freddy poster's nose |
| D-pad up / down, A, START (menus) | Choose and confirm |

Debug keys during a night: **X** power out, **Y** Bonnie's jumpscare, **D-pad down** Chica's, **D-pad up** both at the doors, **START** skip to 6 AM, **C-stick down** Foxy's run, **C-stick up** Golden Freddy (his ending resets the console, as it closes the game in the original).

## What's in

- The office: panning, both doors and lights, the power meter and usage, and the clock to 6 AM
- All 11 cameras with the map, static, camera switching and the tablet flip
- Bonnie, Chica and Foxy with the original's night 1 behaviour, their jumpscares, and Freddy's after a power-out
- The power-out: the music box, his face in the dark, and everything still moving while you sit there
- Golden Freddy, the hallucination flashes, and the rare camera pictures at the original's odds
- The phone call, the ambience, and the original's sound levels

## Not yet

- Nights 2–5, their phone calls, and Freddy roaming (he's inactive on night 1 in the original too)
- Saving progress

## Building

Needs devkitPro with devkitPPC, Python 3 with Pillow, ffmpeg, and [Octave-libogc](https://github.com/myuu-151/Octave-libogc) at `Documents/octave-libogc` (commit `ba79236` or later) with its GameCube library and `Octave.exe` built.

```sh
# Convert the original's images and sounds (only when source/ or the converter changes)
python tools/build_data.py

# Build the disc image -> FNAF1/Packaged/GameCube/FNAF1.iso
Octave.exe -headless -project <path>/fnaf1-gc/FNAF1/FNAF1.octp -build GameCube
```

`make -f Makefile_GCN` in `FNAF1/` compiles just the DOL.

## Layout

| Path | Contents |
|---|---|
| `FNAF1/Source/` | The game's C++ (it builds its UI at runtime; no scenes or editor assets) |
| `FNAF1/Scripts/Data/` | Converted images, sounds and sprites, packed into the ISO |
| `tools/build_data.py` | Converts `source/` into that folder |
| `source/`, `data/` | The original's Clickteam export, and `Application.ccj` with its game logic |

## Notes

- **Memory:** the GameCube's 24 MB is nearly full. Office and camera pictures, Foxy's run and short sounds live in RAM; jumpscare frames are read from the disc as they play, and long sounds are streamed.
- **Reading from SD:** each streamed sound and the animations read the ISO through their own handle on a background thread, because a seek on FAT costs 50–100 ms.
- **Logging:** with Octave's local SD logger enabled, the game writes its startup steps, animation timings and a `perf` line every 5 s to `/octiso.log`.
- **Where it departs from the original:** Foxy's run is stored at lower quality and doesn't get through all its frames on console, since every frame is an SD read and a JPEG decode. With no mouse, the view jumps to the left door when Foxy arrives, and the tablet bar is centred to suit a 4:3 screen.
