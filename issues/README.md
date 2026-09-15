# Issues

Open items to check on a real GameCube, booting `FNAF1.iso` from the SD through Swiss. Each file says what to look for, how to read `/octiso.log`, and the planned fix.

| # | Issue | Status |
|---|---|---|
| [01](01-stutter.md) | Stutter while idle or panning the office | Fix in, not yet tested on hardware |
| [02](02-stream-read-time.md) | Stream reads showing up in frame time | Fix in, not yet tested on hardware |
| [03](03-sound-gaps.md) | Call or ambience cutting out | Watch for it |
| [04](04-stream-read-failed.md) | `stream read failed` in the log | Watch for it |
| [05](05-camera-switch-pause.md) | Short pause when switching cameras | Known, not fixed |

## Reading the log

With Octave's local SD logger enabled, the game writes to `/octiso.log` on the SD card. Every 5 s during a night it adds a line like:

```
FNAF1: perf avg 16.9 ms, worst 20.1 ms, streams 0.1 ms/frame (worst 0.4 ms), call on, free 2462 KB
```

| Field | Meaning |
|---|---|
| `avg` / `worst` | Average and longest frame in the last 5 s. 16.7 ms is 60 fps. |
| `streams` | Main-thread time spent on streamed sounds per frame, and the worst single frame |
| `call` | Whether the phone call is still playing |
| `free` | Free heap memory |

It also logs `FNAF1: streams read /FNAF1.iso directly` once when streamed sounds open their own handle on the ISO. If that line is missing, streams fell back to the engine's reads.
