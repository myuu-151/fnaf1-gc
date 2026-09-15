# 02: Stream reads showing up in frame time

**Status:** fix in, not yet tested on hardware

## Symptom

The `perf` log lines showed the time spent on streamed sounds each frame. Before the background thread:

| Build | `streams` per frame | `streams` worst |
|---|---|---|
| Streams sharing the engine's ISO handle | 3.4–3.7 ms | up to 183 ms |
| Each stream with its own ISO handle | 2.0–3.6 ms | 50–100 ms |

## Causes

- **Shared handle:** the call and ambience read from different places in the ISO through the engine's single `FILE`. On FAT, every backwards seek walks the file's cluster chain from the start.
- **Slow reads:** even straight-through reads of a 0.5 s chunk took 50–100 ms on the SD.

## Fixes

- Each stream opens its own handle on `FNAF1.iso`, at the sound's offset read from the ISO's file table.
- The reads moved to a background thread (see [01](01-stutter.md)).

## To check

The `streams` numbers in the `perf` lines should drop to almost nothing (well under 1 ms per frame, with a worst of a few ms).
