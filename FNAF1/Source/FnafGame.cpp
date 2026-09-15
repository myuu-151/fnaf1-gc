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
static constexpr int32_t kFoxyBaseAi = 5;      // rolls 1..20 every 5 s; raised from 2 for a livelier night 1

// Rare camera pictures (the original rolls a "random for pic" counter): Freddy staring on
// the Show Stage, and rare posters on empty cameras. Rolled when the tablet goes up or the
// camera changes. Placeholder odds: the real value is in the game's compiled events, not
// decoded yet.
static constexpr int32_t kRarePicOdds = 20;

// Main menu timing
static constexpr float kMenuFrameSeconds = 0.08f;   // Freddy's face frame and flicker
static constexpr float kNewspaperSeconds = 5.0f;    // help-wanted ad after New Game
static constexpr float kNightIntroSeconds = 2.5f;   // "12:00 AM / 1st Night"
static constexpr float kGameOverStaticSeconds = 10.8f;  // static before the game over screen (the static sound's length)
static constexpr float kGameOverSeconds = 10.0f;        // game over screen, then the menu

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
    for (const char* office : { "office", "office_light_l", "office_light_r", "office_bonnie", "office_chica", "office_dark", "office_freddy_dark" })
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
                const bool watchingKitchen = mTabletUp && (Room)mCameraIndex == Room::Kitchen;
                PlaySound(kPots[rand() % 5], false, watchingKitchen ? 0.9f : 0.25f);
            }
        }
    }
    else
    {
        mPotsTimer = 4.0f;
    }

    // Every 10 s, a 1/50 chance of a faint door pounding (the original plays it on a channel at
    // volume 10), so it can happen right at the start of the night.
    mPoundingTimer -= deltaTime;
    if (mPoundingTimer <= 0.0f)
    {
        mPoundingTimer += 10.0f;
        if ((rand() % 50) == 0)
        {
            PlaySound("knock", false, 0.15f);
        }
    }

    // Every 5 s while Bonnie or Chica is in the office ("got you"), a 1/3 chance of one of
    // the 4 groaning sounds.
    if (IsAt(mBonnie, Room::Office) || IsAt(mChica, Room::Office))
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
            mRareMusic.Start("snd/circus.pcm", (uint32_t)mCounts["size_circus"], false, 0.15f);   // faint, far away
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
        mUsage += door.mClosed ? 1 : 0;
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
    OctLog("FNAF1: power out");
}

void FnafGame::UpdatePowerOut(float deltaTime)
{
    // Power-out, as in the original: the office goes dark and the doors open; after a while
    // Freddy's face flickers in the left doorway to the music box; the music stops, it goes
    // pitch black with footsteps, and he attacks. Each step rolls a chance every few seconds,
    // with a cap (timings are estimates, not decoded from the original's events).
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
    case 0:     // dark office, waiting for Freddy
        // The power-down sound winds down, then the music box starts right away.
        if (!mJingle.IsPlaying())
        {
            mPowerOutPhase = 1;
            mPowerOutPhaseTimer = 0.0f;
            mPowerOutRollTimer = 0.0f;
            mMusicBox.Start("snd/musicbox.pcm", (uint32_t)mCounts["size_musicbox"], true, 0.8f);
        }
        break;

    case 1:     // music box: his face flickers in the left doorway
        mFreddyFlickerTimer -= deltaTime;
        if (mFreddyFlickerTimer <= 0.0f)
        {
            mFreddyFaceOn = (rand() % 100) < 60;
            mFreddyFlickerTimer = 0.05f + (rand() % 20) / 100.0f;
        }
        if (roll(5.0f, 5, 20.0f))
        {
            mPowerOutPhase = 2;
            mPowerOutPhaseTimer = 0.0f;
            mPowerOutRollTimer = 0.0f;
            mFreddyFaceOn = false;
            mMusicBox.Stop();
            PlaySound("steps", false, 0.8f);
        }
        break;

    default:    // pitch black, footsteps, then the jumpscare
        if (roll(2.0f, 5, 10.0f))
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
            // Cameras open: the original plays the MiniDV tape sound (stereo, full volume) on
            // its own channel and turns the fan's channel down (to 10, from 25).
            mTapeSound.Start("snd/minidv.pcm", (uint32_t)mCounts["size_minidv"], false, 1.0f);
            mFanSound.SetVolume(0.24f);
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
            mTapeSound.Stop();              // the original mutes its channel when the cameras close
            mFanSound.SetVolume(0.6f);
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
                OctLog("FNAF1: camera %s", kCameras[mCameraIndex].mId);
            }
        }
        return;
    }

    // Doors: L / R. Lights: D-pad left / right. The controls stop working once
    // someone is inside the office.
    if (intruder)
    {
        // The buttons are dead: they just buzz.
        if (Pressed(GAMEPAD_L1) || Pressed(GAMEPAD_R1) || Pressed(GAMEPAD_LEFT) || Pressed(GAMEPAD_RIGHT))
        {
            PlaySound("error");
        }
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
                mTapeSound.Stop();
                mFanSound.SetVolume(0.6f);
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

        // Moving on the camera you're watching cuts that feed to static for 5 s (the original
        // counts 300 frames at 60 fps with the picture hidden) and rolls a garble:
        // Random(4) + 1, where 2-4 play one of the three and 1 plays nothing.
        const bool cameraOn = mTabletUp && mTabletProgress >= 1.0f;
        const Room watched = (Room)mCameraIndex;
        if (cameraOn && (before == watched || a.mRoom == watched))
        {
            mCameraCutTimer = 5.0f;
            // Random(4) + 1: 1 plays COMPUTER_DIGITAL, 2-4 play garble1-3.
            const int32_t roll = (rand() % 4) + 1;
            static const char* kMoveSounds[] = { "camhum", "garble1", "garble2", "garble3" };
            PlaySound(kMoveSounds[roll - 1], false, 0.7f);
        }
        // Footsteps as they close in, louder the nearer they get (the original plays its
        // "deep steps" at 10-40% depending on where they are).
        float steps = 0.0f;
        switch (a.mRoom)
        {
        case Room::WestHall:
        case Room::EastHall:     steps = 0.25f; break;
        case Room::WestCorner:
        case Room::EastCorner:   steps = 0.35f; break;
        case Room::LeftDoor:
        case Room::RightDoor:
        case Room::Office:       steps = 0.5f; break;
        default:                 break;
        }
        if (steps > 0.0f)
        {
            PlaySound("steps", false, steps);
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
        // Bangs on the door (the original's knock2, much louder than the random knock), drains
        // power (more each time), and goes back to the cove.
        PlaySound("foxybang", false, 1.0f);
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
        // Game over: full-screen static with its sound, then the game over screen.
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

    if (mCameraCutTimer > 0.0f)
    {
        mCameraCutTimer -= deltaTime;
    }
    // While the feed is cut after someone moved, only the static shows.
    mCamera->SetVisible(cameraOn && !cameraImage.empty() && mCameraCutTimer <= 0.0f);
    // Black for the audio-only Kitchen camera, and for the pitch-black end of a power-out.
    mCameraBlack->SetVisible((cameraOn && cameraImage.empty()) || (mState == State::PowerOut && mPowerOutPhase == 2));
    if (cameraOn && !cameraImage.empty())
    {
        if (cameraImage != mCameraShown)
        {
            // (Movement on this camera is handled in MoveAnimatronic: the feed cuts to static.)
            ShowImage(mCameraCanvas, cameraImage, mCameraShown);
        }
        mCameraFresh = false;

        // The camera sweeps left to right at a steady speed, holds, sweeps back, and holds.
        const float kSweep = 4.0f;
        const float kHold = 1.5f;
        const float cycle = 2.0f * (kSweep + kHold);
        const float t = fmod(mCameraPanTime, cycle);
        float amount = 0.0f;
        if (t < kHold)                          amount = 0.0f;
        else if (t < kHold + kSweep)            amount = (t - kHold) / kSweep;
        else if (t < 2.0f * kHold + kSweep)     amount = 1.0f;
        else                                    amount = 1.0f - (t - 2.0f * kHold - kSweep) / kSweep;
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
        // roughly 10-41% opaque. It's solid for the switch burst and while the feed is cut.
        mStaticLevelTimer -= deltaTime;
        if (mStaticLevelTimer <= 0.0f)
        {
            mStaticLevelTimer += 1.0f;
            mStaticLevel = rand() % 3;
        }
        const float coefficient = 150.0f + (float)(rand() % 50) + mStaticLevel * 15.0f;
        const float alpha = (mStaticTimer > 0.0f || mCameraCutTimer > 0.0f) ? 1.0f : 1.0f - coefficient / 255.0f;
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
