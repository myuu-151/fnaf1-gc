# 04: `stream read failed` in the log

**Status:** watch for it

## What to look for

A line like this in `/octiso.log`:

```
FNAF1: stream read failed for snd/call.pcm
```

The sound stops when its read fails.

## Why it could happen

On an SD boot, streamed sounds read `FNAF1.iso` through their own file handles on the background thread (`PcmPlayer::ReaderStep` in `FNAF1/Source/FnafData.cpp`), while the engine reads the same ISO on the main thread. libfat serializes access to the card, so this is expected to work, but it hasn't been tested on hardware yet.

## Related: the Dolphin crash

The first background-thread build crashed in Dolphin inside `malloc`, when loading a camera picture. In Dolphin the ISO is read as a disc, so the thread went through the engine's DVD reader. The engine's whole-file DVD read (`OctDvdReadAligned` in `System_Dolphin.cpp`) doesn't take the ISO mutex, so it could overlap with a thread read and write past a buffer.

Fixed in the game: on disc boots (no own SD handle), streams read on the main thread. The underlying engine bug is still there. It could also affect the engine's own async loading and video streaming on disc boots, and the fix would be taking the ISO mutex around that read.
