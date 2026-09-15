# 04: `stream read failed` in the log

**Status:** watch for it

## What to look for

A line like this in `/octiso.log`:

```
FNAF1: stream read failed for snd/call.pcm
```

The sound stops when its read fails.

## How streams read

All streamed sounds read on a low-priority background thread (`PcmPlayer::ReaderStep` in `FNAF1/Source/FnafData.cpp`):

- **SD boot:** each stream reads `FNAF1.iso` through its own file handle, while the engine reads the same ISO on the main thread. libfat serializes access to the card.
- **Disc boot (Dolphin, or a real disc):** streams read through the engine's disc reader, which the main thread also uses for pictures.

## The Dolphin crash, and the engine change it needs

The first background-thread build crashed in Dolphin inside `malloc`, while loading a camera picture. The engine's whole-file DVD read (`OctDvdReadAligned` in `SYS_AcquireFileData`, `System_Dolphin.cpp`) didn't take the ISO mutex, so a thread read could overlap it and write past a buffer.

- **First workaround:** disc boots read streams on the main thread. That stopped the crash but made Dolphin stutter, both audio and picture.
- **Current fix:** the engine's whole-file DVD read now takes the ISO mutex, and disc boots use the background thread again.

The mutex fix is in Octave-libogc commit `99db4e5`. Building this game against an older engine can bring the crash back on disc boots.
