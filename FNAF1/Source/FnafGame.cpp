#include "FnafGame.h"

#include "Engine.h"
#include "World.h"
#include "Log.h"
#include "AudioManager.h"
#include "Assets/Texture.h"
#include "Assets/SoundWave.h"
#include "Nodes/Widgets/Widget.h"
#include "Nodes/Widgets/Quad.h"
#include "Nodes/Widgets/Text.h"
#include "Input/Input.h"
#include "System/System.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

// Layout of the original 1600x720 office, in its own pixels.
static constexpr float kOfficeWidth = 1600.0f;
static constexpr float kOfficeHeight = 720.0f;
static constexpr float kDoorX[2] = { 72.0f, 1270.0f };
static constexpr float kDoorWidth[2] = { 223.0f, 229.0f };
static constexpr float kButtonX[2] = { 6.0f, 1497.0f };
static constexpr float kButtonY[2] = { 263.0f, 273.0f };
static constexpr float kButtonWidth = 57.0f;
static constexpr float kButtonHeight = 172.0f;
static constexpr float kFanX = 780.0f;
static constexpr float kFanY = 303.0f;
static constexpr float kFanWidth = 138.0f;
static constexpr float kFanHeight = 196.0f;

// Timing
static constexpr float kHourSeconds = 89.0f;
static constexpr float kDoorSpeed = 5.0f;        // door animation, 1/seconds
static constexpr float kTabletSpeed = 4.0f;
static constexpr float kPanSpeed = 0.9f;         // office pan, screens/second
static constexpr float kStaticSeconds = 0.25f;
static constexpr float kJumpFrameSeconds = 1.0f / 24.0f;
static constexpr float kFanFrameSeconds = 1.0f / 30.0f;
static constexpr uint64_t kLoadBudgetUs = 30000;  // loading work per frame

// Prototype difficulty: a bit above the original Night 1 so things happen.
static constexpr int32_t kBonnieBaseAi = 3;
static constexpr int32_t kChicaBaseAi = 3;
static constexpr int32_t kFoxyBaseAi = 2;

// Rare camera pictures (the original rolls a "random for pic" counter): Freddy staring on
// the Show Stage, and rare posters on empty cameras. Rolled when the tablet goes up or the
// camera changes. Placeholder odds: the real value is in the game's compiled events, not
// decoded yet.
static constexpr int32_t kRarePicOdds = 20;

// Foxy
static constexpr float kFoxyMoveInterval = 5.01f;
static constexpr float kFoxyArriveSeconds = 25.0f;     // after leaving the cove, if nobody watches the hall
static constexpr float kFoxyRunFrameSeconds = 1.0f / 20.0f;

struct CameraInfo
{
    const char* mId;
    const char* mName;
};

static constexpr int32_t kNumCameras = 11;
static const CameraInfo kCameras[kNumCameras] = {
    { "1A", "Show Stage" },
    { "1B", "Dining Area" },
    { "1C", "Pirate Cove" },
    { "5", "Backstage" },
    { "7", "Restrooms" },
    { "6", "Kitchen" },
    { "2A", "W. Hall" },
    { "2B", "W. Hall Corner" },
    { "3", "Supply Closet" },
    { "4A", "E. Hall" },
    { "4B", "E. Hall Corner" },
};

static const char* kSoundNames[] = {
    "light", "door", "blip", "tablet", "scream", "windowscare", "steps", "powerdown", "run", "knock", "honk",
    "camup", "camhum", "garble1", "garble2", "garble3", "pots1", "pots2", "pots3",
};

// SD diagnostic log (Octave System_Dolphin.cpp; writes /octiso.log when the local logger is enabled).
void OctLog(const char* format, ...);

static bool Pressed(int32_t button)
{
    return INP_IsGamepadButtonJustDown(button, 0);
}

bool FnafGame::Initialize()
{
    srand((unsigned)SYS_GetTimeMicroseconds());

    mScreenWidth = (float)GetEngineState()->mWindowWidth;
    mScreenHeight = (float)GetEngineState()->mWindowHeight;

    if (!mOfficeCanvas.Init("OfficeCanvas", 992, 448) ||
        !mCameraCanvas.Init("CameraCanvas", 992, 448) ||
        !mJumpCanvas.Init("JumpCanvas", 992, 448) ||
        !mStaticCanvas.Init("StaticCanvas", 320, 176))
    {
        OctLog("FNAF1: canvas allocation FAILED");
        return false;
    }
    OctLog("FNAF1: canvases created");

    if (!ReadManifest())
    {
        OctLog("FNAF1: manifest missing");
        return false;
    }

    QueueLoadJobs();
    BuildUi();
    BuildLoadingUi();

    mState = State::Loading;
    OctLog("FNAF1: loading %u files", (unsigned)mLoadJobs.size());
    return true;
}

bool FnafGame::ReadManifest()
{
    std::vector<uint8_t> manifest;
    if (!ReadDataFile("manifest.txt", manifest))
    {
        return false;
    }

    manifest.push_back(0);
    char key[64];
    int32_t count = 0;
    const char* cursor = (const char*)manifest.data();
    int consumed = 0;
    while (sscanf(cursor, "%63s %d%n", key, &count, &consumed) == 2)
    {
        mCounts[key] = count;
        cursor += consumed;
    }
    return true;
}

void FnafGame::QueueLoadJobs()
{
    auto queueImage = [this](const std::string& name) { LoadJob job; job.mType = LoadType::Image; job.mName = name; mLoadJobs.push_back(job); };
    auto queueSprite = [this](const std::string& file, Sprite* dst) { LoadJob job; job.mType = LoadType::Sprite; job.mName = file; job.mSprite = dst; mLoadJobs.push_back(job); };

    char name[64];

    // Camera backgrounds, jumpscare frames and Foxy's run aren't loaded here: they stay on
    // the disc and are read when shown (see ShowImage), to keep RAM free for the sounds.
    // The office pictures (~300 KB, swapped every time the lights flash) and the static
    // frames, which animate all the time, are kept in RAM.
    for (const char* office : { "office", "office_light_l", "office_light_r", "office_bonnie", "office_chica", "office_dark" })
    {
        queueImage(office);
    }

    for (int32_t i = 0; i < mCounts["static"]; ++i)
    {
        snprintf(name, sizeof(name), "static_%02d", i);
        queueImage(name);
    }

    // Sprite vectors are sized up front so the job pointers stay valid.
    mFlipFrames.resize(mCounts["flip"]);
    for (int32_t i = 0; i < (int32_t)mFlipFrames.size(); ++i)
    {
        snprintf(name, sizeof(name), "spr/flip_%02d.rgx", i);
        queueSprite(name, &mFlipFrames[i]);
    }

    mFanFrames.resize(mCounts["fan"]);
    for (int32_t i = 0; i < (int32_t)mFanFrames.size(); ++i)
    {
        snprintf(name, sizeof(name), "spr/fan_%02d.rgx", i);
        queueSprite(name, &mFanFrames[i]);
    }

    for (int32_t side = 0; side < 2; ++side)
    {
        const char* key = (side == 0) ? "door_l" : "door_r";
        mDoors[side].mFrames.resize(mCounts[key]);
        for (int32_t i = 0; i < (int32_t)mDoors[side].mFrames.size(); ++i)
        {
            snprintf(name, sizeof(name), "spr/%s_%02d.rgx", key, i);
            queueSprite(name, &mDoors[side].mFrames[i]);
        }
    }

    for (int32_t side = 0; side < 2; ++side)
    {
        for (int32_t closed = 0; closed < 2; ++closed)
        {
            for (int32_t light = 0; light < 2; ++light)
            {
                snprintf(name, sizeof(name), "spr/btn_%c_c%d_l%d.rgx", side == 0 ? 'l' : 'r', closed, light);
                queueSprite(name, &mButtons[side][closed][light]);
            }
        }
    }

    for (const char* sound : kSoundNames)
    {
        LoadJob job;
        job.mType = LoadType::Sound;
        job.mName = sound;
        mLoadJobs.push_back(job);
    }
}

bool FnafGame::RunLoadJob(const LoadJob& job)
{
    switch (job.mType)
    {
    case LoadType::Image:
        return ReadDataFile("img/" + job.mName + ".jpg", mImages[job.mName]);

    case LoadType::Sprite:
        return LoadSprite(job.mName, *job.mSprite);

    case LoadType::Sound:
    {
        SoundWave* wave = LoadPcmSound("snd/" + job.mName + ".pcm", 22050);
        if (wave == nullptr)
        {
            return false;
        }
        mSounds[job.mName] = wave;
        return true;
    }
    }

    return false;
}

void FnafGame::UpdateLoading()
{
    const uint64_t start = SYS_GetTimeMicroseconds();

    while (mLoadNext < mLoadJobs.size() && SYS_GetTimeMicroseconds() - start < kLoadBudgetUs)
    {
        const LoadJob& job = mLoadJobs[mLoadNext];
        if (!RunLoadJob(job))
        {
            OctLog("FNAF1: failed to load %s", job.mName.c_str());

            // A missing sound just stays silent; missing pictures stop the game.
            if (job.mType != LoadType::Sound)
            {
                mLoadFailed = true;
            }
        }
        ++mLoadNext;
    }

    const float total = (float)glm::max<size_t>(1, mLoadJobs.size());
    const float progress = (float)mLoadNext / total;
    const float barWidth = mScreenWidth * 0.5f;
    mLoadBar->SetRect(mScreenWidth * 0.25f, mScreenHeight * 0.55f, barWidth * progress, 16.0f);

    if (mLoadNext < mLoadJobs.size())
    {
        return;
    }

    if (mLoadFailed || mDoors[0].mFrames.empty() || mDoors[1].mFrames.empty() || mFlipFrames.empty())
    {
        if (!mLoadFailedLogged)
        {
            OctLog("FNAF1: data loading FAILED");
            mLoadText->SetText("Loading failed");
            mLoadFailedLogged = true;
        }
        return;
    }

    OctLog("FNAF1: data loaded: %u images, %u sounds, free %u KB", (unsigned)mImages.size(), (unsigned)mSounds.size(), GetFreeMemoryKb());

    for (Widget* widget : { (Widget*)mLoadBack, (Widget*)mLoadBarBack, (Widget*)mLoadBar, (Widget*)mLoadText })
    {
        widget->SetVisible(false);
    }

    StartNight();
    OctLog("FNAF1: night started");
}

void FnafGame::BuildUi()
{
    World* world = GetWorld(0);
    mRoot = world->SpawnNode<Widget>();
    mRoot->SetName("FNAF1");
    mRoot->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);

    auto makeText = [this](const char* name, float x, float y, float w, float h, float size) -> Text*
    {
        Text* text = mRoot->CreateChild<Text>(name);
        text->SetRect(x, y, w, h);
        text->SetTextSize(size);
        text->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        return text;
    };

    // Draw order follows child order.
    mOffice = mRoot->CreateChild<Quad>("Office");
    mOffice->SetTexture(mOfficeCanvas.GetTexture());

    mFan = mRoot->CreateChild<Quad>("Fan");

    for (int32_t side = 0; side < 2; ++side)
    {
        mDoors[side].mQuad = mRoot->CreateChild<Quad>(side == 0 ? "DoorLeft" : "DoorRight");
        mDoors[side].mButton = mRoot->CreateChild<Quad>(side == 0 ? "ButtonLeft" : "ButtonRight");
    }

    mFlip = mRoot->CreateChild<Quad>("TabletFlip");
    mFlip->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);

    mCamera = mRoot->CreateChild<Quad>("Camera");
    mCamera->SetTexture(mCameraCanvas.GetTexture());

    // Kitchen camera: audio only, so the screen goes black.
    mCameraBlack = mRoot->CreateChild<Quad>("CameraBlack");
    mCameraBlack->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
    mCameraBlack->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    mCameraBlack->SetVisible(false);

    mStatic = mRoot->CreateChild<Quad>("Static");
    mStatic->SetTexture(mStaticCanvas.GetTexture());
    mStatic->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);

    mCameraText = makeText("CameraName", mScreenWidth - 300.0f, mScreenHeight - 70.0f, 280.0f, 60.0f, 22.0f);

    mTimeText = makeText("Time", mScreenWidth - 150.0f, 16.0f, 130.0f, 40.0f, 28.0f);
    mNightText = makeText("Night", mScreenWidth - 150.0f, 50.0f, 130.0f, 30.0f, 18.0f);
    mPowerText = makeText("Power", 24.0f, mScreenHeight - 80.0f, 300.0f, 30.0f, 20.0f);
    mUsageText = makeText("Usage", 24.0f, mScreenHeight - 50.0f, 300.0f, 30.0f, 20.0f);

    mJump = mRoot->CreateChild<Quad>("Jumpscare");
    mJump->SetTexture(mJumpCanvas.GetTexture());
    mJump->SetVisible(false);

    mMessageText = makeText("Message", 0.0f, mScreenHeight * 0.4f, mScreenWidth, 80.0f, 36.0f);
    mMessageText->SetHorizontalJustification(Justification::Center);
    mMessageText->SetVisible(false);
}

void FnafGame::BuildLoadingUi()
{
    // Drawn over everything (created last) until the data is in.
    mLoadBack = mRoot->CreateChild<Quad>("LoadBack");
    mLoadBack->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
    mLoadBack->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

    mLoadBarBack = mRoot->CreateChild<Quad>("LoadBarBack");
    mLoadBarBack->SetRect(mScreenWidth * 0.25f - 2.0f, mScreenHeight * 0.55f - 2.0f, mScreenWidth * 0.5f + 4.0f, 20.0f);
    mLoadBarBack->SetColor(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f));

    mLoadBar = mRoot->CreateChild<Quad>("LoadBar");
    mLoadBar->SetRect(mScreenWidth * 0.25f, mScreenHeight * 0.55f, 0.0f, 16.0f);
    mLoadBar->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

    mLoadText = mRoot->CreateChild<Text>("LoadText");
    mLoadText->SetRect(0.0f, mScreenHeight * 0.55f - 50.0f, mScreenWidth, 40.0f);
    mLoadText->SetTextSize(24.0f);
    mLoadText->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    mLoadText->SetHorizontalJustification(Justification::Center);
    mLoadText->SetText("Loading...");
}

void FnafGame::StartNight()
{
    AudioManager::StopAllSounds();
    StopStreams();

    mState = State::Playing;
    mNightTime = 0.0f;
    mHour = 0;
    mPower = 100.0f;
    mUsage = 1;
    mOfficePan = 0.5f;
    mTabletUp = false;
    mTabletProgress = 0.0f;
    mTabletUpTime = 0.0f;
    mCameraIndex = 0;
    mStaticTimer = 0.0f;
    mPowerOutTimer = 0.0f;
    mJumpWho.clear();

    for (Door& door : mDoors)
    {
        door.mClosed = false;
        door.mLight = false;
        door.mProgress = 0.0f;
    }

    mBonnie = Animatronic();
    mBonnie.mName = "bonnie";
    mBonnie.mLeftSide = true;
    mBonnie.mMoveInterval = 4.97f;

    mChica = Animatronic();
    mChica.mName = "chica";
    mChica.mLeftSide = false;
    mChica.mMoveInterval = 4.98f;

    mFoxyStage = 0;
    mFoxyMoveTimer = 0.0f;
    mFoxyLockTimer = 0.0f;
    mFoxyRunTimer = 0.0f;
    mFoxyRunning = false;
    mFoxyRunFrame = 0;
    mFoxyRunFrameTimer = 0.0f;
    mFoxyKnocks = 0;

    mOfficeShown.clear();
    mCameraShown.clear();
    mJump->SetVisible(false);
    ShowMessage("");

    mFanSound.Start("snd/fan.pcm", (uint32_t)mCounts["size_fan"], true, 0.6f);

    mAmbience.Start("snd/ambience.pcm", (uint32_t)mCounts["size_ambience"], true, 0.35f);
    mCall.Start("snd/call.pcm", (uint32_t)mCounts["size_call"], false, 1.0f);
    mCameraFresh = true;
    mPotsTimer = 3.0f;
    mPirateSongTimer = 4.0f;
    mCircusTimer = 5.0f;
    mCheerTimer = 0.0f;
    mLaughed = false;
}

void FnafGame::Update(float deltaTime)
{
    if (mRoot == nullptr)
    {
        return;
    }

    static uint32_t sFrames = 0;
    if (sFrames < 3 || sFrames == 60)
    {
        OctLog("FNAF1: frame %u (dt %.3f)", (unsigned)sFrames, deltaTime);
    }
    ++sFrames;

    if (mState == State::Loading)
    {
        UpdateLoading();
        return;
    }

    const uint64_t streamStart = SYS_GetTimeMicroseconds();
    mCall.Update();
    mAmbience.Update();
    mMusicBox.Update();
    mRareMusic.Update();
    mFanSound.Update();
    mJingle.Update();
    mCheer.Update();
    const uint64_t streamUs = SYS_GetTimeMicroseconds() - streamStart;

    // Timing summary every 5 s: average and worst frame, and time spent reading streams.
    static uint32_t sPerfFrames = 0;
    static float sPerfTime = 0.0f;
    static float sPerfWorst = 0.0f;
    static uint64_t sStreamUs = 0;
    static uint64_t sStreamWorstUs = 0;
    ++sPerfFrames;
    sPerfTime += deltaTime;
    sPerfWorst = glm::max(sPerfWorst, deltaTime);
    sStreamUs += streamUs;
    sStreamWorstUs = glm::max(sStreamWorstUs, streamUs);
    if (sPerfTime >= 5.0f)
    {
        OctLog("FNAF1: perf avg %.1f ms, worst %.1f ms, streams %.1f ms/frame (worst %.1f ms), call %s, free %u KB",
            sPerfTime * 1000.0f / sPerfFrames, sPerfWorst * 1000.0f,
            sStreamUs / 1000.0f / sPerfFrames, sStreamWorstUs / 1000.0f,
            mCall.IsPlaying() ? "on" : "off", GetFreeMemoryKb());
        sPerfFrames = 0;
        sPerfTime = 0.0f;
        sPerfWorst = 0.0f;
        sStreamUs = 0;
        sStreamWorstUs = 0;
    }

    deltaTime = glm::min(deltaTime, 0.1f);

    switch (mState)
    {
    case State::Playing:
    case State::PowerOut:
        UpdatePlaying(deltaTime);
        break;

    case State::Jumpscare:
        UpdateJumpscare(deltaTime);
        break;

    case State::GameOver:
    case State::Win:
        if (mCheerTimer > 0.0f)
        {
            mCheerTimer -= deltaTime;
            if (mCheerTimer <= 0.0f)
            {
                mCheer.Start("snd/cheer.pcm", (uint32_t)mCounts["size_cheer"], false, 0.9f);
            }
        }
        if (Pressed(GAMEPAD_START) || Pressed(GAMEPAD_A))
        {
            StartNight();
        }
        break;

    case State::Loading:
        break;
    }

    UpdateView(deltaTime);
    UpdateHud();
}

void FnafGame::UpdatePlaying(float deltaTime)
{
    mNightTime += deltaTime;
    const int32_t hour = (int32_t)(mNightTime / kHourSeconds);
    if (hour != mHour)
    {
        mHour = hour;
        if (mHour >= 6)
        {
            AudioManager::StopAllSounds();
            StopStreams();
            mJingle.Start("snd/chimes.pcm", (uint32_t)mCounts["size_chimes"], false, 1.0f);
            mCheerTimer = 6.0f;
            mState = State::Win;
            mTabletUp = false;
            ShowMessage("6 AM");
            return;
        }
    }

    if (mState == State::PowerOut)
    {
        mPowerOutTimer += deltaTime;
        if (mPowerOutTimer > 7.0f && !mLaughed)
        {
            mLaughed = true;
            mMusicBox.Stop();
            mJingle.Start("snd/laugh.pcm", (uint32_t)mCounts["size_laugh"], false, 1.0f);
        }
        if (mPowerOutTimer > 10.0f)
        {
            StartJumpscare("freddy");
        }
        return;
    }

    UpdateInput(deltaTime);
    UpdateTablet(deltaTime);
    UpdateAnimatronics(deltaTime);
    UpdateFoxy(deltaTime);

    if (mState != State::Playing)
    {
        return;
    }

    // Chica in the kitchen rattles pots and pans; loud when you're on CAM 6.
    if (mChica.mRoom == Room::Kitchen)
    {
        mPotsTimer -= deltaTime;
        if (mPotsTimer <= 0.0f)
        {
            static const char* kPots[] = { "pots1", "pots2", "pots3" };
            const bool watchingKitchen = mTabletUp && (Room)mCameraIndex == Room::Kitchen;
            PlaySound(kPots[rand() % 3], false, watchingKitchen ? 0.9f : 0.25f);
            mPotsTimer = 5.0f + (rand() % 400) / 100.0f;
        }
    }
    else
    {
        mPotsTimer = 1.0f;
    }

    // Rare music, as in the original's events: "every 4 s, Random(30) = 1" plays Foxy's
    // pirate song, and "every 5 s, Random(30) = 1" plays the faint circus tune. The pirate
    // song event also checks a counter we haven't mapped; here it needs Foxy in the cove.
    mPirateSongTimer -= deltaTime;
    if (mPirateSongTimer <= 0.0f)
    {
        mPirateSongTimer += 4.0f;
        if (mFoxyStage < 3 && (rand() % 30) == 0 && !mRareMusic.IsPlaying())
        {
            mRareMusicVolume = GetPirateSongVolume();
            mRareMusic.Start("snd/piratesong.pcm", (uint32_t)mCounts["size_piratesong"], false, mRareMusicVolume);
            mRareMusicIsPirate = true;
        }
    }

    mCircusTimer -= deltaTime;
    if (mCircusTimer <= 0.0f)
    {
        mCircusTimer += 5.0f;
        if ((rand() % 30) == 0 && !mRareMusic.IsPlaying())
        {
            mRareMusic.Start("snd/circus.pcm", (uint32_t)mCounts["size_circus"], false, 0.4f);
            mRareMusicIsPirate = false;
        }
    }

    // The pirate song comes from Pirate Cove: full volume while you watch it, muffled otherwise.
    if (mRareMusicIsPirate && mRareMusic.IsPlaying())
    {
        const float volume = GetPirateSongVolume();
        if (volume != mRareMusicVolume)
        {
            mRareMusicVolume = volume;
            mRareMusic.SetVolume(volume);
        }
    }

    // Power: each thing in use adds a bar.
    mUsage = 1;
    for (const Door& door : mDoors)
    {
        mUsage += door.mClosed ? 1 : 0;
        mUsage += door.mLight ? 1 : 0;
    }
    mUsage += mTabletUp ? 1 : 0;

    mPower -= deltaTime * 0.1f * mUsage;
    if (mPower <= 0.0f)
    {
        mPower = 0.0f;
        mState = State::PowerOut;
        mTabletUp = false;
        for (Door& door : mDoors)
        {
            door.mClosed = false;
            door.mLight = false;
        }
        AudioManager::StopAllSounds();
        StopStreams();
        PlaySound("powerdown");
        mMusicBox.Start("snd/musicbox.pcm", (uint32_t)mCounts["size_musicbox"], false, 0.8f);
        mLaughed = false;
    }
}

void FnafGame::UpdateInput(float deltaTime)
{
    // Office pan with the main stick (tablet down only).
    if (!mTabletUp)
    {
        float stick = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, 0);
        if (fabs(stick) > 0.2f)
        {
            mOfficePan = glm::clamp(mOfficePan + stick * kPanSpeed * deltaTime, 0.0f, 1.0f);
        }
    }

    const bool intruder = IsAt(mBonnie, Room::Office) || IsAt(mChica, Room::Office);

    // B mutes the phone call, like the original's "mute call" button.
    if (Pressed(GAMEPAD_B) && mCall.IsPlaying())
    {
        mCall.Stop();
    }

    // Z honks the Freddy poster's nose (clicking it in the original), in the office view.
    if (Pressed(GAMEPAD_Z) && !mTabletUp && mTabletProgress <= 0.0f)
    {
        PlaySound("honk");
    }

    if (Pressed(GAMEPAD_A))
    {
        mTabletUp = !mTabletUp;
        mTabletUpTime = 0.0f;
        if (mTabletUp)
        {
            PlaySound("camup");
            PlaySound("camhum", true, 0.5f);
            SetLight(true, false);
            SetLight(false, false);
            mStaticTimer = kStaticSeconds;
            mRandomForPic = (rand() % kRarePicOdds) + 1;
            mRareVariant = rand() % 4;
            mCameraFresh = true;
        }
        else
        {
            PlaySound("tablet");
            StopSound("camhum");
        }
    }

    if (mTabletUp)
    {
        if (mTabletProgress >= 1.0f)
        {
            int32_t step = 0;
            if (Pressed(GAMEPAD_RIGHT)) step = 1;
            if (Pressed(GAMEPAD_LEFT)) step = -1;
            if (step != 0)
            {
                mCameraIndex = (mCameraIndex + step + kNumCameras) % kNumCameras;
                mStaticTimer = kStaticSeconds;
                mRandomForPic = (rand() % kRarePicOdds) + 1;
                mRareVariant = rand() % 4;
                mCameraFresh = true;
                PlaySound("blip");
            }
        }
        return;
    }

    // Doors: L / R. Lights: D-pad left / right. The controls stop working once
    // someone is inside the office.
    if (intruder)
    {
        return;
    }

    for (int32_t side = 0; side < 2; ++side)
    {
        if (Pressed(side == 0 ? GAMEPAD_L1 : GAMEPAD_R1))
        {
            mDoors[side].mClosed = !mDoors[side].mClosed;
            PlaySound("door");
        }

        if (Pressed(side == 0 ? GAMEPAD_LEFT : GAMEPAD_RIGHT))
        {
            SetLight(side == 0, !mDoors[side].mLight);
        }
    }
}

void FnafGame::UpdateTablet(float deltaTime)
{
    const float target = mTabletUp ? 1.0f : 0.0f;
    if (mTabletProgress < target)
        mTabletProgress = glm::min(target, mTabletProgress + deltaTime * kTabletSpeed);
    else if (mTabletProgress > target)
        mTabletProgress = glm::max(target, mTabletProgress - deltaTime * kTabletSpeed);

    if (mTabletUp)
    {
        mTabletUpTime += deltaTime;
        mCameraPanTime += deltaTime;
    }

    for (Door& door : mDoors)
    {
        const float doorTarget = door.mClosed ? 1.0f : 0.0f;
        if (door.mProgress < doorTarget)
            door.mProgress = glm::min(doorTarget, door.mProgress + deltaTime * kDoorSpeed);
        else if (door.mProgress > doorTarget)
            door.mProgress = glm::max(doorTarget, door.mProgress - deltaTime * kDoorSpeed);
    }

    if (mStaticTimer > 0.0f)
    {
        mStaticTimer -= deltaTime;
    }
}

int32_t FnafGame::GetAi(const Animatronic& a) const
{
    // Like the original: the AI level goes up at set hours.
    if (&a == &mBonnie)
        return kBonnieBaseAi + (mHour >= 2) + (mHour >= 3) + (mHour >= 4);

    return kChicaBaseAi + (mHour >= 3) + (mHour >= 4);
}

bool FnafGame::IsAt(const Animatronic& a, Room room) const
{
    return a.mRoom == room;
}

void FnafGame::UpdateAnimatronics(float deltaTime)
{
    for (Animatronic* a : { &mBonnie, &mChica })
    {
        const int32_t side = a->mLeftSide ? 0 : 1;

        if (a->mRoom == Room::Office)
        {
            // Inside: they wait for the tablet to go down, and pull it down if you
            // keep looking at the cameras.
            a->mOfficeTimer += deltaTime;
            if (mTabletUp && mTabletUpTime > 3.0f)
            {
                mTabletUp = false;
                PlaySound("tablet");
                StopSound("camhum");
            }
            if (!mTabletUp && mTabletProgress <= 0.0f && a->mOfficeTimer > 0.8f)
            {
                StartJumpscare(a->mName);
                return;
            }
            continue;
        }

        const Room door = a->mLeftSide ? Room::LeftDoor : Room::RightDoor;
        if (a->mRoom == door && mDoors[side].mLight && !a->mSeenAtDoor)
        {
            a->mSeenAtDoor = true;
            PlaySound("windowscare");
        }

        a->mMoveTimer += deltaTime;
        if (a->mMoveTimer >= a->mMoveInterval)
        {
            a->mMoveTimer -= a->mMoveInterval;
            if ((rand() % 20) + 1 <= GetAi(*a))
            {
                MoveAnimatronic(*a);
            }
        }
    }
}

void FnafGame::MoveAnimatronic(Animatronic& a)
{
    const bool coin = (rand() & 1) != 0;
    const Room before = a.mRoom;

    if (a.mLeftSide)
    {
        switch (a.mRoom)
        {
        case Room::ShowStage:    a.mRoom = coin ? Room::DiningArea : Room::Backstage; break;
        case Room::DiningArea:   a.mRoom = coin ? Room::Backstage : Room::WestHall; break;
        case Room::Backstage:    a.mRoom = coin ? Room::DiningArea : Room::WestHall; break;
        case Room::WestHall:     a.mRoom = coin ? Room::SupplyCloset : Room::WestCorner; break;
        case Room::SupplyCloset: a.mRoom = coin ? Room::WestCorner : Room::LeftDoor; break;
        case Room::WestCorner:   a.mRoom = coin ? Room::LeftDoor : Room::SupplyCloset; break;
        case Room::LeftDoor:     a.mRoom = mDoors[0].mClosed ? Room::DiningArea : Room::Office; break;
        default: break;
        }
    }
    else
    {
        switch (a.mRoom)
        {
        case Room::ShowStage:  a.mRoom = Room::DiningArea; break;
        case Room::DiningArea: a.mRoom = coin ? Room::Restrooms : Room::Kitchen; break;
        case Room::Restrooms:  a.mRoom = coin ? Room::Kitchen : Room::EastHall; break;
        case Room::Kitchen:    a.mRoom = coin ? Room::Restrooms : Room::EastHall; break;
        case Room::EastHall:   a.mRoom = coin ? Room::EastCorner : Room::Kitchen; break;
        case Room::EastCorner: a.mRoom = coin ? Room::RightDoor : Room::EastHall; break;
        case Room::RightDoor:  a.mRoom = mDoors[1].mClosed ? Room::EastHall : Room::Office; break;
        default: break;
        }
    }

    // Only one of them fits in each doorway.
    const Animatronic& other = (&a == &mBonnie) ? mChica : mBonnie;
    if ((a.mRoom == Room::LeftDoor || a.mRoom == Room::RightDoor) && other.mRoom == a.mRoom)
    {
        a.mRoom = before;
        return;
    }

    if (a.mRoom != before)
    {
        a.mSeenAtDoor = false;
        a.mOfficeTimer = 0.0f;
        if (a.mRoom == Room::LeftDoor || a.mRoom == Room::RightDoor || a.mRoom == Room::Office)
        {
            PlaySound("steps", false, 0.7f);
        }
        LogDebug("FNAF1: %s moved to room %d", a.mName, (int)a.mRoom);
    }
}

void FnafGame::UpdateFoxy(float deltaTime)
{
    if (mState != State::Playing)
    {
        return;
    }

    if (mFoxyRunning)
    {
        // The run plays out on CAM 2A; he reaches the door when it ends.
        mFoxyRunFrameTimer += deltaTime;
        while (mFoxyRunFrameTimer >= kFoxyRunFrameSeconds)
        {
            mFoxyRunFrameTimer -= kFoxyRunFrameSeconds;
            mFoxyRunFrame++;
        }
        if (mFoxyRunFrame >= mCounts["foxyrun"])
        {
            FoxyArrive();
        }
        return;
    }

    if (mFoxyStage < 3)
    {
        // Watching the cameras keeps him in the cove, and he stays put for a while after.
        if (mTabletUp)
        {
            mFoxyLockTimer = 0.83f + (rand() % 1584) / 100.0f;
            return;
        }

        if (mFoxyLockTimer > 0.0f)
        {
            mFoxyLockTimer -= deltaTime;
            return;
        }

        mFoxyMoveTimer += deltaTime;
        if (mFoxyMoveTimer >= kFoxyMoveInterval)
        {
            mFoxyMoveTimer -= kFoxyMoveInterval;
            const int32_t ai = kFoxyBaseAi + (mHour >= 3) + (mHour >= 4);
            if ((rand() % 20) + 1 <= ai)
            {
                mFoxyStage++;
                LogDebug("FNAF1: foxy stage %d", mFoxyStage);
                if (mFoxyStage == 3)
                {
                    mFoxyRunTimer = kFoxyArriveSeconds;
                }
            }
        }
        return;
    }

    // Out of the cove: checking the West Hall sets him running, otherwise he comes anyway.
    const bool watchingWestHall = mTabletUp && mTabletProgress >= 1.0f && (Room)mCameraIndex == Room::WestHall;
    if (watchingWestHall)
    {
        mFoxyRunning = true;
        mFoxyRunFrame = 0;
        mFoxyRunFrameTimer = 0.0f;
        PlaySound("run");
        return;
    }

    mFoxyRunTimer -= deltaTime;
    if (mFoxyRunTimer <= 0.0f)
    {
        PlaySound("run");
        FoxyArrive();
    }
}

void FnafGame::FoxyArrive()
{
    mFoxyRunning = false;

    if (mDoors[0].mClosed)
    {
        // Bangs on the door, drains power (more each time), and goes back to the cove.
        PlaySound("knock");
        mPower = glm::max(0.0f, mPower - (1.0f + 5.0f * mFoxyKnocks));
        mFoxyKnocks++;
        mFoxyStage = rand() % 2;
        mFoxyMoveTimer = 0.0f;
        LogDebug("FNAF1: foxy knocked (%d)", mFoxyKnocks);
        return;
    }

    StartJumpscare("foxy");
}

void FnafGame::SetLight(bool left, bool on)
{
    Door& door = mDoors[left ? 0 : 1];
    Door& other = mDoors[left ? 1 : 0];

    if (on && other.mLight)
    {
        other.mLight = false;
    }

    door.mLight = on;

    if (mDoors[0].mLight || mDoors[1].mLight)
    {
        if (!AudioManager::IsSoundPlaying(mSounds["light"].Get<SoundWave>()))
        {
            PlaySound("light", true, 1.0f);
        }
    }
    else
    {
        StopSound("light");
    }
}

void FnafGame::StartJumpscare(const std::string& who)
{
    AudioManager::StopAllSounds();
    StopStreams();
    PlaySound("scream");

    mState = State::Jumpscare;
    mTabletUp = false;
    mTabletProgress = 0.0f;
    mJumpWho = who;
    mJumpFrame = -1;
    mJumpTimer = kJumpFrameSeconds;
    mJump->SetVisible(true);
}

void FnafGame::UpdateJumpscare(float deltaTime)
{
    const int32_t frames = mCounts["jump_" + mJumpWho];
    mJumpTimer += deltaTime;

    if (mJumpTimer >= kJumpFrameSeconds && mJumpFrame + 1 < frames)
    {
        mJumpTimer = 0.0f;
        mJumpFrame++;
        char name[64];
        snprintf(name, sizeof(name), "jump_%s_%02d", mJumpWho.c_str(), mJumpFrame);
        std::string shown;
        ShowImage(mJumpCanvas, name, shown);
    }
    else if (mJumpFrame + 1 >= frames && mJumpTimer > 0.6f)
    {
        mJump->SetVisible(false);
        mState = State::GameOver;
        ShowMessage("GAME OVER\nPress START");
    }
}

std::string FnafGame::GetOfficeImage() const
{
    if (mState == State::PowerOut)
        return "office_dark";

    if (mDoors[0].mLight)
        return IsAt(mBonnie, Room::LeftDoor) ? "office_bonnie" : "office_light_l";

    if (mDoors[1].mLight)
        return IsAt(mChica, Room::RightDoor) ? "office_chica" : "office_light_r";

    return "office";
}

float FnafGame::GetPirateSongVolume() const
{
    const bool watchingCove = mTabletUp && mTabletProgress >= 1.0f && (Room)mCameraIndex == Room::PirateCove;
    return watchingCove ? 0.6f : 0.12f;
}

std::string FnafGame::GetCameraImage(Room camera) const
{
    const bool bonnie = IsAt(mBonnie, camera);
    const bool chica = IsAt(mChica, camera);

    switch (camera)
    {
    case Room::ShowStage:
        if (bonnie && chica) return "cam1a_all";
        if (bonnie) return "cam1a_no_chica";
        if (chica) return "cam1a_no_bonnie";
        return (mRandomForPic == 1) ? "cam1a_freddy_stare" : "cam1a_freddy";
    case Room::DiningArea:   return bonnie ? "cam1b_bonnie" : (chica ? "cam1b_chica" : "cam1b_empty");
    case Room::PirateCove:
    {
        // Curtains closed: rarely, an "IT'S ME" sign instead of the usual one.
        if (mFoxyStage <= 0 && mRandomForPic == 1)
        {
            return "cam1c_rare_itsme";
        }

        char name[16];
        snprintf(name, sizeof(name), "cam1c_%d", glm::clamp(mFoxyStage, 0, 3));
        return name;
    }
    case Room::Backstage:    return bonnie ? "cam5_bonnie" : "cam5_empty";
    case Room::Restrooms:    return chica ? "cam7_chica" : "cam7_empty";
    case Room::Kitchen:      return "";
    case Room::WestHall:
        if (mFoxyRunning)
        {
            char name[24];
            snprintf(name, sizeof(name), "foxyrun_%02d", glm::clamp(mFoxyRunFrame, 0, glm::max(0, mCounts.at("foxyrun") - 1)));
            return name;
        }
        return bonnie ? "cam2a_bonnie" : "cam2a_empty";
    case Room::SupplyCloset: return bonnie ? "cam3_bonnie" : "cam3_empty";
    // Rare posters: roll 1 or 2 swaps an empty camera's poster.
    case Room::WestCorner:
        if (bonnie) return "cam2b_bonnie";
        if (mRandomForPic == 1) return "cam2b_rare_freddy";
        if (mRandomForPic == 2) return "cam2b_rare_golden";
        return "cam2b_empty";
    case Room::EastHall:
        if (chica) return "cam4a_chica";
        if (mRandomForPic == 1) return "cam4a_rare_faces";
        if (mRandomForPic == 2) return "cam4a_rare_itsme";
        return "cam4a_empty";
    case Room::EastCorner:
        if (chica) return "cam4b_chica";
        if (mRandomForPic == 1)
        {
            // One of four newspaper clippings.
            static const char* kNews[] = { "cam4b_rare_news0", "cam4b_rare_news1", "cam4b_rare_news2", "cam4b_rare_news3" };
            return kNews[glm::clamp(mRareVariant, 0, 3)];
        }
        return "cam4b_empty";
    default:                 return "";
    }
}

void FnafGame::ShowImage(YuvCanvas& canvas, const std::string& name, std::string& shown)
{
    if (name == shown)
    {
        return;
    }

    auto it = mImages.find(name);
    if (it != mImages.end())
    {
        if (canvas.Show(it->second))
        {
            shown = name;
        }
        return;
    }

    // Backgrounds and animation frames (jumpscares, Foxy's run) are read from the disc when shown.
    if (ReadDataFile("img/" + name + ".jpg", mFrameBuffer) && canvas.Show(mFrameBuffer))
    {
        shown = name;
    }
}

void FnafGame::UpdateView(float deltaTime)
{
    const float scale = mScreenHeight / kOfficeHeight;
    const float officeWidth = kOfficeWidth * scale;
    const float panX = mOfficePan * glm::max(0.0f, officeWidth - mScreenWidth);

    // Office, fan, doors and buttons
    ShowImage(mOfficeCanvas, GetOfficeImage(), mOfficeShown);
    mOffice->SetRect(-panX, 0.0f, officeWidth, mScreenHeight);

    const bool fanOn = (mState != State::PowerOut) && !mFanFrames.empty();
    mFan->SetVisible(fanOn);
    if (fanOn)
    {
        mFanTime += deltaTime;
        const int32_t frame = (int32_t)(mFanTime / kFanFrameSeconds) % (int32_t)mFanFrames.size();
        mFan->SetTexture(mFanFrames[frame].Get());
        mFan->SetRect(kFanX * scale - panX, kFanY * scale, kFanWidth * scale, kFanHeight * scale);
    }

    for (int32_t side = 0; side < 2; ++side)
    {
        Door& door = mDoors[side];
        const int32_t last = (int32_t)door.mFrames.size() - 1;
        const int32_t frame = glm::clamp((int32_t)lround(door.mProgress * last), 0, last);
        door.mQuad->SetTexture(door.mFrames[frame].Get());
        door.mQuad->SetRect(kDoorX[side] * scale - panX, 0.0f, kDoorWidth[side] * scale, mScreenHeight);

        door.mButton->SetTexture(mButtons[side][door.mClosed ? 1 : 0][door.mLight ? 1 : 0].Get());
        door.mButton->SetRect(kButtonX[side] * scale - panX, kButtonY[side] * scale, kButtonWidth * scale, kButtonHeight * scale);
        door.mButton->SetVisible(mState != State::PowerOut);
    }

    // Tablet flip
    const bool flipping = mTabletProgress > 0.0f && mTabletProgress < 1.0f;
    mFlip->SetVisible(flipping && !mFlipFrames.empty());
    if (flipping && !mFlipFrames.empty())
    {
        const int32_t last = (int32_t)mFlipFrames.size() - 1;
        const int32_t frame = glm::clamp((int32_t)(mTabletProgress * last), 0, last);
        mFlip->SetTexture(mFlipFrames[frame].Get());
    }

    // Cameras
    const bool cameraOn = mTabletUp && mTabletProgress >= 1.0f && mState == State::Playing;
    const Room room = (Room)mCameraIndex;
    const std::string cameraImage = cameraOn ? GetCameraImage(room) : "";

    mCamera->SetVisible(cameraOn && !cameraImage.empty());
    mCameraBlack->SetVisible(cameraOn && cameraImage.empty());
    if (cameraOn && !cameraImage.empty())
    {
        if (cameraImage != mCameraShown)
        {
            // A view change flashes static, except between frames of Foxy's run.
            const bool runFrame = cameraImage.compare(0, 7, "foxyrun") == 0 && mCameraShown.compare(0, 7, "foxyrun") == 0;
            ShowImage(mCameraCanvas, cameraImage, mCameraShown);
            if (!runFrame)
            {
                mStaticTimer = glm::max(mStaticTimer, 0.1f);

                // Someone moved on the camera you're watching: the feed garbles.
                if (!mCameraFresh)
                {
                    static const char* kGarbles[] = { "garble1", "garble2", "garble3" };
                    PlaySound(kGarbles[rand() % 3], false, 0.7f);
                }
            }
        }
        mCameraFresh = false;

        // The camera slowly pans back and forth.
        const float pan = (sin(mCameraPanTime * 0.35f) * 0.5f + 0.5f) * glm::max(0.0f, officeWidth - mScreenWidth);
        mCamera->SetRect(-pan, 0.0f, officeWidth, mScreenHeight);
    }

    mCameraText->SetVisible(cameraOn);
    if (cameraOn)
    {
        char label[96];
        if (room == Room::Kitchen)
            snprintf(label, sizeof(label), "CAM %s  %s\n-CAMERA DISABLED-\nAUDIO ONLY", kCameras[mCameraIndex].mId, kCameras[mCameraIndex].mName);
        else
            snprintf(label, sizeof(label), "CAM %s  %s", kCameras[mCameraIndex].mId, kCameras[mCameraIndex].mName);
        mCameraText->SetText(label);
    }

    // Static bursts when switching cameras or when the view changes.
    const bool staticOn = cameraOn && mStaticTimer > 0.0f;
    mStatic->SetVisible(staticOn);
    if (staticOn)
    {
        mStaticFrameTimer -= deltaTime;
        if (mStaticFrameTimer <= 0.0f)
        {
            mStaticFrameTimer = 0.05f;
            mStaticFrame = (mStaticFrame + 1) % glm::max(1, mCounts["static"]);
            char name[32];
            snprintf(name, sizeof(name), "static_%02d", mStaticFrame);
            std::string unused;
            ShowImage(mStaticCanvas, name, unused);
        }
    }

    // Jumpscare: centered on the current view.
    if (mJump->IsVisible())
    {
        mJump->SetRect(-panX, 0.0f, officeWidth, mScreenHeight);
    }
}

void FnafGame::UpdateHud()
{
    const bool playing = (mState == State::Playing || mState == State::PowerOut);
    mTimeText->SetVisible(playing);
    mNightText->SetVisible(playing);
    mPowerText->SetVisible(playing);
    mUsageText->SetVisible(playing && mState != State::PowerOut);

    if (!playing)
    {
        return;
    }

    char text[64];
    snprintf(text, sizeof(text), "%d AM", mHour == 0 ? 12 : mHour);
    mTimeText->SetText(text);
    mNightText->SetText("Night 1");

    snprintf(text, sizeof(text), "Power left: %d%%", (int)ceil(mPower));
    mPowerText->SetText(text);

    std::string usage = "Usage: ";
    for (int32_t i = 0; i < mUsage; ++i)
    {
        usage += "[] ";
    }
    mUsageText->SetText(usage);
}

void FnafGame::ShowMessage(const std::string& message)
{
    mMessageText->SetText(message);
    mMessageText->SetVisible(!message.empty());
}

void FnafGame::StopStreams()
{
    mCall.Stop();
    mAmbience.Stop();
    mMusicBox.Stop();
    mRareMusic.Stop();
    mFanSound.Stop();
    mJingle.Stop();
    mCheer.Stop();
}

void FnafGame::PlaySound(const char* name, bool loop, float volume)
{
    auto it = mSounds.find(name);
    if (it != mSounds.end())
    {
        AudioManager::PlaySound2D(it->second.Get<SoundWave>(), volume, 1.0f, 0.0f, loop);
    }
}

void FnafGame::StopSound(const char* name)
{
    auto it = mSounds.find(name);
    if (it != mSounds.end())
    {
        AudioManager::StopSounds(it->second.Get<SoundWave>());
    }
}
