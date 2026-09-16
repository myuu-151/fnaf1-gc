#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "AssetRef.h"
#include "FnafData.h"

class Widget;
class Quad;
class Text;
class SoundWave;

class FnafGame
{
public:

    bool Initialize();
    void Update(float deltaTime);

private:

    enum class Room : uint8_t
    {
        ShowStage,
        DiningArea,
        PirateCove,
        Backstage,
        Restrooms,
        Kitchen,
        WestHall,
        SupplyCloset,
        WestCorner,
        EastHall,
        EastCorner,
        LeftDoor,
        RightDoor,
        Office,
    };

    enum class State : uint8_t
    {
        Loading,
        Menu,
        Newspaper,
        NightIntro,
        Playing,
        PowerOut,
        Jumpscare,
        GameOver,
        Win,
        CreepyEnd,      // Golden Freddy got you: his face and scream for 1 s, then the GameCube resets
        Ending,         // after the last night: the paycheck (or the termination notice)
    };

    struct Animatronic
    {
        const char* mName = "";
        bool mLeftSide = true;
        Room mRoom = Room::ShowStage;
        float mMoveTimer = 0.0f;
        float mMoveInterval = 5.0f;
        float mOfficeTimer = 0.0f;
        bool mSeenAtDoor = false;
        int32_t mPose = 1;              // 1 or 2: which picture of her some cameras show (re-rolled on every move roll)
        bool mAttackArmed = false;      // inside, and the cameras have been up since: lowering them starts the jumpscare
        float mTabletUpInside = 0.0f;   // time the cameras have been up while she's inside (30 s pulls them down)
    };

    struct Door
    {
        bool mClosed = false;
        bool mLight = false;
        float mProgress = 0.0f;     // 0 = open, 1 = closed
        std::vector<Sprite> mFrames;
        Quad* mQuad = nullptr;
        Quad* mButton = nullptr;
    };

    // Data is loaded a few files per frame so the load bar can move.
    enum class LoadType : uint8_t
    {
        Image,
        Sprite,
        Sound,
    };

    struct LoadJob
    {
        LoadType mType = LoadType::Image;
        std::string mName;          // image/sound name, or sprite file
        Sprite* mSprite = nullptr;  // sprite destination
    };

    bool ReadManifest();
    void QueueLoadJobs();
    bool RunLoadJob(const LoadJob& job);
    void UpdateLoading();

    void BuildUi();
    void BuildLoadingUi();
    void StartNight();

    void BuildMenuUi();
    void EnterMenu();
    void UpdateMenu(float deltaTime);
    void StartNightIntro();
    void ShowMenuWidgets(bool menu, bool newspaper, bool intro);
    void PlaceSprite(Quad* quad, const Sprite& sprite, float x, float y);
    void UpdateGameOver(float deltaTime);
    void StartWin();
    void UpdateWin(float deltaTime);
    void StartEnding(const char* image);
    void UpdateEnding(float deltaTime);
    void LoadProgress();
    void SaveProgress();

    void UpdatePlaying(float deltaTime);
    void UpdateInput(float deltaTime);
    void UpdateAnimatronics(float deltaTime);
    void UpdateTablet(float deltaTime);
    void UpdateJumpscare(float deltaTime);
    void UpdateView(float deltaTime);
    void UpdateTabletUi(float deltaTime, bool cameraOn);
    void StartCameraFlash();
    void UpdateHud();

    void MoveAnimatronic(Animatronic& a);
    void UpdateFreddy(float deltaTime);
    bool MoveFreddy();      // false when his room's conditions block it, so the move stays pending
    void UpdateFoxy(float deltaTime);
    void FoxyArrive();
    void LowerTablet();
    void RaiseTablet();
    void StartPowerOut();
    void UpdatePowerOut(float deltaTime);
    void StartJumpscare(const std::string& who);
    void UpdateRandomSounds(float deltaTime);
    void UpdateEerieAndPower(float deltaTime);
    void UpdateGoldenFreddy(float deltaTime);
    void LoadGoldenSprite();
    void StartCreepyEnd();
    void UpdateCreepyEnd(float deltaTime);
    void SetLight(bool left, bool on);
    void ShowMessage(const std::string& message);

    int32_t GetAi(const Animatronic& a) const;
    const char* GetCallFile() const;
    bool IsAt(const Animatronic& a, Room room) const;
    std::string GetCameraImage(Room camera) const;
    float GetPirateSongVolume() const;
    std::string GetOfficeImage() const;
    void ShowImage(YuvCanvas& canvas, const std::string& name, std::string& shown);
    void StartAnimPreload(const std::string& prefix, int32_t count, size_t first);

    void PlaySound(const char* name, bool loop = false, float volume = 1.0f);
    void StopSound(const char* name);
    void StopStreams();

    // Data
    std::unordered_map<std::string, std::vector<uint8_t>> mImages;
    std::vector<uint8_t> mFrameBuffer;      // animation frame read from the disc
    std::unordered_map<std::string, int32_t> mCounts;
    std::unordered_map<std::string, AssetRef> mSounds;
    std::vector<Sprite> mFlipFrames;
    std::vector<Sprite> mFanFrames;
    Sprite mButtons[2][2][2];   // [side][doorClosed][lightOn]

    // Long sounds streamed from the disc
    PcmPlayer mCall;
    PcmPlayer mAmbience;
    PcmPlayer mMusicBox;
    PcmPlayer mRareMusic;       // Foxy's pirate song or the circus tune, one at a time
    bool mRareMusicIsPirate = false;
    float mRareMusicVolume = 0.0f;
    PcmPlayer mFanSound;
    PcmPlayer mJingle;          // 6 AM chimes, or Freddy's laugh during the power-out
    PcmPlayer mCheer;
    PcmPlayer mMenuMusic;
    PcmPlayer mMenuHum;
    PcmPlayer mEerie;           // eerie ambience, louder as the animatronics get close
    PcmPlayer mBreath;          // breathing when someone gets into the office
    PcmPlayer mTapeSound;       // MiniDV tape (stereo) while the cameras are open
    PcmPlayer mRobotVoice;      // under the hallucination flashes (the night's 8th stream)
    float mEerieVolume = 0.0f;
    int32_t mAmbienceLayer = -1;    // how many of Bonnie and Chica are off the stage (eerie ambience level)
    // The engine runs at most 4 streams: the night has call, ambience, fan and rare music;
    // StopStreams() clears them before the power-out (music box, laugh) and 6 AM (chimes, cheer).

    std::vector<LoadJob> mLoadJobs;
    size_t mLoadNext = 0;
    bool mLoadFailed = false;
    bool mLoadFailedLogged = false;

    YuvCanvas mOfficeCanvas;
    YuvCanvas mCameraCanvas;
    YuvCanvas mStaticCanvas;
    YuvCanvas mJumpCanvas;
    std::string mOfficeShown;
    std::string mCameraShown;

    // UI
    Widget* mRoot = nullptr;
    Quad* mOffice = nullptr;
    Quad* mFan = nullptr;
    Quad* mFlip = nullptr;
    Quad* mCamera = nullptr;
    Quad* mCameraBlack = nullptr;
    Quad* mStatic = nullptr;
    // Tablet map overlay: the floor plan (blinking between two pictures), a green dot on the camera
    // being watched, and the white bands of the camera-switch flash.
    Quad* mMap = nullptr;
    Quad* mMapButtons[11] = {};
    Quad* mMapLabels[11] = {};
    Quad* mFlashBands[2] = {};
    Quad* mCamBorder[4] = {};   // the white frame around the camera feed
    Quad* mCamRec = nullptr;    // blinking red recording dot
    Sprite mCamRecSprite;
    Quad* mFlipBar = nullptr;   // the tablet bar at the bottom of the office
    Sprite mFlipBarSprite;
    Quad* mUsageMeter = nullptr;        // the original's 5 usage pictures, one per level
    Sprite mUsageSprites[5];
    Sprite mMapPlainSprite;
    Sprite mMapConesSprite;
    Sprite mMapButtonOnSprite;
    Sprite mMapButtonOffSprite;
    Sprite mMapLabelSprites[11];
    float mMapBlinkTime = 0.0f;
    float mFlashTime = -1.0f;       // < 0 = no flash playing
    Quad* mJump = nullptr;
    Text* mTimeText = nullptr;
    Text* mNightText = nullptr;
    Text* mPowerText = nullptr;
    Text* mUsageText = nullptr;
    Text* mCameraText = nullptr;
    Text* mAudioOnlyText = nullptr;     // the Kitchen's sign, centred near the top as in the original
    Text* mMessageText = nullptr;
    Quad* mLoadBack = nullptr;
    Quad* mLoadBarBack = nullptr;
    Quad* mLoadBar = nullptr;
    Text* mLoadText = nullptr;

    // Main menu, the new-game newspaper and the "12:00 AM / 1st Night" intro
    Quad* mMenuBlack = nullptr;
    Quad* mMenuBack = nullptr;          // Freddy's face, or the newspaper
    Quad* mMenuStaticQuad = nullptr;
    Quad* mMenuTitle = nullptr;
    Quad* mMenuNewGame = nullptr;
    Quad* mMenuContinue = nullptr;
    Quad* mMenuArrows = nullptr;
    Quad* mMenuCopyright = nullptr;
    Text* mMenuSixthText = nullptr;     // "6th Night", offered once night 5 is beaten
    Text* mIntroClockText = nullptr;    // "12:00 AM" and "1st Night"
    Text* mIntroNightText = nullptr;
    Sprite mMenuTitleSprite;
    Sprite mMenuNewGameSprite;
    Sprite mMenuContinueSprite;
    Sprite mMenuArrowsSprite;
    Sprite mMenuCopyrightSprite;
    std::string mMenuShown;
    int32_t mMenuSelection = 0;         // 0 = New Game, 1 = Continue
    float mMenuTimer = 0.0f;
    float mMenuFrameTimer = 0.0f;
    Text* mGameOverLabel = nullptr;
    float mGameOverTimer = 0.0f;
    float mGameOverRollTimer = 0.0f;    // the game over screen's 1-in-10000-per-second roll
    bool mGameOverRare = false;         // it came up: the "creepy end" follows instead of the title
    float mScreenWidth = 640.0f;
    float mScreenHeight = 480.0f;

    // Game state
    State mState = State::Loading;
    Door mDoors[2];             // [0] left, [1] right
    Animatronic mBonnie;
    Animatronic mChica;

    // Freddy. He only moves with the cameras down, waits out a countdown between moves (the
    // original's 1000 - activity x 100 frames), and leaves the stage last. Once he's in the
    // office he whispers until a 1-in-4-per-second roll kills you.
    Animatronic mFreddy;
    int32_t mFreddyActivity = 0;    // his own activity level: rolled at the start of the night
    float mFreddyWait = 0.0f;       // frames waited since his move roll came up (60 fps)
    bool mFreddyReady = false;      // the roll came up: he's counting down to the move
    bool mFreddyPending = false;    // the countdown finished: he moves as soon as the room allows
    bool mFreddyWasInKitchen = false;
    bool mFreddyInOffice = false;
    float mFreddyKillTimer = 0.0f;  // 1 s between his kill rolls once he's inside
    float mFreddyMusicTimer = 0.0f; // the music box he plays in the kitchen every 300 s

    // Foxy: Pirate Cove stage 0 (curtains closed) .. 3 (gone), then runs to the left door.
    int32_t mFoxyStage = 0;
    float mFoxyMoveTimer = 0.0f;
    float mFoxyLockTimer = 0.0f;    // can't move for a while after the cameras were up
    float mFoxyRunTimer = 0.0f;     // time since he left the cove (25 s to the door) or started running (1.67 s)
    bool mFoxyRunning = false;      // the West Hall run is playing (the original's progress 4)
    bool mFoxyAtDoor = false;       // at the left door, waiting for the tablet to go down (progress 5)
    int32_t mFoxyRunFrame = 0;
    float mFoxyRunFrameTimer = 0.0f;
    int32_t mFoxyKnocks = 0;
    float mNightTime = 0.0f;
    int32_t mNight = 1;             // which night is being played
    int32_t mSavedNight = 1;        // what Continue starts (the original's "level" in its .ini)
    bool mCallPlayed[7] = {};       // the original's "play voice N": a retried night has no call again
    bool mBeatGame = false;         // night 5 done: the 6th night is offered on the menu
    float mEndingTimer = 0.0f;      // the paycheck screen's 15 s
    std::string mEndingImage;
    int32_t mHour = 0;
    int32_t mGlitchRoll = 1;        // the original's 1-30, re-rolled every 50 ms (#387)
    float mGlitchTimer = 0.0f;
    float mVoiceRollTimer = 0.0f;   // robot voice volume roll, every 100 ms (#381-#384)
    float mPower = 100.0f;
    int32_t mUsage = 1;
    float mPowerDrainTimer = 0.0f;  // the night's extra drain (#341-#344)
    float mOfficePan = 0.5f;    // 0..1
    bool mTabletUp = false;
    float mTabletProgress = 0.0f;
    float mTabletUpTime = 0.0f;
    int32_t mCameraIndex = 0;
    int32_t mRandomForPic = 0;      // the original's "random for pic": Random(100) + 1, rolled when the tablet goes down
    bool mCameraFresh = true;       // the view just opened or switched (no garble for that change)
    float mPotsTimer = 0.0f;        // Chica rattling pots in the kitchen
    float mPirateSongTimer = 0.0f;  // rolls for Foxy's pirate song every 4 s
    float mCircusTimer = 0.0f;      // rolls for the circus tune every 5 s
    float mPoundingTimer = 0.0f;    // rolls for door pounding every 10 s
    float mGroanTimer = 0.0f;       // rolls for a groan every 5 s while someone is in the office
    // 6 AM screen (the original's "next day" frame)
    int32_t mWinPhase = 0;          // 0 fading in, 1 running, 2 fading out
    float mWinFadeTime = 0.0f;
    float mWinTimer = 0.0f;         // running time (the frame's events are paused during its fades)
    bool mWinCheered = false;       // the 5 has stopped: kids cheering, and the 201-frame countdown runs
    float mWinCheerTime = 0.0f;
    Text* mWinFiveText = nullptr;   // the digit rolls from 5 to 6; "AM" stays put
    Text* mWinSixText = nullptr;
    Text* mWinAmText = nullptr;
    Quad* mWinMaskTop = nullptr;
    Quad* mWinMaskBottom = nullptr;
    bool mLaughed = false;          // Freddy's laugh during the power-out
    float mCameraPanTime = 0.0f;
    float mFlickerTimer = 0.0f;
    bool mHallLit = false;          // CAM 2A caught by the light this flicker step (3 in 10)
    bool mLightDropout = false;     // a hall light drops out this flicker step (1 in 10)
    float mStaticTimer = 0.0f;
    float mStaticFrameTimer = 0.0f;
    int32_t mStaticFrame = 0;
    float mFanTime = 0.0f;
    float mPowerOutTimer = 0.0f;
    int32_t mPowerOutPhase = 0;     // 0 dark, 1 music box + Freddy's face, 2 lights flicker out, 3 pitch black
    bool mPowerOutFlickerDark = false;  // phase 2: this frame is black
    float mPowerOutPhaseTimer = 0.0f;
    float mPowerOutRollTimer = 0.0f;
    float mFreddyFlickerTimer = 0.0f;
    bool mFreddyFaceOn = false;
    int32_t mStaticLevel = 0;       // camera static: re-rolled 0-2 every second
    float mStaticLevelTimer = 0.0f;
    float mCameraCutTimer = 0.0f;   // feed cut to static after someone moved on the watched camera
    std::string mJumpWho;
    int32_t mJumpFrame = 0;
    float mJumpTimer = 0.0f;

    // Golden Freddy ("yellow bear" in the original). mYellowBear is its alterable value A:
    // 0 idle, 1 armed (his poster is on CAM 2B), 2 seen (he sits in the office once the cameras go down).
    int32_t mYellowBear = 0;
    float mYellowBearShownTime = 0.0f;  // its value B: frames he's been in the office (300 = 5 s ends the game)
    bool mYellowBearShown = false;
    bool mYellowBearWasShown = false;   // for the original's "only once" on showing him
    float mYellowBearRollTimer = 0.0f;  // every 1 s, a 1 in 100000 chance to arm him
    Quad* mGoldenQuad = nullptr;
    Sprite mGoldenSprite;

    // The hallucination flashes ("Active 21"): for 100 frames, each frame has a 1 in 10 chance to show.
    bool mHallucination = false;
    float mHallucinationTime = 0.0f;
    float mHallucinationStepTimer = 0.0f;   // the original re-rolls every frame at 60 fps
    bool mHallucinationVisible = false;
    float mHallucinationRollTimer = 0.0f;   // every 1 s, a 1 in 1000 chance of a hallucination
    bool mRobotVoiceOn = false;
    Quad* mHallucinationQuad = nullptr;
    std::string mHallucinationShown;
    float mCreepyEndTimer = 0.0f;
};
