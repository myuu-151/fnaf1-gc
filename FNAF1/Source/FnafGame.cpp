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
// The original's clock: a minute counter that starts at 0, ticks every second and starts the next
// hour at 90, resetting to 1. So the first hour is 90 s and the rest 89 s (6 AM at 535 s).
static constexpr float kFirstHourSeconds = 90.0f;
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
static constexpr int32_t kFoxyBaseAi = 5;      // rolls 1..20 every 5 s; raised from 2 for a livelier night 1

// Rare camera pictures (the original rolls a "random for pic" counter): Freddy staring on
// the Show Stage, and rare posters on empty cameras. Rolled when the tablet goes up or the
// camera changes. Placeholder odds: the real value is in the game's compiled events, not
// decoded yet.

// Main menu timing
static constexpr float kMenuFrameSeconds = 0.08f;   // Freddy's face frame and flicker
static constexpr float kNewspaperSeconds = 5.0f;    // help-wanted ad after New Game
static constexpr float kNightIntroSeconds = 2.5f;   // "12:00 AM / 1st Night"
static constexpr float kGameOverStaticSeconds = 10.8f;  // static before the game over screen (the static sound's length)
static constexpr float kGameOverSeconds = 10.0f;        // game over screen, then the menu

// Foxy
static constexpr float kFoxyMoveInterval = 5.01f;
static constexpr float kFoxyArriveSeconds = 25.0f;     // after leaving the cove, if nobody watches the hall
static constexpr float kFoxyRunFrameSeconds = 1.0f / 39.0f;  // animation speed 65 at 60 fps
static constexpr float kFoxyRunSeconds = 100.0f / 60.0f;  // the original's run: 100 frames at 60 fps

// Load cost of animation frames (jumpscares, Foxy's run): summed while one plays, logged when it ends.
struct AnimLoadStats
{
    uint32_t frames = 0;
    uint64_t readUs = 0;
    uint64_t decodeUs = 0;
    uint64_t worstUs = 0;
};
static AnimLoadStats sAnimStats;

void OctLog(const char* format, ...);

static void LogAnimStats(const char* label)
{
    if (sAnimStats.frames > 0)
    {
        OctLog("FNAF1: %s: %u frames loaded, read avg %.1f ms, decode avg %.1f ms, worst frame %.1f ms", label,
               sAnimStats.frames, sAnimStats.readUs / 1000.0f / sAnimStats.frames,
               sAnimStats.decodeUs / 1000.0f / sAnimStats.frames, sAnimStats.worstUs / 1000.0f);
    }
    sAnimStats = AnimLoadStats();
}

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
    { "3", "Supply Closet" },       // same order as the Room enum (was swapped with 2B)
    { "2B", "W. Hall Corner" },
    { "4A", "E. Hall" },
    { "4B", "E. Hall Corner" },
};

static const char* kSoundNames[] = {
    "light", "door", "blip", "tablet", "scream", "windowscare", "steps", "run", "knock", "foxybang", "honk",
    "camup", "camhum", "garble1", "garble2", "garble3", "pots1", "pots2", "pots3", "pots4", "error",
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
    BuildMenuUi();
    BuildLoadingUi();

    // The jumpscare is created last so it draws in front of everything (child order is draw order).
    mJump = mRoot->CreateChild<Quad>("Jumpscare");
    mJump->SetTexture(mJumpCanvas.GetTexture());
    mJump->SetVisible(false);

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
    // So are CAM 2A's three pictures, which the hall light's flicker swaps several times a second.
    for (const char* office : { "office", "office_light_l", "office_light_r", "office_bonnie", "office_chica", "office_dark", "office_freddy_dark",
                                "cam2a_dark", "cam2a_empty", "cam2a_bonnie" })
    {
        queueImage(office);
    }

    // Main menu: Freddy's face frames (swapped every few frames, so kept in RAM) and the text.
    for (int32_t i = 0; i < 4; ++i)
    {
        snprintf(name, sizeof(name), "menu_freddy%d", i);
        queueImage(name);
    }
    queueSprite("spr/menu_title.rgx", &mMenuTitleSprite);
    queueSprite("spr/menu_newgame.rgx", &mMenuNewGameSprite);
    queueSprite("spr/menu_continue.rgx", &mMenuContinueSprite);
    queueSprite("spr/menu_arrows.rgx", &mMenuArrowsSprite);
    queueSprite("spr/menu_copyright.rgx", &mMenuCopyrightSprite);
    queueSprite("spr/menu_clock.rgx", &mIntroClockSprite);
    queueSprite("spr/menu_first.rgx", &mIntroFirstSprite);
    queueSprite("spr/menu_night.rgx", &mIntroNightSprite);
    queueSprite("spr/menu_gameover.rgx", &mGameOverSprite);

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

    EnterMenu();
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
    mDebugText = makeText("Debug", 16.0f, 12.0f, 460.0f, 24.0f, 14.0f);   // debug: ambience layer and rooms


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
    ShowMenuWidgets(false, false, false);
    OctLog("FNAF1: night started");

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
    for (Animatronic* a : { &mBonnie, &mChica })
    {
        a->mPose = 1;
        a->mAttackArmed = false;
        a->mTabletUpInside = 0.0f;
    }

    mFoxyStage = 0;
    mFoxyMoveTimer = 0.0f;
    mFoxyLockTimer = 0.0f;
    mFoxyRunTimer = 0.0f;
    mFoxyRunning = false;
    mFoxyRunFrame = 0;
    mFoxyRunFrameTimer = 0.0f;
    mFoxyKnocks = 0;
    mFoxyAtDoor = false;
    mRandomForPic = 0;      // the original's counter starts at 0 and is only rolled when the tablet goes down

    mOfficeShown.clear();
    mCameraShown.clear();
    mJump->SetVisible(false);
    ShowMessage("");

    mFanSound.Start("snd/fan.pcm", (uint32_t)mCounts["size_fan"], true, 0.6f);

    // As in the original, both ambience loops start with the night and are never restarted
    // (so their full tracks play); only the eerie one's volume changes. The dark ambience is
    // its channel 2 at volume 50, the eerie ambience channel 18 starting at 0. (Its robotvoice
    // loop also starts here, but is heard only when the animatronics glitch on the cameras.)
    mAmbience.Start("snd/darkambience.pcm", (uint32_t)mCounts["size_darkambience"], true, 1.5f);
    mEerieVolume = 0.0f;
    mAmbienceLayer = -1;
    mEerie.Start("snd/eerie.pcm", (uint32_t)mCounts["size_eerie"], true, 0.0f);
    mCall.Start("snd/call.pcm", (uint32_t)mCounts["size_call"], false, 1.0f);
    mCameraFresh = true;
    mPotsTimer = 3.0f;
    mPirateSongTimer = 4.0f;
    mCircusTimer = 5.0f;
    mPoundingTimer = 10.0f;
    mGroanTimer = 5.0f;
    mCameraCutTimer = 0.0f;
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
    mMenuMusic.Update();
    mMenuHum.Update();
    mEerie.Update();
    mBreath.Update();
    mTapeSound.Update();
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

    // Crash breadcrumb: every state change goes to the log.
    static const char* kStateNames[] = { "Loading", "Menu", "Newspaper", "NightIntro", "Playing", "PowerOut", "Jumpscare", "GameOver", "Win" };
    static int32_t sLoggedState = -1;
    if ((int32_t)mState != sLoggedState)
    {
        sLoggedState = (int32_t)mState;
        OctLog("FNAF1: state -> %s (hour %d, power %.0f, free %u KB)", kStateNames[sLoggedState], mHour, mPower, GetFreeMemoryKb());
    }

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
        UpdateGameOver(deltaTime);
        break;

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
            EnterMenu();
        }
        break;

    case State::Menu:
    case State::Newspaper:
    case State::NightIntro:
        UpdateMenu(deltaTime);
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
    const int32_t hour = (mNightTime < kFirstHourSeconds) ? 0 : 1 + (int32_t)((mNightTime - kFirstHourSeconds) / kHourSeconds);
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
        UpdatePowerOut(deltaTime);
        return;
    }

    // Debug keys: X = power out, Y = Bonnie's jumpscare, D-pad down = Chica's,
    // D-pad up = Bonnie and Chica at the doors.
    if (Pressed(GAMEPAD_UP))
    {
        for (Animatronic* a : { &mBonnie, &mChica })
        {
            a->mRoom = a->mLeftSide ? Room::LeftDoor : Room::RightDoor;
            a->mSeenAtDoor = false;
            a->mMoveTimer = 0.0f;
            a->mOfficeTimer = 0.0f;
        }
        OctLog("FNAF1: debug: Bonnie and Chica at the doors");
    }
    if (Pressed(GAMEPAD_X))
    {
        StartPowerOut();
        return;
    }
    if (Pressed(GAMEPAD_Y))
    {
        StartJumpscare("bonnie");
        return;
    }
    if (Pressed(GAMEPAD_DOWN))
    {
        StartJumpscare("chica");
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

    // Chica in the kitchen: every 4 s, a 50% chance of one of the original's 5 kitchen sounds
    // (4 files, one used twice); loud when you're on CAM 6.
    if (mChica.mRoom == Room::Kitchen)
    {
        mPotsTimer -= deltaTime;
        if (mPotsTimer <= 0.0f)
        {
            mPotsTimer += 4.0f;
            if ((rand() % 2) == 0)
            {
                static const char* kPots[] = { "pots1", "pots2", "pots3", "pots4", "pots4" };
                // Channel volume 10 with the cameras down, 20 on another camera, 75 on CAM 6
                // (channel volume 25 is our 0.6, the fan).
                const bool watchingKitchen = mTabletUp && (Room)mCameraIndex == Room::Kitchen;
                const int32_t channelVolume = watchingKitchen ? 75 : (mTabletUp ? 20 : 10);
                PlaySound(kPots[rand() % 5], false, channelVolume * 0.024f);
            }
        }
    }
    else
    {
        mPotsTimer = 4.0f;
    }

    // Every 10 s, a 1/50 chance of a door pounding at channel volume 10 + Random(40), so it can
    // happen right at the start of the night.
    mPoundingTimer -= deltaTime;
    if (mPoundingTimer <= 0.0f)
    {
        mPoundingTimer += 10.0f;
        if ((rand() % 50) == 0)
        {
            PlaySound("knock", false, (10 + rand() % 40) * 0.024f);
        }
    }

    // Every 5 s while Bonnie or Chica is in the office ("got you") and the cameras are up, a 1/3
    // chance of one of the 4 groaning sounds.
    if (mTabletUp && (IsAt(mBonnie, Room::Office) || IsAt(mChica, Room::Office)))
    {
        mGroanTimer -= deltaTime;
        if (mGroanTimer <= 0.0f)
        {
            mGroanTimer += 5.0f;
            if ((rand() % 3) == 0 && !mBreath.IsPlaying())
            {
                const int32_t groan = (rand() % 4) + 1;
                char path[32];
                char key[32];
                snprintf(path, sizeof(path), "snd/breath%d.pcm", groan);
                snprintf(key, sizeof(key), "size_breath%d", groan);
                mBreath.Start(path, (uint32_t)mCounts[key], false, 0.8f);
            }
        }
    }
    else
    {
        mGroanTimer = 5.0f;
    }

    // Rare music, as in the original's events: "every 4 s, Random(30) = 1" plays Foxy's
    // pirate song, and "every 5 s, Random(30) = 1" plays the faint circus tune. The pirate
    // song event also checks a counter we haven't mapped; here it needs Foxy in the cove.
    mPirateSongTimer -= deltaTime;
    if (mPirateSongTimer <= 0.0f)
    {
        mPirateSongTimer += 4.0f;
        if (mFoxyStage == 0 && !mFoxyRunning && (rand() % 30) == 0 && !mRareMusic.IsPlaying())   // only while Foxy has no progress
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
            mRareMusic.Start("snd/circus.pcm", (uint32_t)mCounts["size_circus"], false, 0.12f);   // channel volume 5: faint, far away
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

    // Eerie ambience: its volume follows how much danger you're in. Each of these adds a step:
    // Bonnie on CAM 3, 2A or 2B, at the door or inside; Chica on CAM 4A or 4B, at the door or
    // inside; Foxy out from behind the curtain. None = silent, one = 30, two = 50, three = 75
    // (Freddy in the room would be 100; he doesn't roam here yet). The dark ambience keeps
    // playing underneath. Channel volumes map to stream volume so the dark ambience's 50 is
    // 1.5 (1.0 is the mixer's middle level, 2.0 its maximum).
    const bool bonnieDanger = mBonnie.mRoom == Room::SupplyCloset || mBonnie.mRoom == Room::WestHall ||
                              mBonnie.mRoom == Room::WestCorner || mBonnie.mRoom == Room::LeftDoor ||
                              mBonnie.mRoom == Room::Office;
    const bool chicaDanger = mChica.mRoom == Room::EastHall || mChica.mRoom == Room::EastCorner ||
                             mChica.mRoom == Room::RightDoor || mChica.mRoom == Room::Office;
    const bool foxyDanger = mFoxyStage >= 2 || mFoxyRunning;
    const int32_t danger = (bonnieDanger ? 1 : 0) + (chicaDanger ? 1 : 0) + (foxyDanger ? 1 : 0);
    static constexpr int32_t kEerieChannelVolume[] = { 0, 30, 50, 75 };
    const float eerieVolume = glm::min(2.0f, kEerieChannelVolume[danger] * 0.03f);

    if (danger != mAmbienceLayer)
    {
        OctLog("FNAF1: eerie ambience -> channel volume %d (danger %d)", kEerieChannelVolume[danger], danger);
        mAmbienceLayer = danger;
    }
    if (eerieVolume != mEerieVolume)
    {
        mEerieVolume = eerieVolume;
        mEerie.SetVolume(eerieVolume);
    }

    // Power: each thing in use adds a bar.
    mUsage = 1;
    for (const Door& door : mDoors)
    {
        // A door counts once it's fully shut, and until it's fully open again.
        mUsage += (door.mClosed ? door.mProgress >= 1.0f : door.mProgress > 0.0f) ? 1 : 0;
        mUsage += door.mLight ? 1 : 0;
    }
    mUsage += mTabletUp ? 1 : 0;

    mPower -= deltaTime * 0.1f * mUsage;
    if (mPower <= 0.0f)
    {
        StartPowerOut();
    }
}

void FnafGame::StartPowerOut()
{
    mPower = 0.0f;
    mState = State::PowerOut;
    mTabletUp = false;
    mTabletProgress = 0.0f;

    bool doorWasClosed = false;
    for (Door& door : mDoors)
    {
        doorWasClosed = doorWasClosed || door.mClosed;
        door.mClosed = false;
        door.mLight = false;
    }

    AudioManager::StopAllSounds();
    StopStreams();
    mJingle.Start("snd/powerdown.pcm", (uint32_t)mCounts["size_powerdown"], false, 1.0f);

    // The original switches its ambience channel to ambience2 (volume 50) when the power runs out.
    mAmbience.Start("snd/ambience.pcm", (uint32_t)mCounts["size_ambience"], true, 2.0f);   // a quiet recording
    if (doorWasClosed)
    {
        PlaySound("door");
    }

    mPowerOutPhase = 0;
    mPowerOutTimer = 0.0f;
    mPowerOutPhaseTimer = 0.0f;
    mPowerOutRollTimer = 0.0f;
    mFreddyFaceOn = false;
    mFreddyFlickerTimer = 0.0f;
    mPowerOutFlickerDark = false;
    OctLog("FNAF1: power out");
}

void FnafGame::UpdatePowerOut(float deltaTime)
{
    // Power-out, from the original's events: the office goes dark and the doors open; the music
    // box starts and Freddy's face flickers in the left doorway; the music stops and the lights
    // flicker out; then silent darkness until he attacks. Each step rolls a 1-in-5 chance at a
    // fixed interval, or happens for sure after 20 s.
    mPowerOutTimer += deltaTime;
    mPowerOutPhaseTimer += deltaTime;
    mPowerOutRollTimer += deltaTime;

    for (Door& door : mDoors)
    {
        door.mProgress = glm::max(0.0f, door.mProgress - deltaTime * kDoorSpeed);
    }

    // You can still look around (Freddy's face is in the left doorway).
    const float stick = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, 0);
    if (fabs(stick) > 0.2f)
    {
        mOfficePan = glm::clamp(mOfficePan + stick * kPanSpeed * deltaTime, 0.0f, 1.0f);
    }

    auto roll = [this](float interval, int32_t odds, float cap)
    {
        if (mPowerOutPhaseTimer >= cap)
        {
            return true;
        }
        if (mPowerOutRollTimer >= interval)
        {
            mPowerOutRollTimer -= interval;
            return (rand() % odds) == 0;
        }
        return false;
    };

    switch (mPowerOutPhase)
    {
    case 0:     // dark office: the music box starts on a 1-in-5 roll every 5 s, or after 20 s
        if (roll(5.0f, 5, 20.0f))
        {
            mPowerOutPhase = 1;
            mPowerOutPhaseTimer = 0.0f;
            mPowerOutRollTimer = 0.0f;
            mFreddyFlickerTimer = 0.0f;
            mMusicBox.Start("snd/musicbox.pcm", (uint32_t)mCounts["size_musicbox"], true, 0.8f);
        }
        break;

    case 1:     // music box: every 50 ms, a 1-in-4 chance his face shows in the left doorway
        mFreddyFlickerTimer -= deltaTime;
        while (mFreddyFlickerTimer <= 0.0f)
        {
            mFreddyFlickerTimer += 0.05f;
            mFreddyFaceOn = (rand() % 4) == 0;
        }
        // The music ends on a 1-in-5 roll every 5 s, or after 20 s.
        if (roll(5.0f, 5, 20.0f))
        {
            mPowerOutPhase = 2;
            mPowerOutPhaseTimer = 0.0f;
            mPowerOutRollTimer = 0.0f;
            mFreddyFaceOn = false;
            // Everything stops, and the fluorescent buzz comes back while the lights flicker out.
            mMusicBox.Stop();
            mAmbience.Stop();
            mJingle.Stop();
            mFanSound.Start("snd/fan.pcm", (uint32_t)mCounts["size_fan"], true, 1.2f);
        }
        break;

    case 2:     // lights flicker out: each frame a coin flip between the dark office with the buzz
                // and black silence, for 21 frames (at the original's 60 fps)
        mPowerOutFlickerDark = (rand() % 2) == 0;
        mFanSound.SetVolume(mPowerOutFlickerDark ? 0.0f : 1.2f);
        if (mPowerOutPhaseTimer >= 21.0f / 60.0f)
        {
            mPowerOutPhase = 3;
            mPowerOutPhaseTimer = 0.0f;
            mPowerOutRollTimer = 0.0f;
            mPowerOutFlickerDark = true;
            mFanSound.Stop();
        }
        break;

    default:    // silent darkness: the jumpscare comes on a 1-in-5 roll every 2 s, or after 20 s
        if (roll(2.0f, 5, 20.0f))
        {
            StartJumpscare("freddy");
        }
        break;
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

    // B mutes the phone call, like the original's "mute call" button, which shows 20 s into the
    // night and goes away at 40 s.
    if (Pressed(GAMEPAD_B) && mCall.IsPlaying() && mNightTime >= 20.0f && mNightTime <= 40.0f)
    {
        mCall.Stop();
    }

    // Z honks the Freddy poster's nose (clicking it in the original), in the office view.
    if (Pressed(GAMEPAD_Z) && !mTabletUp && mTabletProgress <= 0.0f)
    {
        PlaySound("honk");
    }

    // The tablet can't come up while Foxy is at the door (the original's progress 5).
    if (Pressed(GAMEPAD_A) && !(mFoxyAtDoor && !mTabletUp))
    {
        mTabletUp = !mTabletUp;
        mTabletUpTime = 0.0f;
        if (mTabletUp)
        {
            PlaySound("camup");
            // Cameras open: the original plays the MiniDV tape sound (stereo, full volume) on
            // its own channel and turns the fan's channel down (to 10, from 25).
            mTapeSound.Start("snd/minidv.pcm", (uint32_t)mCounts["size_minidv"], false, 1.0f);
            mFanSound.SetVolume(0.24f);
            mCall.SetVolume(0.5f);          // the call's channel goes from 100 to 50 with the cameras up
            SetLight(true, false);
            SetLight(false, false);
            mStaticTimer = kStaticSeconds;
            mCameraFresh = true;
        }
        else
        {
            mTabletUp = true;   // LowerTablet() expects it up
            LowerTablet();
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
                mCameraFresh = true;
                PlaySound("blip");
                OctLog("FNAF1: camera %s", kCameras[mCameraIndex].mId);
            }
        }
        return;
    }

    // Doors: L / R. Lights: D-pad left / right.
    for (int32_t side = 0; side < 2; ++side)
    {
        // Foxy at an open left door: the original hides that door's buttons.
        if (side == 0 && mFoxyAtDoor && !mDoors[0].mClosed)
        {
            continue;
        }

        const bool doorPressed = Pressed(side == 0 ? GAMEPAD_L1 : GAMEPAD_R1);
        const bool lightPressed = Pressed(side == 0 ? GAMEPAD_LEFT : GAMEPAD_RIGHT);

        // Bonnie inside kills the left side's buttons and Chica the right's: they just buzz.
        if (IsAt(side == 0 ? mBonnie : mChica, Room::Office))
        {
            if (doorPressed || lightPressed)
            {
                PlaySound("error");
            }
            continue;
        }

        // A door only responds once it has finished opening or closing.
        Door& door = mDoors[side];
        if (doorPressed && door.mProgress == (door.mClosed ? 1.0f : 0.0f))
        {
            door.mClosed = !door.mClosed;
            PlaySound("door");
        }

        if (lightPressed)
        {
            SetLight(side == 0, !door.mLight);
        }
    }
}

void FnafGame::LowerTablet()
{
    if (!mTabletUp)
    {
        return;
    }

    mTabletUp = false;
    mTabletUpTime = 0.0f;
    PlaySound("tablet");
    mTapeSound.Stop();              // the original mutes its channel when the cameras close
    mFanSound.SetVolume(0.6f);
    mCall.SetVolume(1.0f);
    mRandomForPic = (rand() % 100) + 1;   // the original re-rolls "random for pic" as the tablet goes down
}

void FnafGame::UpdateTablet(float deltaTime)
{
    const float target = mTabletUp ? 1.0f : 0.0f;
    if (mTabletProgress < target)
        mTabletProgress = glm::min(target, mTabletProgress + deltaTime * kTabletSpeed);
    else if (mTabletProgress > target)
        mTabletProgress = glm::max(target, mTabletProgress - deltaTime * kTabletSpeed);

    mCameraPanTime += deltaTime;    // the original's camera sweep runs all night
    if (mTabletUp)
    {
        mTabletUpTime += deltaTime;
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
            // Inside ("got you"), as in the original: her side's light goes out, and nothing
            // happens until you've had the cameras up with her in the room. Lowering them then
            // starts the jumpscare; keeping them up for 30 s pulls them down.
            a->mOfficeTimer += deltaTime;
            if (mDoors[side].mLight)
            {
                SetLight(a->mLeftSide, false);
            }
            if (mTabletUp)
            {
                a->mAttackArmed = true;
                a->mTabletUpInside += deltaTime;
                if (a->mTabletUpInside >= 30.0f)
                {
                    LowerTablet();
                }
            }
            else if (a->mAttackArmed && mTabletProgress <= 0.0f)
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

    // Feed cut when someone moves on the camera you're watching. In the original a successful move
    // roll sets a 10-frame flag before the move events run, and the camera check tests where she is:
    // her old spot on that frame (even if the move is then blocked), her new one on the next frames.
    // It hides the picture for 300 frames at 60 fps (only the faint static shows), keeps counting
    // on other cameras and with the tablet down, and brings the picture straight back.
    auto cutFeedIfWatched = [this](Room room)
    {
        const bool cameraOn = mTabletUp && mTabletProgress >= 1.0f;
        if (cameraOn && room == (Room)mCameraIndex)
        {
            mCameraCutTimer = 5.0f;
            // Random(4) + 1: 1 plays COMPUTER_DIGITAL, 2-4 play garble1-3.
            const int32_t roll = (rand() % 4) + 1;
            static const char* kMoveSounds[] = { "camhum", "garble1", "garble2", "garble3" };
            PlaySound(kMoveSounds[roll - 1], false, 0.7f);
            return true;
        }
        return false;
    };
    const bool cutOnLeaving = cutFeedIfWatched(before);
    a.mPose = (rand() % 2) + 1;     // the original re-rolls her camera pose on every move roll

    if (a.mLeftSide)
    {
        switch (a.mRoom)
        {
        case Room::ShowStage:    a.mRoom = coin ? Room::DiningArea : Room::Backstage; break;
        case Room::DiningArea:   a.mRoom = coin ? Room::Backstage : Room::WestHall; break;
        case Room::Backstage:    a.mRoom = coin ? Room::DiningArea : Room::WestHall; break;
        case Room::WestHall:     a.mRoom = coin ? Room::SupplyCloset : Room::WestCorner; break;
        case Room::SupplyCloset: a.mRoom = coin ? Room::LeftDoor : Room::WestHall; break;
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
        case Room::EastHall:   a.mRoom = coin ? Room::DiningArea : Room::EastCorner; break;
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

        // Arriving on the camera you're watching (leaving it was checked above).
        if (!cutOnLeaving)
        {
            cutFeedIfWatched(a.mRoom);
        }
        // Footsteps ("deep steps"): the original plays them on every move except getting in, at a
        // channel volume set by the room she left (10 far away .. 40 close), and mutes that
        // channel while you watch the camera she's on. Channel volume 25 is our 0.6 (the fan).
        int32_t stepsVolume = 0;
        switch (before)
        {
        case Room::ShowStage:
        case Room::Backstage:    stepsVolume = 10; break;
        case Room::DiningArea:   stepsVolume = a.mLeftSide ? 20 : 10; break;
        case Room::Kitchen:      stepsVolume = (a.mRoom == Room::Restrooms) ? 10 : 20; break;
        case Room::Restrooms:    stepsVolume = 20; break;
        case Room::WestHall:
        case Room::SupplyCloset:
        case Room::EastHall:
        case Room::LeftDoor:     stepsVolume = 30; break;
        case Room::WestCorner:
        case Room::EastCorner:
        case Room::RightDoor:    stepsVolume = 40; break;
        default:                 break;
        }
        const bool watchingHer = mTabletUp && mTabletProgress >= 1.0f && a.mRoom == (Room)mCameraIndex;
        if (a.mRoom != Room::Office && stepsVolume > 0 && !watchingHer)
        {
            PlaySound("steps", false, stepsVolume * 0.024f);
        }
        a.mAttackArmed = false;
        a.mTabletUpInside = 0.0f;
        LogDebug("FNAF1: %s moved to room %d", a.mName, (int)a.mRoom);
    }
}

void FnafGame::UpdateFoxy(float deltaTime)
{
    if (mState != State::Playing)
    {
        return;
    }

    if (mFoxyAtDoor)
    {
        // At the left door (the original's progress 5): the cameras are forced down and the lights
        // go off. Nothing happens until the tablet is fully down and the door isn't moving.
        LowerTablet();
        if (mDoors[0].mLight || mDoors[1].mLight)
        {
            SetLight(true, false);
            SetLight(false, false);
        }

        const Door& door = mDoors[0];
        if (mTabletProgress > 0.0f || door.mProgress != (door.mClosed ? 1.0f : 0.0f))
        {
            return;
        }
        mFoxyAtDoor = false;

        if (door.mClosed)
        {
            // Bangs on the door (knock2, much louder than the random knock), drains 10 + 50 x bangs
            // of the original's 999 power (1%, 6%, 11%, ...) and goes back to curtain stage 0 or 1.
            PlaySound("foxybang", false, 1.0f);
            mPower = glm::max(0.0f, mPower - (1.0f + 5.0f * mFoxyKnocks));
            mFoxyKnocks++;
            mFoxyStage = rand() % 2;
            OctLog("FNAF1: foxy banged (%d), back to stage %d, power %.0f", mFoxyKnocks, mFoxyStage, mPower);
            return;
        }

        StartJumpscare("foxy");
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
        mFoxyRunTimer += deltaTime;
        if (mFoxyRunTimer >= kFoxyRunSeconds)
        {
            LogAnimStats("foxy run");
            FoxyArrive();
        }
        return;
    }

    if (mFoxyStage < 3)
    {
        // Watching the cameras keeps him in the cove, and he stays put for a while after.
        if (mTabletUp)
        {
            mFoxyLockTimer = (50 + rand() % 1000) / 60.0f;   // the original: 50 + Random(1000) frames
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
                OctLog("FNAF1: foxy stage %d (hour %d)", mFoxyStage, mHour);
                if (mFoxyStage == 3)
                {
                    mFoxyRunTimer = 0.0f;
                }
            }
        }
        return;
    }

    // Out of the cove: checking the West Hall sets him running, otherwise he comes anyway.
    const bool watchingWestHall = mTabletUp && mTabletProgress >= 1.0f && (Room)mCameraIndex == Room::WestHall;
    if (watchingWestHall)
    {
        OctLog("FNAF1: foxy runs (seen on CAM 2A)");
        mFoxyRunning = true;
        mFoxyRunTimer = 0.0f;
        mFoxyRunFrame = 0;
        mFoxyRunFrameTimer = 0.0f;
        PlaySound("run");
        return;
    }

    // Not seen: 1500 frames (25 s) after leaving, whatever the cameras show, he's at the door
    // without the run.
    mFoxyRunTimer += deltaTime;
    if (mFoxyRunTimer >= kFoxyArriveSeconds)
    {
        FoxyArrive();
    }
}

void FnafGame::FoxyArrive()
{
    OctLog("FNAF1: foxy at the left door (%s, door %s, tablet %s, camera %s, power %.0f)",
           mFoxyRunning ? "after the run" : "25 s timer", mDoors[0].mClosed ? "closed" : "open",
           mTabletUp ? "up" : "down", kCameras[mCameraIndex].mId, mPower);
    mFoxyRunning = false;
    mFoxyAtDoor = true;     // resolved in UpdateFoxy once the tablet is down
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
    mOfficePan = 0.5f;      // face the middle of the office, where the lunge happens
    mJumpWho = who;
    OctLog("FNAF1: jumpscare %s", who.c_str());
    mJumpFrame = 0;
    mJumpTimer = 0.0f;

    // Load the first frame before showing the jumpscare: the canvas is shared with the menu,
    // newspaper and game over screen, and its old picture would flash for a frame.
    char name[64];
    snprintf(name, sizeof(name), "jump_%s_00", who.c_str());
    std::string shown;
    ShowImage(mJumpCanvas, name, shown);
    mMenuShown.clear();
    mJump->SetVisible(true);
}

void FnafGame::UpdateJumpscare(float deltaTime)
{
    const int32_t frames = mCounts["jump_" + mJumpWho];
    mJumpTimer += deltaTime;

    // The original's animation speeds at 60 fps: Bonnie 75 (45 fps), Chica 99 (59 fps), Foxy 50
    // (30 fps), Freddy's power-out lunge 60 (36 fps).
    float fps = 24.0f;
    if (mJumpWho == "bonnie")       fps = 45.0f;
    else if (mJumpWho == "chica")   fps = 59.4f;
    else if (mJumpWho == "foxy")    fps = 30.0f;
    else if (mJumpWho == "freddy")  fps = 36.0f;
    const float frameSeconds = 1.0f / fps;
    if (mJumpTimer >= frameSeconds && mJumpFrame + 1 < frames)
    {
        // Advance by the time that passed, skipping frames when loading one took longer than a
        // frame (reading and decoding a frame on the GameCube can), so the scare keeps its length.
        const int32_t steps = (int32_t)(mJumpTimer / frameSeconds);
        mJumpTimer -= steps * frameSeconds;
        mJumpFrame = glm::min(mJumpFrame + steps, frames - 1);
        char name[64];
        snprintf(name, sizeof(name), "jump_%s_%02d", mJumpWho.c_str(), mJumpFrame);
        std::string shown;
        ShowImage(mJumpCanvas, name, shown);
    }
    else if (mJumpFrame + 1 >= frames && mJumpTimer > 0.6f)
    {
        // Game over: full-screen static with its sound, then the game over screen.
        LogAnimStats(("jumpscare " + mJumpWho).c_str());
        mJump->SetVisible(false);
        AudioManager::StopAllSounds();      // the scream ends with the animation
        mState = State::GameOver;
        mGameOverTimer = 0.0f;
        mMenuShown.clear();
        ShowMenuWidgets(false, false, false);
        mMenuBlack->SetVisible(true);
        mMenuStaticQuad->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        mMenuStaticQuad->SetVisible(true);
        mJingle.Start("snd/deadstatic.pcm", (uint32_t)mCounts["size_deadstatic"], false, 1.0f);
    }
}

std::string FnafGame::GetOfficeImage() const
{
    if (mState == State::PowerOut)
        return (mPowerOutPhase == 1 && mFreddyFaceOn) ? "office_freddy_dark" : "office_dark";

    // A hall light drops out for a moment now and then (the original: 1 frame in 10).
    if ((mDoors[0].mLight || mDoors[1].mLight) && mLightDropout)
        return "office";

    if (mDoors[0].mLight)
        return IsAt(mBonnie, Room::LeftDoor) ? "office_bonnie" : "office_light_l";

    if (mDoors[1].mLight)
        return IsAt(mChica, Room::RightDoor) ? "office_chica" : "office_light_r";

    return "office";
}

float FnafGame::GetPirateSongVolume() const
{
    const bool watchingCove = mTabletUp && mTabletProgress >= 1.0f && (Room)mCameraIndex == Room::PirateCove;
    return watchingCove ? 0.36f : 0.12f;    // channel volume 15 while watching CAM 1C, 5 otherwise
}

std::string FnafGame::GetCameraImage(Room camera) const
{
    // Pictures as the original's events pick them. "random for pic" (1..100) is rolled each time
    // the tablet goes down; Bonnie's and Chica's poses (1 or 2) on every move roll.
    const bool bonnie = IsAt(mBonnie, camera);
    const bool chica = IsAt(mChica, camera);
    const int32_t pic = mRandomForPic;

    switch (camera)
    {
    case Room::ShowStage:
        if (bonnie && chica) return "cam1a_all";
        if (chica) return "cam1a_no_bonnie";
        if (bonnie) return "cam1a_no_chica";
        return (pic <= 10) ? "cam1a_freddy_stare" : "cam1a_freddy";
    case Room::DiningArea:
        // Chica wins when both are there.
        if (chica) return (mChica.mPose == 1) ? "cam1b_chica2" : "cam1b_chica";
        if (bonnie) return (mBonnie.mPose == 1) ? "cam1b_bonnie" : "cam1b_bonnie2";
        return "cam1b_empty";
    case Room::PirateCove:
    {
        // Gone from the cove (also while he runs or is at the door): the empty stage, or the
        // "IT'S ME" sign when the roll is 10 or less.
        if (mFoxyStage >= 3)
        {
            return (pic <= 10) ? "cam1c_rare_itsme" : "cam1c_3";
        }

        char name[16];
        snprintf(name, sizeof(name), "cam1c_%d", glm::clamp(mFoxyStage, 0, 2));
        return name;
    }
    case Room::Backstage:
        if (bonnie) return (pic <= 10) ? "cam5_bonnie_stare" : "cam5_bonnie";
        return (pic <= 5) ? "cam5_rare" : "cam5_empty";
    case Room::Restrooms:
        if (chica) return (mChica.mPose == 1) ? "cam7_chica" : "cam7_chica2";
        return "cam7_empty";
    case Room::Kitchen:      return "";
    case Room::WestHall:
        if (mFoxyRunning)
        {
            char name[24];
            // After the last frame the original's run animation loops its last two frames
            // (the empty hall at the end) until he reaches the door.
            const int32_t runFrames = glm::max(1, mCounts.at("foxyrun"));
            int32_t frame = mFoxyRunFrame;
            if (frame >= runFrames)
            {
                frame = (runFrames >= 2) ? runFrames - 2 + ((frame - runFrames) % 2) : runFrames - 1;
            }
            snprintf(name, sizeof(name), "foxyrun_%02d", frame);
            return name;
        }
        // Dark, except on the flicker steps when the light catches the hall (and Bonnie).
        if (!mHallLit) return "cam2a_dark";
        return bonnie ? "cam2a_bonnie" : "cam2a_empty";
    case Room::SupplyCloset: return bonnie ? "cam3_bonnie" : "cam3_empty";
    // Rare pictures on empty cameras, by the roll.
    case Room::WestCorner:
        if (bonnie) return "cam2b_bonnie";
        return (pic < 2) ? "cam2b_rare_freddy" : "cam2b_empty";
    case Room::EastHall:
        if (chica) return (mChica.mPose == 1) ? "cam4a_chica" : "cam4a_chica2";
        if (pic == 99) return "cam4a_rare_faces";
        if (pic == 100) return "cam4a_rare_itsme";
        return "cam4a_empty";
    case Room::EastCorner:
        if (chica) return "cam4b_chica";
        if (pic >= 97)
        {
            // One of four newspaper clippings: 97, 98, 99 or 100.
            static const char* kNews[] = { "cam4b_rare_news0", "cam4b_rare_news1", "cam4b_rare_news2", "cam4b_rare_news3" };
            return kNews[glm::clamp(pic - 97, 0, 3)];
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
    const uint64_t startUs = SYS_GetTimeMicroseconds();
    const bool read = ReadDataFile("img/" + name + ".jpg", mFrameBuffer);
    const uint64_t readUs = SYS_GetTimeMicroseconds();
    if (read && canvas.Show(mFrameBuffer))
    {
        shown = name;
    }
    const uint64_t endUs = SYS_GetTimeMicroseconds();

    if (name.compare(0, 5, "jump_") == 0 || name.compare(0, 8, "foxyrun_") == 0)
    {
        sAnimStats.frames++;
        sAnimStats.readUs += readUs - startUs;
        sAnimStats.decodeUs += endUs - readUs;
        sAnimStats.worstUs = glm::max(sAnimStats.worstUs, endUs - startUs);
    }
}

void FnafGame::UpdateView(float deltaTime)
{
    const float scale = mScreenHeight / kOfficeHeight;
    const float officeWidth = kOfficeWidth * scale;
    const float panX = mOfficePan * glm::max(0.0f, officeWidth - mScreenWidth);

    // Flicker rolls. The original rolls these every frame at 60 fps: the West Hall (CAM 2A) is lit
    // on 3 frames in 10 (only then can Bonnie be seen there), and a hall light drops out 1 frame in
    // 10. Every change means decoding another picture, which costs a GameCube frame or two, so we
    // roll every 100 ms instead.
    mFlickerTimer -= deltaTime;
    if (mFlickerTimer <= 0.0f)
    {
        mFlickerTimer += 0.1f;
        mHallLit = (rand() % 10) < 3;
        mLightDropout = (rand() % 10) == 0;
    }

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

    if (mCameraCutTimer > 0.0f)
    {
        mCameraCutTimer -= deltaTime;
    }
    // While the feed is cut after someone moved, only the static shows.
    mCamera->SetVisible(cameraOn && !cameraImage.empty() && mCameraCutTimer <= 0.0f);
    // Black for the audio-only Kitchen camera, and for the pitch-black end of a power-out.
    mCameraBlack->SetVisible((cameraOn && (cameraImage.empty() || mCameraCutTimer > 0.0f)) ||
                             (mState == State::PowerOut && (mPowerOutPhase == 3 || (mPowerOutPhase == 2 && mPowerOutFlickerDark))));
    if (cameraOn && !cameraImage.empty())
    {
        if (cameraImage != mCameraShown)
        {
            // (Movement on this camera is handled in MoveAnimatronic: the feed cuts to static.)
            ShowImage(mCameraCanvas, cameraImage, mCameraShown);
        }
        mCameraFresh = false;

        // The original's camera sweep: 320 frames (at 60 fps) across, a 100-frame hold, 320 back,
        // a 100-frame hold, running all night. CAM 3 doesn't pan (it stays at the left edge).
        const float kSweep = 320.0f / 60.0f;
        const float kHold = 100.0f / 60.0f;
        const float cycle = 2.0f * (kSweep + kHold);
        const float t = fmod(mCameraPanTime, cycle);
        float amount = 0.0f;
        if (t < kSweep)                         amount = t / kSweep;
        else if (t < kSweep + kHold)            amount = 1.0f;
        else if (t < 2.0f * kSweep + kHold)     amount = 1.0f - (t - kSweep - kHold) / kSweep;
        else                                    amount = 0.0f;
        if (room == Room::SupplyCloset)
        {
            amount = 0.0f;
        }
        const float pan = amount * glm::max(0.0f, officeWidth - mScreenWidth);
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

    // Static: a flickering see-through layer over every camera, solid for a moment when the
    // view switches or changes.
    const bool staticOn = cameraOn;
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

        // The original's static: every frame its blend coefficient (0 opaque .. 255 invisible)
        // is 150 + Random(50) + level * 15, with level re-rolled to 0-2 every second. That's
        // roughly 10-41% opaque. It's solid for the switch burst; while the feed is cut the
        // picture is hidden and this faint static shows over black.
        mStaticLevelTimer -= deltaTime;
        if (mStaticLevelTimer <= 0.0f)
        {
            mStaticLevelTimer += 1.0f;
            mStaticLevel = rand() % 3;
        }
        const float coefficient = 150.0f + (float)(rand() % 50) + mStaticLevel * 15.0f;
        const float alpha = (mStaticTimer > 0.0f) ? 1.0f : 1.0f - coefficient / 255.0f;
        mStatic->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, alpha));
    }

    // Jumpscare: centered on the current view.
    if (mJump->IsVisible())
    {
        if (mJumpWho == "freddy")
        {
            // The power-out jumpscare frames are screen-sized (1280x720), not office-wide.
            mJump->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
        }
        else
        {
            // The frames are office-wide with the lunge in the middle: always draw them centered,
            // whatever the pan, so the jumpscare never happens off-screen.
            const float centerPanX = 0.5f * glm::max(0.0f, officeWidth - mScreenWidth);
            mJump->SetRect(-centerPanX, 0.0f, officeWidth, mScreenHeight);
        }
    }
}

// ---- Main menu --------------------------------------------------------------
// Positions are in the original 1280x720 screen's pixels.

void FnafGame::BuildMenuUi()
{
    // Above the night's widgets, below the loading screen (child order is draw order).
    mMenuBlack = mRoot->CreateChild<Quad>("MenuBlack");
    mMenuBlack->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
    mMenuBlack->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

    mMenuBack = mRoot->CreateChild<Quad>("MenuBack");
    mMenuBack->SetTexture(mJumpCanvas.GetTexture());
    mMenuBack->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);

    mMenuStaticQuad = mRoot->CreateChild<Quad>("MenuStatic");
    mMenuStaticQuad->SetTexture(mStaticCanvas.GetTexture());
    mMenuStaticQuad->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);

    mMenuTitle = mRoot->CreateChild<Quad>("MenuTitle");
    mMenuNewGame = mRoot->CreateChild<Quad>("MenuNewGame");
    mMenuContinue = mRoot->CreateChild<Quad>("MenuContinue");
    mMenuArrows = mRoot->CreateChild<Quad>("MenuArrows");
    mMenuCopyright = mRoot->CreateChild<Quad>("MenuCopyright");
    mIntroClock = mRoot->CreateChild<Quad>("IntroClock");
    mIntroFirst = mRoot->CreateChild<Quad>("IntroFirst");
    mIntroNight = mRoot->CreateChild<Quad>("IntroNight");
    mGameOverText = mRoot->CreateChild<Quad>("GameOverText");

    ShowMenuWidgets(false, false, false);
}

void FnafGame::ShowMenuWidgets(bool menu, bool newspaper, bool intro)
{
    if (mMenuBlack == nullptr)
    {
        return;
    }

    mMenuBlack->SetVisible(menu || newspaper || intro);
    mMenuBack->SetVisible(menu || newspaper);
    mMenuStaticQuad->SetVisible(menu || intro);

    for (Quad* quad : { mMenuTitle, mMenuNewGame, mMenuContinue, mMenuArrows, mMenuCopyright })
    {
        quad->SetVisible(menu);
    }
    for (Quad* quad : { mIntroClock, mIntroFirst, mIntroNight })
    {
        quad->SetVisible(intro);
    }
    mGameOverText->SetVisible(false);   // shown by UpdateGameOver
}

void FnafGame::PlaceSprite(Quad* quad, const Sprite& sprite, float x, float y)
{
    // Menu sprites are stored at their size on a 640x480 screen.
    quad->SetTexture(sprite.Get());
    quad->SetRect(x * mScreenWidth / 1280.0f, y * mScreenHeight / 720.0f,
                  sprite.mWidth * mScreenWidth / 640.0f, sprite.mHeight * mScreenHeight / 480.0f);
}

void FnafGame::EnterMenu()
{
    AudioManager::StopAllSounds();
    StopStreams();

    mState = State::Menu;
    mMenuSelection = 0;
    mMenuTimer = 0.0f;
    mMenuFrameTimer = 0.0f;
    mMenuShown.clear();     // the jumpscare canvas may hold something else now
    mJump->SetVisible(false);
    ShowMessage("");

    PlaceSprite(mMenuTitle, mMenuTitleSprite, 175.0f, 80.0f);
    PlaceSprite(mMenuNewGame, mMenuNewGameSprite, 175.0f, 400.0f);
    PlaceSprite(mMenuContinue, mMenuContinueSprite, 175.0f, 470.0f);
    PlaceSprite(mMenuCopyright, mMenuCopyrightSprite, 1260.0f - mMenuCopyrightSprite.mWidth * 2.0f, 690.0f);
    ShowMenuWidgets(true, false, false);

    mMenuMusic.Start("snd/menumusic.pcm", (uint32_t)mCounts["size_menumusic"], true, 0.8f);
    mMenuHum.Start("snd/menustatic.pcm", (uint32_t)mCounts["size_menustatic"], true, 0.4f);
    OctLog("FNAF1: main menu");
}

void FnafGame::StartNightIntro()
{
    StopStreams();
    mState = State::NightIntro;
    mMenuTimer = 0.0f;

    // "12:00 AM" centered, "1st Night" centered below it with the words' bottoms lined up.
    const float firstWidth = mIntroFirstSprite.mWidth * 2.0f;
    const float nightWidth = mIntroNightSprite.mWidth * 2.0f;
    const float gap = 20.0f;
    const float x = 640.0f - (firstWidth + gap + nightWidth) * 0.5f;
    const float bottom = 400.0f;

    PlaceSprite(mIntroClock, mIntroClockSprite, 640.0f - mIntroClockSprite.mWidth, 290.0f);
    PlaceSprite(mIntroFirst, mIntroFirstSprite, x, bottom - mIntroFirstSprite.mHeight * 1.5f);
    PlaceSprite(mIntroNight, mIntroNightSprite, x + firstWidth + gap, bottom - mIntroNightSprite.mHeight * 1.5f);
    ShowMenuWidgets(false, false, true);
    PlaySound("blip");
}

void FnafGame::UpdateMenu(float deltaTime)
{
    mMenuTimer += deltaTime;

    // Static over the menu and the night intro, animated like the cameras'.
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

    switch (mState)
    {
    case State::Menu:
    {
        // Freddy's face flickers, and now and then twitches or glitches.
        mMenuFrameTimer -= deltaTime;
        if (mMenuFrameTimer <= 0.0f)
        {
            mMenuFrameTimer = kMenuFrameSeconds;
            const int32_t roll = rand() % 100;
            const int32_t frame = (roll < 94) ? 0 : (roll < 97) ? 1 : (roll < 99) ? 2 : 3;
            char name[32];
            snprintf(name, sizeof(name), "menu_freddy%d", frame);
            ShowImage(mJumpCanvas, name, mMenuShown);
            mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 0.55f + (rand() % 46) / 100.0f));
            mMenuStaticQuad->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 0.15f + (rand() % 20) / 100.0f));
        }

        if (Pressed(GAMEPAD_UP) || Pressed(GAMEPAD_DOWN))
        {
            mMenuSelection = 1 - mMenuSelection;
            PlaySound("blip");
        }
        PlaceSprite(mMenuArrows, mMenuArrowsSprite, 95.0f, mMenuSelection == 0 ? 402.0f : 474.0f);

        if (Pressed(GAMEPAD_A) || Pressed(GAMEPAD_START))
        {
            if (mMenuSelection == 0)
            {
                // New Game: the help-wanted ad first.
                StopStreams();
                mState = State::Newspaper;
                mMenuTimer = 0.0f;
                ShowImage(mJumpCanvas, "newspaper", mMenuShown);
                mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
                ShowMenuWidgets(false, true, false);
            }
            else
            {
                StartNightIntro();
            }
        }
        break;
    }

    case State::Newspaper:
        // Fades in and stays a few seconds; A or START skips it.
        mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, glm::clamp(mMenuTimer, 0.0f, 1.0f)));
        if (mMenuTimer >= kNewspaperSeconds || (mMenuTimer > 0.5f && (Pressed(GAMEPAD_A) || Pressed(GAMEPAD_START))))
        {
            StartNightIntro();
        }
        break;

    case State::NightIntro:
        // A burst of static that fades, then the night starts.
        mMenuStaticQuad->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, glm::clamp(1.0f - mMenuTimer * 2.0f, 0.0f, 1.0f)));
        if (mMenuTimer >= kNightIntroSeconds)
        {
            StartNight();
        }
        break;

    default:
        break;
    }
}

void FnafGame::UpdateGameOver(float deltaTime)
{
    mGameOverTimer += deltaTime;

    // Static animates the whole time.
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

    if (mGameOverTimer < kGameOverStaticSeconds)
    {
        return;
    }

    if (!mGameOverText->IsVisible())
    {
        // The game over screen: Freddy in the backstage room, "Game Over" in the corner.
        mJingle.Stop();
        AudioManager::StopAllSounds();
        ShowImage(mJumpCanvas, "gameover", mMenuShown);
        mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        mMenuBack->SetVisible(true);
        mMenuStaticQuad->SetVisible(false);     // a clean cut: no static, no sound
        PlaceSprite(mGameOverText, mGameOverSprite, 1280.0f - mGameOverSprite.mWidth * 2.0f - 60.0f, 640.0f);
        mGameOverText->SetVisible(true);
    }

    const float shown = mGameOverTimer - kGameOverStaticSeconds;
    if (shown >= kGameOverSeconds || (shown > 1.0f && (Pressed(GAMEPAD_A) || Pressed(GAMEPAD_START))))
    {
        EnterMenu();
    }
}

void FnafGame::UpdateHud()
{
    const bool playing = (mState == State::Playing || mState == State::PowerOut);
    mTimeText->SetVisible(playing);
    mNightText->SetVisible(playing);
    mPowerText->SetVisible(playing);
    mUsageText->SetVisible(playing && mState != State::PowerOut);
    mDebugText->SetVisible(mState == State::Playing);

    if (!playing)
    {
        return;
    }

    {
        // Debug line: which ambience layer is playing, and where Bonnie and Chica are.
        static const char* kRooms[] = { "Stage", "Dining", "Cove", "Backstage", "Restrooms", "Kitchen",
                                        "W.Hall", "Closet", "W.Corner", "E.Hall", "E.Corner", "L.Door", "R.Door", "Office" };
        static const char* kLayers[] = { "dark + eerie 0", "dark + eerie 30", "dark + eerie 50", "dark + eerie 75" };
        char debug[128];
        snprintf(debug, sizeof(debug), "Ambience %s | Bonnie %s | Chica %s | Foxy %d | skips %u",
                 mAmbienceLayer >= 0 ? kLayers[mAmbienceLayer] : "-",
                 kRooms[(int)mBonnie.mRoom], kRooms[(int)mChica.mRoom], mFoxyStage,
                 (unsigned)GetStreamUnderruns());
        mDebugText->SetText(debug);
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
    mMenuMusic.Stop();
    mMenuHum.Stop();
    mEerie.Stop();
    mBreath.Stop();
    mTapeSound.Stop();
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
