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

// Octave System_Dolphin.cpp: take/release the engine's file I/O lock. All SD access must go
// through it; the SD driver hangs when two threads use the card at once.
void OctLockFileIo();
void OctUnlockFileIo();

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
    // The header first, then the texels straight into the texture's memory: no second copy of
    // the file in RAM, so a sprite loaded mid-night needs only one block the texture's size.
    uint8_t header[8];
    if (!ReadDataRange(relPath, 0, sizeof(header), (char*)header))
    {
        return false;
    }

    if (memcmp(header, "RGX8", 4) != 0)
    {
        LogError("FNAF1: %s is not an RGX8 sprite", relPath.c_str());
        return false;
    }

    const uint32_t width = (uint32_t(header[4]) << 8) | header[5];
    const uint32_t height = (uint32_t(header[6]) << 8) | header[7];
    const uint32_t texelBytes = width * height * 4;

    if (width == 0 || height == 0 || (width % 4) != 0 || (height % 4) != 0)
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

    // The file already holds GX_TF_RGBA8 texels, so they're read in as-is.
    if (!ReadDataRange(relPath, sizeof(header), texelBytes, (char*)resource->mDynamicData))
    {
        LogError("FNAF1: could not read sprite %s", relPath.c_str());
        return false;
    }
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
static constexpr uint64_t kStreamAheadFrames = kStreamRate;            // keep ~1 s queued (the engine buffers it in RAM)

static uint32_t sStreamUnderruns = 0;

uint32_t GetStreamUnderruns()
{
    return sStreamUnderruns;
}

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
static std::unordered_map<std::string, uint32_t> sIsoSizes;
static bool sIsoScanned = false;

static uint32_t ReadBe32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static void ScanIsoLocked()
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
                sIsoSizes[name] = ReadBe32(entry + 8);
            }
        }

        sIsoPath = path;
        OctLog("FNAF1: streams read %s directly (%u files)", path, (unsigned)sIsoOffsets.size());
        return;
    }
}

static void ScanIso()
{
    OctLockFileIo();
    ScanIsoLocked();
    OctUnlockFileIo();
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

// Animation frames (jumpscares, Foxy's run) through their own handle on FNAF1.iso. Through the
// engine's shared handle every frame paid a backwards seek (libfat walks the cluster chain from
// the start), 50-100 ms per frame on hardware. A frame's files sit one after another in the ISO,
// so this handle mostly reads straight on without seeking.
static FILE* sAnimFile = nullptr;
static uint32_t sAnimFilePos = UINT32_MAX;

bool ReadAnimationFrame(const std::string& relPath, std::vector<uint8_t>& out)
{
    uint32_t base = 0;
    if (!FindIsoFile(relPath, base))
    {
        return ReadDataFile(relPath, out);      // disc boots (Dolphin) and loose files
    }

    std::string key;
    for (char c : std::string(kDataRoot) + relPath)
    {
        key += (char)tolower((unsigned char)c);
    }
    const uint32_t size = sIsoSizes[key];
    if (size == 0)
    {
        return ReadDataFile(relPath, out);
    }

    out.resize(size);
    OctLockFileIo();
    if (sAnimFile == nullptr)
    {
        sAnimFile = fopen(sIsoPath.c_str(), "rb");
        sAnimFilePos = UINT32_MAX;
    }
    // Files are padded to 4 bytes in the ISO, so the next frame usually starts a few bytes on:
    // read through that gap rather than seek.
    bool ok = sAnimFile != nullptr;
    if (ok && sAnimFilePos != base)
    {
        char gap[64];
        const bool shortGap = sAnimFilePos < base && base - sAnimFilePos <= sizeof(gap);
        ok = shortGap ? fread(gap, 1, base - sAnimFilePos, sAnimFile) == base - sAnimFilePos
                      : fseek(sAnimFile, long(base), SEEK_SET) == 0;
    }
    ok = ok && fread(out.data(), 1, size, sAnimFile) == size;
    sAnimFilePos = ok ? base + size : UINT32_MAX;
    OctUnlockFileIo();

    if (!ok)
    {
        return ReadDataFile(relPath, out);
    }
    return true;
}

// One reader thread serves every PcmPlayer. The main thread only moves finished chunks
// into the engine's audio streams, so a slow SD read never lands on a frame.
static MutexObject* sStreamMutex = nullptr;
static std::vector<PcmPlayer*> sStreamPlayers;

// Animation read-ahead: while a jumpscare or Foxy's run plays, the reader thread keeps the next
// few frames read, so the main thread only decodes (on hardware a frame read was 20-40 ms on top
// of a 25-30 ms decode). Slot buffers keep their capacity, so frames don't churn the heap.
static constexpr size_t kAnimAhead = 4;
struct AnimSlot
{
    size_t mIndex = 0;
    bool mReady = false;
    std::vector<uint8_t> mData;
};
static MutexObject* sAnimMutex = nullptr;
static std::vector<std::string> sAnimPaths;
static AnimSlot sAnimSlots[kAnimAhead];
static size_t sAnimNext = 0;            // next frame for the reader
static size_t sAnimReading = SIZE_MAX;  // frame the reader is reading right now
static uint32_t sAnimGeneration = 0;    // bumped by start/stop, so a read in flight is dropped

static bool AnimReaderStep()
{
    if (sAnimMutex == nullptr)
    {
        return false;
    }

    SYS_LockMutex(sAnimMutex);
    int32_t freeSlot = -1;
    for (size_t i = 0; i < kAnimAhead; ++i)
    {
        if (!sAnimSlots[i].mReady)
        {
            freeSlot = int32_t(i);
            break;
        }
    }
    if (freeSlot < 0 || sAnimNext >= sAnimPaths.size())
    {
        SYS_UnlockMutex(sAnimMutex);
        return false;
    }

    const size_t index = sAnimNext;
    const std::string path = sAnimPaths[index];
    const uint32_t generation = sAnimGeneration;
    std::vector<uint8_t> data;
    data.swap(sAnimSlots[freeSlot].mData);  // read into the slot's buffer (not ready, so nobody takes it)
    sAnimReading = index;
    SYS_UnlockMutex(sAnimMutex);

    const bool ok = ReadAnimationFrame(path, data);

    SYS_LockMutex(sAnimMutex);
    sAnimReading = SIZE_MAX;
    AnimSlot& slot = sAnimSlots[freeSlot];
    slot.mData.swap(data);
    if (generation == sAnimGeneration && index == sAnimNext)
    {
        slot.mIndex = index;
        slot.mReady = ok;
        sAnimNext = ok ? index + 1 : sAnimPaths.size();
    }
    SYS_UnlockMutex(sAnimMutex);
    return true;
}

void AnimPreloadStart(const std::vector<std::string>& relPaths, size_t first)
{
    if (sAnimMutex == nullptr)
    {
        return;
    }

    SYS_LockMutex(sAnimMutex);
    sAnimPaths = relPaths;
    sAnimNext = first;
    sAnimGeneration++;
    for (AnimSlot& slot : sAnimSlots)
    {
        slot.mReady = false;
    }
    SYS_UnlockMutex(sAnimMutex);
}

void AnimPreloadStop()
{
    if (sAnimMutex == nullptr)
    {
        return;
    }

    SYS_LockMutex(sAnimMutex);
    sAnimPaths.clear();
    sAnimNext = 0;
    sAnimGeneration++;
    for (AnimSlot& slot : sAnimSlots)
    {
        slot.mReady = false;
        std::vector<uint8_t>().swap(slot.mData);    // give the memory back
    }
    SYS_UnlockMutex(sAnimMutex);
}

bool AnimPreloadTake(const std::string& relPath, std::vector<uint8_t>& out)
{
    if (sAnimMutex == nullptr)
    {
        return false;
    }

    SYS_LockMutex(sAnimMutex);
    bool found = false;
    for (size_t index = 0; index < sAnimPaths.size(); ++index)
    {
        if (sAnimPaths[index] != relPath)
        {
            continue;
        }

        // The reader is reading this very frame: wait for it rather than read it a second time
        // (on the one CPU core that doubled the cost of the run's later frames).
        for (int32_t waited = 0; sAnimReading == index && waited < 250; ++waited)
        {
            SYS_UnlockMutex(sAnimMutex);
            SYS_Sleep(1);
            SYS_LockMutex(sAnimMutex);
        }

        for (AnimSlot& slot : sAnimSlots)
        {
            if (!slot.mReady)
            {
                continue;
            }
            if (slot.mIndex == index)
            {
                out.swap(slot.mData);
                slot.mReady = false;
                found = true;
            }
            else if (slot.mIndex < index)
            {
                slot.mReady = false;    // skipped past it
            }
        }
        // The reader carries on after this frame (if the main thread had to read it itself, the
        // reader's copy in flight is dropped).
        if (sAnimNext <= index)
        {
            sAnimNext = index + 1;
            sAnimGeneration++;
        }
        break;
    }
    SYS_UnlockMutex(sAnimMutex);
    return found;
}

static ThreadFuncRet StreamReaderMain(void* arg)
{
    // Below the main thread (priority 64), like VideoStream: reads run while it waits for vsync.
    // Both SD reads and the engine's DVD reads busy-wait (IsoDvd_Dolphin polls DI_CR), so a
    // higher priority would hold up frames.
    LWP_SetThreadPriority(LWP_GetSelf(), 40);

    for (;;)
    {
        // Animation frames first: the streams keep about a second of audio queued, while a frame
        // is needed within one or two video frames.
        bool worked = AnimReaderStep();

        SYS_LockMutex(sStreamMutex);
        for (size_t i = 0; i < sStreamPlayers.size(); ++i)
        {
            worked = sStreamPlayers[i]->ReaderStep() || worked;
        }
        SYS_UnlockMutex(sStreamMutex);

        if (!worked)
        {
            SYS_Sleep(2);
        }
    }

    THREAD_RETURN();
}

void StartReaderThread()
{
    if (sStreamMutex != nullptr)
    {
        return;
    }

    sStreamMutex = SYS_CreateMutex();
    sAnimMutex = SYS_CreateMutex();
    // Created directly for a bigger stack: SYS_CreateThread gives 16 KB, and on hardware the
    // reader hung at the same read every time (fread -> libfat -> SD driver, plus logging),
    // which looks like a stack overflow corrupting memory.
    static lwp_t sReaderThread = LWP_THREAD_NULL;
    LWP_CreateThread(&sReaderThread, StreamReaderMain, nullptr, nullptr, 64 * 1024, 40);
}

bool PcmPlayer::ReaderStep()
{
    // Disc boots (no own SD file) read through the engine here too. That needs the engine's
    // whole-file DVD read to take its ISO mutex (local Octave change in System_Dolphin.cpp);
    // without it a DVD read from this thread could overlap one on the main thread.
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
    const uint32_t bytes = glm::min(kStreamChunkBytes * mChannels, mSize - offset);
    mReading = true;

    // Read without the lock; Stop() waits for mReading to clear before closing mFile.
    SYS_UnlockMutex(sStreamMutex);

    bool ok = false;
    if (mFile != nullptr)
    {
        // Only seeks when looping back to the start. Holds the engine's file I/O lock so it never
        // overlaps the main thread's SD use (log writes, picture loads).
        OctLockFileIo();
        ok = (mFilePos == offset || fseek(mFile, long(mFileBase + offset), SEEK_SET) == 0) &&
             fread(mReady.data(), 1, bytes, mFile) == bytes;
        OctUnlockFileIo();
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

bool PcmPlayer::Start(const std::string& relPath, uint32_t sizeBytes, bool loop, float volume, uint32_t channels)
{
    Stop();

    if (sizeBytes < 2)
    {
        OctLog("FNAF1: stream %s has no data (missing from the manifest?)", relPath.c_str());
        return false;
    }

    mChannels = (channels == 2) ? 2 : 1;
    mStream = AUD_OpenStream(kStreamRate, mChannels);
    if (mStream == 0)
    {
        OctLog("FNAF1: no free audio stream for %s", relPath.c_str());
        return false;
    }

    // Booted from FNAF1.iso on the SD: read it through our own handle. Otherwise (disc,
    // Dolphin) go through the engine.
    uint32_t base = 0;
    if (FindIsoFile(relPath, base))
    {
        OctLockFileIo();
        mFile = fopen(sIsoPath.c_str(), "rb");
        OctUnlockFileIo();
        mFileBase = base;
        mFilePos = UINT32_MAX;
    }

    mPath = relPath;
    mSize = sizeBytes & ~(2u * mChannels - 1u);     // whole frames only
    mQueuedFrames = 0;
    mLoop = loop;
    mUnderrun = false;
    mOffset = 0;
    mReading = false;
    mReadFailed = false;
    mReady.resize(kStreamChunkBytes * mChannels);
    mReadyBytes = 0;

    StartReaderThread();

    SYS_LockMutex(sStreamMutex);
    sStreamPlayers.push_back(this);
    mWantRead = true;
    SYS_UnlockMutex(sStreamMutex);

    OctLog("FNAF1: stream start %s (stream %u, %s, free %u KB)", relPath.c_str(), mStream,
           mFile != nullptr ? "own SD file" : "engine read", GetFreeMemoryKb());
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
        OctLog("FNAF1: stream stop %s", mPath.c_str());
        AUD_CloseStream(mStream);
        mStream = 0;
    }

    if (mFile != nullptr)
    {
        OctLockFileIo();
        fclose(mFile);
        OctUnlockFileIo();
        mFile = nullptr;
    }
}

void PcmPlayer::SetVolume(float volume)
{
    if (mStream != 0)
    {
        AUD_SetStreamVolume(mStream, volume);
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
        mQueuedFrames += mReadyBytes / (2 * mChannels);
        mReadyBytes = 0;
    }

    const uint64_t played = AUD_GetStreamPlayedFrames(mStream);
    const bool moreData = mLoop || mOffset < mSize;

    // Everything queued has played but more is coming: the reader fell behind (a gap).
    const bool underrun = moreData && mQueuedFrames > 0 && played >= mQueuedFrames;
    if (underrun && !mUnderrun)
    {
        sStreamUnderruns++;
        OctLog("FNAF1: stream underrun in %s at %.1f s", mPath.c_str(), played / 22050.0f);
    }
    mUnderrun = underrun;

    if (mReadFailed)
    {
        failed = true;
    }
    else if (moreData)
    {
        if (!mReading && mQueuedFrames < played + kStreamAheadFrames)
        {
            mWantRead = true;
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
