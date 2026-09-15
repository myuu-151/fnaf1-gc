#include "FnafData.h"

#include "AssetManager.h"
#include "Assets/Texture.h"
#include "Assets/SoundWave.h"
#include "Audio/Audio.h"
#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "System/System.h"
#include "Log.h"

#include <cstring>
#include <ogc/cache.h>

static const char* kDataRoot = "FNAF1/Scripts/Data/";

bool ReadDataFile(const std::string& relPath, std::vector<uint8_t>& out)
{
    std::string path = kDataRoot + relPath;
    char* data = nullptr;
    uint32_t size = 0;

    // As an asset path: served from the disc image's FST when booted from FNAF1.iso
    // (disc or SD), otherwise from loose files on the SD.
    SYS_AcquireFileData(path.c_str(), true, 0, data, size);

    if (data == nullptr || size == 0)
    {
        LogError("FNAF1: missing data file %s", path.c_str());
        SYS_ReleaseFileData(data);
        out.clear();
        return false;
    }

    out.assign((uint8_t*)data, (uint8_t*)data + size);
    SYS_ReleaseFileData(data);
    return true;
}

bool YuvCanvas::Init(const char* name, uint32_t width, uint32_t height)
{
    Texture* texture = NewTransientAsset<Texture>();
    texture->SetName(name);
    texture->InitDynamicYuv(width, height);
    texture->Create();

    TextureResource* resource = texture->GetResource();
    if (resource->mDynamicData == nullptr)
    {
        LogError("FNAF1: could not allocate %ux%u canvas", width, height);
        return false;
    }

    mTexture = texture;
    mWidth = width;
    mHeight = height;
    return true;
}

bool YuvCanvas::Show(const std::vector<uint8_t>& jpeg)
{
    Texture* texture = mTexture.Get<Texture>();
    if (texture == nullptr || jpeg.empty())
    {
        return false;
    }

    TextureResource* resource = texture->GetResource();

    JpegYuvPlanes planes;
    planes.mY = (uint8_t*)resource->mDynamicData;
    planes.mCb = (uint8_t*)resource->mDynamicCb;
    planes.mCr = (uint8_t*)resource->mDynamicCr;
    planes.mGxLayout = true;

    if (!mDecoder.Decode(jpeg.data(), (uint32_t)jpeg.size(), mWidth, mHeight, planes))
    {
        LogError("FNAF1: JPEG decode failed");
        return false;
    }

    DCFlushRange(planes.mY, mWidth * mHeight);
    DCFlushRange(planes.mCb, (mWidth / 2) * (mHeight / 2));
    DCFlushRange(planes.mCr, (mWidth / 2) * (mHeight / 2));

    // Re-point the texture objects at the (updated) planes so GX reloads them.
    GFX_SetTextureResourceData(texture, nullptr);
    return true;
}

Texture* YuvCanvas::GetTexture() const
{
    return mTexture.Get<Texture>();
}

Texture* Sprite::Get() const
{
    return mTexture.Get<Texture>();
}

bool LoadSprite(const std::string& relPath, Sprite& out)
{
    std::vector<uint8_t> file;
    if (!ReadDataFile(relPath, file))
    {
        return false;
    }

    if (file.size() < 8 || memcmp(file.data(), "RGX8", 4) != 0)
    {
        LogError("FNAF1: %s is not an RGX8 sprite", relPath.c_str());
        return false;
    }

    const uint32_t width = (uint32_t(file[4]) << 8) | file[5];
    const uint32_t height = (uint32_t(file[6]) << 8) | file[7];
    const uint32_t texelBytes = width * height * 4;

    if (width == 0 || height == 0 || (width % 4) != 0 || (height % 4) != 0 || file.size() < 8 + texelBytes)
    {
        LogError("FNAF1: %s has a bad size (%ux%u)", relPath.c_str(), width, height);
        return false;
    }

    Texture* texture = NewTransientAsset<Texture>();
    texture->SetName(relPath);
    texture->InitDynamic(width, height);
    texture->Create();

    TextureResource* resource = texture->GetResource();
    if (resource->mDynamicData == nullptr)
    {
        LogError("FNAF1: could not allocate sprite %s", relPath.c_str());
        return false;
    }

    // The file already holds GX_TF_RGBA8 texels, so copy them in as-is.
    memcpy(resource->mDynamicData, file.data() + 8, texelBytes);
    DCFlushRange(resource->mDynamicData, texelBytes);
    GFX_SetTextureResourceData(texture, nullptr);

    out.mTexture = texture;
    out.mWidth = width;
    out.mHeight = height;
    return true;
}

bool ReadDataRange(const std::string& relPath, uint32_t offset, uint32_t size, char* out)
{
    std::string path = kDataRoot + relPath;
    return SYS_ReadFileRange(path.c_str(), true, offset, size, out);
}

static constexpr uint32_t kStreamRate = 22050;
static constexpr uint32_t kStreamChunkBytes = (kStreamRate / 5) * 2;   // 0.2 s per read
static constexpr uint64_t kStreamAheadFrames = kStreamRate / 2;        // keep ~0.5 s queued

PcmPlayer::~PcmPlayer()
{
    Stop();
}

bool PcmPlayer::Start(const std::string& relPath, uint32_t sizeBytes, bool loop, float volume)
{
    Stop();

    if (sizeBytes < 2)
    {
        LogError("FNAF1: stream %s has no data", relPath.c_str());
        return false;
    }

    mStream = AUD_OpenStream(kStreamRate, 1);
    if (mStream == 0)
    {
        LogError("FNAF1: no free audio stream for %s", relPath.c_str());
        return false;
    }

    mPath = relPath;
    mSize = sizeBytes & ~1u;
    mOffset = 0;
    mQueuedFrames = 0;
    mLoop = loop;
    mChunk.resize(kStreamChunkBytes);

    AUD_SetStreamVolume(mStream, volume);
    Update();
    AUD_SetStreamPaused(mStream, false);
    return true;
}

void PcmPlayer::Stop()
{
    if (mStream != 0)
    {
        AUD_CloseStream(mStream);
        mStream = 0;
    }
}

bool PcmPlayer::IsPlaying() const
{
    return mStream != 0;
}

void PcmPlayer::Update()
{
    if (mStream == 0)
    {
        return;
    }

    const uint64_t played = AUD_GetStreamPlayedFrames(mStream);

    while (mQueuedFrames < played + kStreamAheadFrames)
    {
        if (mOffset >= mSize)
        {
            if (!mLoop)
            {
                break;
            }
            mOffset = 0;
        }

        const uint32_t bytes = glm::min(kStreamChunkBytes, mSize - mOffset);
        if (!ReadDataRange(mPath, mOffset, bytes, mChunk.data()))
        {
            LogError("FNAF1: stream read failed for %s", mPath.c_str());
            Stop();
            return;
        }

        AUD_QueueStreamData(mStream, (const uint8_t*)mChunk.data(), bytes);
        mQueuedFrames += bytes / 2;
        mOffset += bytes;
    }

    // A one-shot sound is done once everything queued has played (the played count
    // advances in 1024-sample steps at 48 kHz, so allow a little slack).
    if (!mLoop && mOffset >= mSize && played + 1024 >= mQueuedFrames)
    {
        Stop();
    }
}

SoundWave* LoadPcmSound(const std::string& relPath, uint32_t sampleRate)
{
    std::vector<uint8_t> file;
    if (!ReadDataFile(relPath, file))
    {
        return nullptr;
    }

    const uint32_t size = uint32_t(file.size()) & ~1u;
    uint8_t* buffer = AUD_AllocWaveBuffer(size);
    if (buffer == nullptr)
    {
        LogError("FNAF1: could not allocate sound %s", relPath.c_str());
        return nullptr;
    }

    memcpy(buffer, file.data(), size);

    SoundWave* sound = NewTransientAsset<SoundWave>();
    sound->SetName(relPath);
    sound->SetPcmData(buffer, size, size / 2, 16, 1, sampleRate);
    return sound;
}
