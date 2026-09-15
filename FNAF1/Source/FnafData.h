#pragma once

#include <cstdint>
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

private:

    uint32_t mStream = 0;
    std::string mPath;
    uint32_t mSize = 0;
    uint32_t mOffset = 0;
    uint64_t mQueuedFrames = 0;
    bool mLoop = false;
    std::vector<char> mChunk;
};
