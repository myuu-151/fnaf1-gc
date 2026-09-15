# 01: Stutter while idle or panning the office

**Status:** fix in, not yet tested on hardware

## Symptom

The game stuttered on the GameCube, even while idling in the office. Panning left and right made it easy to see.

## Cause

The `perf` lines showed about 16.7 ms per frame of rendering (60 fps), plus 2–4 ms per frame on average reading streamed sounds from the SD. Single stream reads took 50–100 ms, and they happened on the main thread during frames.

## Fix

Stream reads now run on a background thread at priority 40, below the main thread's 64, the same way the engine's `VideoStream` works. The main thread only passes finished chunks to the audio stream (`PcmPlayer` in `FNAF1/Source/FnafData.cpp`).

This only applies when booting from the SD. On a disc boot (Dolphin or a real disc), streams still read on the main thread; see [04](04-stream-read-failed.md).

## To check

- Idle in the office for a minute, then pan left and right. Both should be smooth.
- The `perf` lines should show `avg` close to 16.7 ms and a small `worst`.
