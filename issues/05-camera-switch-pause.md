# 05: Short pause when switching cameras

**Status:** known, not fixed

## Symptom

Switching cameras, or a camera picture changing, can take a frame or two longer than usual.

## Cause

To save memory, camera pictures aren't loaded up front. `FnafGame::ShowImage` reads the picture's JPEG from the disc and decodes it on the main thread when it's shown. Jumpscare frames and Foxy's run work the same way. Only the office pictures and static frames stay in RAM, because they change often (the lights flash, the static animates).

The camera view shows static when it switches, which hides some of the delay.

## Possible fixes

- Read camera pictures on the background thread, and show static until the picture is ready.
- Keep the most recently viewed camera pictures in RAM, if memory allows. Check the `free` number in the `perf` lines.
