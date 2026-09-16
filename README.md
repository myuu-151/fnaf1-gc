# FNAF1 for GameCube

A reimplementation of Five Nights at Freddy's (all credit to Scott Cawthon) for the Nintendo GameCube, built on the [Octave](https://github.com/myuu-151/Octave-libogc) engine.


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
