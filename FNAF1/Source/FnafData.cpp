#include "FnafData.h"

#include "AssetManager.h"
#include "Assets/Texture.h"
#include "Assets/SoundWave.h"
#include "Audio/Audio.h"
#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "System/System.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <malloc.h>
#include <unordered_map>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/system.h>

// SD diagnostic log (Octave System_Dolphin.cpp).
void OctLog(const char* format, ...);

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
// Each stream reads through its own file handle, so its reads run straight through the
// file. A small queue keeps the engine's stream buffers (heap memory) small.
static constexpr uint32_t kStreamChunkBytes = kStreamRate;             // 0.5 s per read
static constexpr uint64_t kStreamAheadFrames = kStreamRate;            // keep ~1 s queued

uint32_t GetFreeMemoryKb()
{
    // Free blocks inside the heap, plus the part of MEM1 the heap hasn't grown into yet.
    const struct mallinfo info = mallinfo();
    const uint32_t unclaimed = uint32_t((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo());
    return (uint32_t(info.fordblks) + unclaimed) / 1024;
}

// Where each file sits inside FNAF1.iso on the SD, read from the disc image's FST.
// The engine reads the ISO through one shared FILE, and libfat walks a file's cluster
// chain from the start on every backwards seek. With two streams reading from different
// places that cost ~180 ms per read, so each stream opens the ISO itself.
static const char* kIsoPaths[] = { "/FNAF1.iso", "FNAF1/FNAF1.iso", "FNAF1.iso" };
static std::string sIsoPath;
static std::unordered_map<std::string, uint32_t> sIsoOffsets;
static bool sIsoScanned = false;

static uint32_t ReadBe32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static void ScanIso()
{
    sIsoScanned = true;

    for (const char* path : kIsoPaths)
    {
        FILE* file = fopen(path, "rb");
        if (file == nullptr)
        {
            continue;
        }

        uint8_t header[0x430];
        std::vector<uint8_t> fst;
        bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) && ReadBe32(header + 0x1C) == 0xC2339F3D;
        uint32_t fstSize = 0;

        if (ok)
        {
            const uint32_t fstOffset = ReadBe32(header + 0x424);
            fstSize = ReadBe32(header + 0x428);
            ok = fstSize >= 12 && fstSize < 1024 * 1024;
            if (ok)
            {
                fst.resize(fstSize);
                ok = fseek(file, long(fstOffset), SEEK_SET) == 0 && fread(fst.data(), 1, fstSize, file) == fstSize;
            }
        }
        fclose(file);

        const uint32_t count = ok ? ReadBe32(&fst[8]) : 0;
        const uint32_t names = count * 12;
        if (!ok || count == 0 || names >= fstSize)
        {
            continue;
        }

        // Entries are in directory order: a directory entry holds the index just past
        // its last child, so a stack of open directories gives each file's full path.
        std::vector<std::pair<uint32_t, std::string>> dirs;
        for (uint32_t i = 1; i < count; ++i)
        {
            while (!dirs.empty() && i >= dirs.back().first)
            {
                dirs.pop_back();
            }

            const uint8_t* entry = &fst[i * 12];
            std::string name = dirs.empty() ? "" : dirs.back().second;
            for (uint32_t c = names + (ReadBe32(entry) & 0x00FFFFFF); c < fstSize && fst[c] != 0; ++c)
            {
                name += (char)tolower(fst[c]);
            }

            if (entry[0] & 1)
            {
                dirs.push_back({ ReadBe32(entry + 8), name + "/" });
            }
            else
            {
                sIsoOffsets[name] = ReadBe32(entry + 4);
            }
        }

        sIsoPath = path;
        OctLog("FNAF1: streams read %s directly (%u files)", path, (unsigned)sIsoOffsets.size());
        return;
    }
}

static bool FindIsoFile(const std::string& relPath, uint32_t& offset)
{
    if (!sIsoScanned)
    {
        ScanIso();
    }

    std::string key;
    for (char c : std::string(kDataRoot) + relPath)
    {
        key += (char)tolower((unsigned char)c);
    }

    auto it = sIsoOffsets.find(key);
    if (sIsoPath.empty() || it == sIsoOffsets.end())
    {
        return false;
    }

    offset = it->second;
    return true;
}

// One reader thread serves every PcmPlayer. The main thread only moves finished chunks
// into the engine's audio streams, so a slow SD read never lands on a frame.
static MutexObject* sStreamMutex = nullptr;
static ThreadObject* sStreamThread = nullptr;
static std::vector<PcmPlayer*> sStreamPlayers;

static ThreadFuncRet StreamReaderMain(void* arg)
{
    // Below the main thread (priority 64), like VideoStream: reads run while it waits for vsync.
    LWP_SetThreadPriority(LWP_GetSelf(), 40);

    for (;;)
    {
        bool worked = false;

        SYS_LockMutex(sStreamMutex);
        for (size_t i = 0; i < sStreamPlayers.size(); ++i)
        {
            worked = sStreamPlayers[i]->ReaderStep(false) || worked;
        }
        SYS_UnlockMutex(sStreamMutex);

        if (!worked)
        {
            SYS_Sleep(2);
        }
    }

    THREAD_RETURN();
}

bool PcmPlayer::ReaderStep(bool mainThread)
{
    // Disc boots (no own file) read on the main thread: the engine's whole-file DVD reads
    // skip its ISO mutex, so a DVD read from this thread could overlap one and corrupt memory.
    const bool ownFile = mFile != nullptr;
    if (mainThread == ownFile)
    {
        return false;
    }

    if (!mWantRead || mReading || mReadyBytes != 0 || mReadFailed)
    {
        return false;
    }

    if (mOffset >= mSize)
    {
        if (!mLoop)
        {
            mWantRead = false;
            return false;
        }
        mOffset = 0;
    }

    const uint32_t offset = mOffset;
    const uint32_t bytes = glm::min(kStreamChunkBytes, mSize - offset);
    mReading = true;

    // Read without the lock; Stop() waits for mReading to clear before closing mFile.
    SYS_UnlockMutex(sStreamMutex);

    bool ok = false;
    if (mFile != nullptr)
    {
        // Only seeks when looping back to the start.
        ok = (mFilePos == offset || fseek(mFile, long(mFileBase + offset), SEEK_SET) == 0) &&
             fread(mReady.data(), 1, bytes, mFile) == bytes;
        mFilePos = ok ? (offset + bytes) : UINT32_MAX;
    }
    else
    {
        ok = ReadDataRange(mPath, offset, bytes, mReady.data());
    }

    SYS_LockMutex(sStreamMutex);
    mReading = false;
    mWantRead = false;

    if (ok)
    {
        mReadyBytes = bytes;
        mOffset = offset + bytes;
    }
    else
    {
        mReadFailed = true;
    }
    return true;
}

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

    // Booted from FNAF1.iso on the SD: read it through our own handle. Otherwise (disc,
    // Dolphin) go through the engine.
    uint32_t base = 0;
    if (FindIsoFile(relPath, base))
    {
        mFile = fopen(sIsoPath.c_str(), "rb");
        mFileBase = base;
        mFilePos = UINT32_MAX;
    }

    mPath = relPath;
    mSize = sizeBytes & ~1u;
    mQueuedFrames = 0;
    mLoop = loop;
    mOffset = 0;
    mReading = false;
    mReadFailed = false;
    mReady.resize(kStreamChunkBytes);
    mReadyBytes = 0;

    if (sStreamMutex == nullptr)
    {
        sStreamMutex = SYS_CreateMutex();
        sStreamThread = SYS_CreateThread(StreamReaderMain, nullptr);
    }

    SYS_LockMutex(sStreamMutex);
    sStreamPlayers.push_back(this);
    mWantRead = true;
    SYS_UnlockMutex(sStreamMutex);

    AUD_SetStreamVolume(mStream, volume);
    AUD_SetStreamPaused(mStream, false);
    return true;
}

void PcmPlayer::Stop()
{
    if (sStreamMutex != nullptr)
    {
        SYS_LockMutex(sStreamMutex);
        mWantRead = false;

        // Let a read in progress finish before its file and buffer go away.
        while (mReading)
        {
            SYS_UnlockMutex(sStreamMutex);
            SYS_Sleep(1);
            SYS_LockMutex(sStreamMutex);
        }

        sStreamPlayers.erase(std::remove(sStreamPlayers.begin(), sStreamPlayers.end(), this), sStreamPlayers.end());
        mReadyBytes = 0;
        SYS_UnlockMutex(sStreamMutex);
    }

    if (mStream != 0)
    {
        AUD_CloseStream(mStream);
        mStream = 0;
    }

    if (mFile != nullptr)
    {
        fclose(mFile);
        mFile = nullptr;
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

    bool failed = false;
    bool finished = false;

    SYS_LockMutex(sStreamMutex);

    // Hand the chunk the reader finished to the audio stream.
    if (mReadyBytes > 0)
    {
        AUD_QueueStreamData(mStream, (const uint8_t*)mReady.data(), mReadyBytes);
        mQueuedFrames += mReadyBytes / 2;
        mReadyBytes = 0;
    }

    const uint64_t played = AUD_GetStreamPlayedFrames(mStream);
    const bool moreData = mLoop || mOffset < mSize;

    if (mReadFailed)
    {
        failed = true;
    }
    else if (moreData)
    {
        if (!mReading && mQueuedFrames < played + kStreamAheadFrames)
        {
            mWantRead = true;

            // No SD file of its own: read here; it's queued on the next Update.
            ReaderStep(true);
        }
    }
    else
    {
        // A one-shot sound is done once everything queued has played (the played count
        // advances in 1024-sample steps at 48 kHz, so allow a little slack).
        finished = !mReading && played + 1024 >= mQueuedFrames;
    }

    SYS_UnlockMutex(sStreamMutex);

    if (failed)
    {
        OctLog("FNAF1: stream read failed for %s", mPath.c_str());
    }
    if (failed || finished)
    {
        Stop();
    }
}

SoundWave* LoadPcmSound(const std::string& relPath, uint32_t sampleRate)
{
    // Copied straight from the engine's file buffer (no extra std::vector copy): RAM is
    // tight on the GameCube and a third copy made the later sounds fail to load.
    std::string path = kDataRoot + relPath;
    char* data = nullptr;
    uint32_t fileSize = 0;
    SYS_AcquireFileData(path.c_str(), true, 0, data, fileSize);

    if (data == nullptr || fileSize < 2)
    {
        LogError("FNAF1: missing data file %s", path.c_str());
        SYS_ReleaseFileData(data);
        return nullptr;
    }

    const uint32_t size = fileSize & ~1u;
    uint8_t* buffer = AUD_AllocWaveBuffer(size);
    if (buffer == nullptr)
    {
        LogError("FNAF1: could not allocate sound %s", relPath.c_str());
        SYS_ReleaseFileData(data);
        return nullptr;
    }

    memcpy(buffer, data, size);
    SYS_ReleaseFileData(data);

    SoundWave* sound = NewTransientAsset<SoundWave>();
    sound->SetName(relPath);
    sound->SetPcmData(buffer, size, size / 2, 16, 1, sampleRate);
    return sound;
}
