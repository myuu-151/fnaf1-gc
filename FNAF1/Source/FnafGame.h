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
        Playing,
        PowerOut,
        Jumpscare,
        GameOver,
        Win,
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

    void UpdatePlaying(float deltaTime);
    void UpdateInput(float deltaTime);
    void UpdateAnimatronics(float deltaTime);
    void UpdateTablet(float deltaTime);
    void UpdateJumpscare(float deltaTime);
    void UpdateView(float deltaTime);
    void UpdateHud();

    void MoveAnimatronic(Animatronic& a);
    void StartJumpscare(const std::string& who);
    void SetLight(bool left, bool on);
    void ShowMessage(const std::string& message);

    int32_t GetAi(const Animatronic& a) const;
    bool IsAt(const Animatronic& a, Room room) const;
    std::string GetCameraImage(Room camera) const;
    std::string GetOfficeImage() const;
    void ShowImage(YuvCanvas& canvas, const std::string& name, std::string& shown);

    void PlaySound(const char* name, bool loop = false, float volume = 1.0f);
    void StopSound(const char* name);

    // Data
    std::unordered_map<std::string, std::vector<uint8_t>> mImages;
    std::unordered_map<std::string, int32_t> mCounts;
    std::unordered_map<std::string, AssetRef> mSounds;
    std::vector<Sprite> mFlipFrames;
    std::vector<Sprite> mFanFrames;
    Sprite mButtons[2][2];      // [doorClosed][lightOn]

    std::vector<LoadJob> mLoadJobs;
    size_t mLoadNext = 0;
    bool mLoadFailed = false;

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
    Quad* mStatic = nullptr;
    Quad* mJump = nullptr;
    Text* mTimeText = nullptr;
    Text* mNightText = nullptr;
    Text* mPowerText = nullptr;
    Text* mUsageText = nullptr;
    Text* mCameraText = nullptr;
    Text* mMessageText = nullptr;
    Quad* mLoadBack = nullptr;
    Quad* mLoadBarBack = nullptr;
    Quad* mLoadBar = nullptr;
    Text* mLoadText = nullptr;
    float mScreenWidth = 640.0f;
    float mScreenHeight = 480.0f;

    // Game state
    State mState = State::Loading;
    Door mDoors[2];             // [0] left, [1] right
    Animatronic mBonnie;
    Animatronic mChica;
    float mNightTime = 0.0f;
    int32_t mHour = 0;
    float mPower = 100.0f;
    int32_t mUsage = 1;
    float mOfficePan = 0.5f;    // 0..1
    bool mTabletUp = false;
    float mTabletProgress = 0.0f;
    float mTabletUpTime = 0.0f;
    int32_t mCameraIndex = 0;
    float mCameraPanTime = 0.0f;
    float mStaticTimer = 0.0f;
    float mStaticFrameTimer = 0.0f;
    int32_t mStaticFrame = 0;
    float mFanTime = 0.0f;
    float mPowerOutTimer = 0.0f;
    std::string mJumpWho;
    int32_t mJumpFrame = 0;
    float mJumpTimer = 0.0f;
};
