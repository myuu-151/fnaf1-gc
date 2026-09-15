#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "AssetRef.h"
#include "JpegYuvDecoder.h"

class Texture;
class SoundWave;

// Game data lives in loose files under sd:/FNAF1/Data (built by tools/build_data.py).
bool ReadDataFile(const std::string& relPath, std::vector<uint8_t>& out);

// A YUV texture that shows one background at a time. Backgrounds stay in RAM as
// JPEG bytes and are decoded straight into the texture's planes when shown.
class YuvCanvas
{
public:

    bool Init(const char* name, uint32_t width, uint32_t height);
    bool Show(const std::vector<uint8_t>& jpeg);
    Texture* GetTexture() const;

private:

    AssetRef mTexture;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    JpegYuvDecoder mDecoder;
};

// An RGBA8 texture loaded from a .rgx file (GX_TF_RGBA8 texels).
struct Sprite
{
    AssetRef mTexture;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;

    Texture* Get() const;
};

bool LoadSprite(const std::string& relPath, Sprite& out);

// 16-bit little-endian mono PCM.
SoundWave* LoadPcmSound(const std::string& relPath, uint32_t sampleRate);

// Reads part of a data file (straight from the disc image when booted from the ISO).
bool ReadDataRange(const std::string& relPath, uint32_t offset, uint32_t size, char* out);

// Free heap memory in KB (for the SD log).
uint32_t GetFreeMemoryKb();

// A long sound (phone call, ambience, music box) played from the disc through an
// Octave PCM stream a little at a time, so it never sits in RAM. Call Update() every
// frame. 16-bit little-endian mono PCM at 22050 Hz.
class PcmPlayer
{
public:

    ~PcmPlayer();

    bool Start(const std::string& relPath, uint32_t sizeBytes, bool loop, float volume);
    void Stop();
    void Update();
    bool IsPlaying() const;

    // With the stream mutex held: reads the next chunk if one is wanted. The reader thread
    // handles players with their own SD file; the main thread handles the rest (disc boots),
    // because the engine's whole-file disc reads aren't locked against other threads.
    bool ReaderStep(bool mainThread);

private:

    // The SD reads (50-100 ms each on the GameCube) happen on a background thread so they
    // never hold up a frame. Fields below mStream are shared with it under the stream mutex.
    uint32_t mStream = 0;
    std::string mPath;
    uint32_t mSize = 0;
    uint64_t mQueuedFrames = 0;
    bool mLoop = false;

    uint32_t mOffset = 0;           // next byte the reader reads
    bool mWantRead = false;         // main thread wants another chunk
    bool mReading = false;          // the reader is using mFile and mReady
    bool mReadFailed = false;
    std::vector<char> mReady;       // chunk the reader has read
    uint32_t mReadyBytes = 0;

    // Its own handle on FNAF1.iso when booted from the SD, so the streams don't share
    // the engine's file position (see PcmPlayer::Start).
    FILE* mFile = nullptr;
    uint32_t mFileBase = 0;         // offset of the sound's data in the ISO
    uint32_t mFilePos = UINT32_MAX; // sound offset the handle is at
};
