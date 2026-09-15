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
