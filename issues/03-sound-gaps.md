# 03: Call or ambience cutting out

**Status:** watch for it

## What to listen for

The phone call, the ambience, the fan, or the rare music (pirate song, circus) briefly cutting out and coming back.

## Why it could happen

Each streamed sound keeps about 1 s of audio queued (`kStreamAheadFrames` in `FNAF1/Source/FnafData.cpp`) and reads 0.5 s at a time (`kStreamChunkBytes`). The background reader runs below the main thread's priority, so it only gets time while the main thread waits for vsync. If several streams need data at once, or the SD is slow, the queue can run dry before the next chunk arrives.

## Planned fix if it happens

Keep more audio queued, for example 2 s ahead instead of 1 s. That costs a little heap memory per stream (the engine buffers queued PCM), so check the `free` number in the `perf` lines afterwards.
