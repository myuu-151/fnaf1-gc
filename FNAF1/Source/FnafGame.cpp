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

#if PLATFORM_DOLPHIN
#include <ogc/system.h>
#endif

// ---- Deliberate departures from the original -------------------------------------------------
//
// This file is written against the original's decoded events, so anything that doesn't match them
// is either a bug or a choice. The choices are listed here and tagged "PORT:" where they happen,
// so a future reading of the events doesn't turn them back into "fixes".
//
//  1. Input is a controller, not a mouse. The office pans with the stick instead of the cursor's
//     screen zones, and buttons are pressed rather than clicked. The door and light buttons also
//     have no shared click cooldown (the original locks all four for ~167 ms): the instant
//     response was preferred.
//  2. Some of the original's lettering is drawn with the game's own text: the clock and night on
//     the HUD, the camera name, the 6 AM clock and the night intro. Where its pictures are used
//     they are exact (the menu, "Night N" beside Continue, the 6th night).
//  3. Pictures are JPEG at a lower resolution than the original's, read from the disc and decoded
//     as they are shown, so anything the original does per 60 Hz tick has to earn its cost here:
//       - the camera and menu static animate at 20 fps, not 60;
//       - the hall-light flicker is rolled every 100 ms rather than every frame, and its two
//         effects (the office dropout, CAM 2A lit) are rolled separately where the original drives
//         both from one value;
//       - Bonnie's and Chica's attacks cut to the death screen when their animation has played
//         out, not on the original's flat 40-frame timer, which would truncate it here;
//       - the door animations keep every other picture (9 of 16), which is more than a 0.2 s
//         close can show at this frame rate.
//  4. Sound volumes are tuned for Octave's mixer, not converted from the original's channel
//     volumes. Related: only one garble sound plays when someone moves on the watched camera
//     (the original re-fires them for ~10 frames), and the robot voice is started and stopped
//     rather than left looping all night at volume 0, because a stream here holds one of a small
//     number of slots.
//  5. Effects kept because they were liked, though the original has no such thing: the burst of
//     solid static on a camera switch, and the title screen's static looping instead of playing
//     once.
//  6. Nights 1-6 only. There is no custom night, so its ending (the plain $120.00 cheque) is
//     never shown, and the menu has no seventh option.
//  7. Golden Freddy's ending resets the console, where the original closes the game.
//  8. Foxy's arrival snaps the view to the left door; in the original it drifts there because the
//     cursor is sitting on the tablet bar, which a controller has no equivalent for.
//
// The rule behind most of item 3, learned the slow way: the original's timings describe a game
// whose pictures were already in memory. Ours arrive with a disc read and a JPEG decode behind
// them, so copying a number that was free there can cost something here — and it costs most on
// whatever the player does most often. Matching the static's 60 Hz animation, the flat 40-frame
// cut on Bonnie's and Chica's attacks, the 0.367 s tablet flip and the 20-frame wait after
// choosing a menu option were all tried against the events and taken back out, because each one
// spent real time to reproduce something nobody could see. The fades, the blip flash and the
// picture choices were worth matching exactly, because they cost nothing extra. When a finding
// says "the original does X every tick", the question to ask first is what X costs us.
//
// ----------------------------------------------------------------------------------------------

// Layout of the original 1600x720 office, in its own pixels.
static constexpr float kOfficeWidth = 1600.0f;
static constexpr float kOfficeHeight = 720.0f;
static constexpr float kDoorX[2] = { 72.0f, 1270.0f };
// The pictures' own sizes, not their opaque areas: both doors and the button panels carry
// transparent margins, and drawing a trimmed crop at the untrimmed origin put them off the
// doorways and recesses painted into the office.
static constexpr float kDoorWidth[2] = { 223.0f, 248.0f };
static constexpr float kButtonX[2] = { 6.0f, 1497.0f };
static constexpr float kButtonY[2] = { 263.0f, 273.0f };
static constexpr float kButtonWidth = 92.0f;
static constexpr float kButtonHeight = 247.0f;
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
// PORT: the flip panel's 11 frames run at animation speed 50 (30 fps), so the original's flip takes
// 0.367 s. Ours stays at a quarter second: raising and lowering the tablet is the thing you do most
// of all night, and the slower one reads as sluggish when every flip also carries a disc read and a
// decode behind it. (The 11th picture is still drawn, so the feed doesn't appear a frame early.)
static constexpr float kTabletSpeed = 4.0f;
// PORT: the stick drives the pan. Full deflection matches the original's fast mouse zones (5 px a
// frame across its 320 px of travel) and a light push its slow ones (2 px a frame).
static constexpr float kPanSpeed = 0.9f;         // office pan, screens/second
// PORT: the original never makes its static solid — a switch only flashes the white bands. This
// quarter second of opaque static is ours, and kept because it was liked.
static constexpr float kStaticSeconds = 0.25f;
// PORT: the static's own animation runs at speed 99/100, a picture per 60 Hz tick, but each picture
// is a JPEG we decode: stepping it every frame costs a decode per frame whenever the cameras or the
// menu are up, and looked no different on screen. It stays at 20 fps.
static constexpr float kStaticFrameSeconds = 0.05f;
static constexpr float kJumpFrameSeconds = 1.0f / 24.0f;
static constexpr float kFanFrameSeconds = 1.0f / 59.4f;  // its animation speed 99, like Chica's jumpscare
static constexpr uint64_t kLoadBudgetUs = 30000;  // loading work per frame

// Activity levels, straight from the original's events (#304-#309): each night sets everyone's
// level at the start, and nobody's rises with the hour except through the bumps below. A move
// happens when Random(20) + 1 is at or below the level.
//
// Freddy's is rolled once per night: night 4 is 1 + Random(2), nights 5 and 6 are 2 and 3 + the
// same roll, so he varies between runs of the same night.
struct NightActivity
{
    int32_t mBonnie;
    int32_t mChica;
    int32_t mFoxy;
    int32_t mFreddy;        // his base
    int32_t mFreddyRandom;  // plus Random(mFreddyRandom): 1 means a fixed level
};
static constexpr NightActivity kNightActivity[7] =
{
    {  0,  0, 0, 0, 1 },    // (night 0: unused)
    {  0,  0, 0, 0, 1 },    // night 1: nobody moves until the hourly bumps
    {  3,  1, 1, 0, 1 },    // night 2
    {  0,  5, 2, 1, 1 },    // night 3
    {  2,  4, 6, 1, 2 },    // night 4
    {  5,  7, 5, 2, 2 },    // night 5
    { 10, 12, 6, 3, 2 },    // night 6
};

// The hourly bumps (#334-#336): Bonnie gains one at 2 AM, and Bonnie, Chica and Foxy each gain one
// at 3 AM and again at 4 AM. Freddy never gains any.
static const NightActivity& GetNightActivity(int32_t night)
{
    return kNightActivity[glm::clamp(night, 0, 6)];
}

// Extra power drain per night (#341-#344): one percent every 6 s on night 2, 5 s on night 3,
// 4 s on night 4 and 3 s from night 5, on top of what's switched on. Night 1 has none.
static float GetExtraDrainInterval(int32_t night)
{
    switch (night)
    {
    case 2:  return 6.0f;
    case 3:  return 5.0f;
    case 4:  return 4.0f;
    default: return (night >= 5) ? 3.0f : 0.0f;
    }
}

// Rare camera pictures (the original rolls a "random for pic" counter): Freddy staring on
// the Show Stage, and rare posters on empty cameras. Rolled when the tablet goes up or the
// camera changes. Placeholder odds: the real value is in the game's compiled events, not
// decoded yet.

// Main menu timing
static constexpr float kMenuFrameSeconds = 0.08f;   // Freddy's face frame and flicker
// "Night N" beside Continue, in the original's own lettering. Both pictures are drawn at their
// real size rather than through the menu's half-scale path, so these are the original's pixels
// 1:1 and the placement is exact: measured in the pictures themselves, the word's ink fills its
// whole 63x22 box (the g reaches the last row) with the N's baseline at row 17, and the digit's
// ink sits at x 3..11 of its 14x17 box, ending at row 16. Lining the digit's ink bottom up with
// that baseline, and leaving a 6 px gap after the word, gives the positions below. The stored
// pictures are padded up to the 4-pixel alignment the texture format needs.
static constexpr float kMenuNightWordX = 175.0f;
static constexpr float kMenuNightWordY = 517.0f;
static constexpr float kMenuNightWordW = 64.0f;     // 63 padded
static constexpr float kMenuNightWordH = 24.0f;     // 22 padded
// Horizontally the original's own counter sets the gap: it centres single digits on x 263, which
// with the glyph's 3 px ink inset starts the number at 259, about 21 px clear of the word rather
// than the 6 px I first tried (6 px of a 1280-wide screen is only 3 px of the console's 640).
static constexpr float kMenuNightDigitX = 263.0f - 7.0f;
static constexpr float kMenuNightDigitY = 517.0f + 17.0f - 16.0f;         // word baseline - digit ink bottom
static constexpr float kMenuNightDigitW = 16.0f;    // 14 padded
static constexpr float kMenuNightDigitH = 20.0f;    // 17 padded
// (The console draws the original's 1280x720 layout on a 640x480 screen, so these come out at half
// the pixel size the original gives them. Drawing them at one source pixel per screen pixel was
// tried and looks too big next to Continue, since the rest of the menu is still halved.)
static constexpr float kNewspaperSeconds = 5.0f;    // help-wanted ad after New Game
static constexpr float kNightIntroSeconds = 131.0f / 60.0f;   // "12:00 AM / 1st Night": its 130-frame count
static constexpr float kGameOverStaticSeconds = 10.8f;  // static before the game over screen (the static sound's length)
static constexpr float kGameOverSeconds = 10.0f;        // game over screen, then the menu

// Foxy
static constexpr float kFoxyMoveInterval = 5.01f;
static constexpr float kFoxyArriveSeconds = 25.0f;     // after leaving the cove, if nobody watches the hall
static constexpr float kFoxyRunFrameSeconds = 1.0f / 39.0f;  // animation speed 65 at 60 fps
static constexpr float kFoxyRunSeconds = 100.0f / 60.0f;  // the original's run: 100 frames at 60 fps
// When Foxy forces the tablet down, the view goes to the left door, where his lunge comes in.
// In the original it drifts there instead: the cursor is still on the tablet bar, inside the
// "left low" pan zone, which moves the view 2 px per frame over its 320 px range (#82). That rate,
// as a share of the pan range per second at 60 fps, is kept here for the drift version:
//     mOfficePan = glm::max(0.0f, mOfficePan - kFoxyPanPerSecond * deltaTime);
// (unused: with a controller there's no cursor holding the view, so ours snaps instead)
static constexpr float kFoxyPanPerSecond = 2.0f * 60.0f / 320.0f;

// Load cost of animation frames (jumpscares, Foxy's run): summed while one plays, logged when it ends.
static constexpr uint32_t kAnimStatsMaxFrames = 40;
struct AnimFrameStat
{
    int32_t index = -1;             // frame number
    bool preloaded = false;         // read ahead by the reader thread
    uint32_t readUs = 0;
    uint32_t decodeUs = 0;
    uint32_t sincePreviousUs = 0;   // from the previous frame's load to this one's
};
struct AnimLoadStats
{
    uint32_t frames = 0;
    uint64_t readUs = 0;
    uint64_t decodeUs = 0;
    uint64_t worstUs = 0;
    uint64_t lastShowUs = 0;
    AnimFrameStat perFrame[kAnimStatsMaxFrames];
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

        // One line per frame: which frame, pre-read or read now, read and decode time, and the time
        // since the previous frame was loaded (the gap the player sees).
        const uint32_t logged = glm::min(sAnimStats.frames, kAnimStatsMaxFrames);
        for (uint32_t i = 0; i < logged; ++i)
        {
            const AnimFrameStat& stat = sAnimStats.perFrame[i];
            OctLog("FNAF1:   frame %2d %s read %5.1f ms decode %5.1f ms gap %5.1f ms", stat.index,
                   stat.preloaded ? "pre-read" : "read now", stat.readUs / 1000.0f, stat.decodeUs / 1000.0f,
                   stat.sincePreviousUs / 1000.0f);
        }
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
    "giggle", "freddysteps",
};

// Golden Freddy in the office: image 573's top-left in the 1600x720 office, and its size.
static constexpr float kGoldenX = 390.0f;
static constexpr float kGoldenY = 218.0f;
static constexpr float kGoldenWidth = 541.0f;
static constexpr float kGoldenHeight = 521.0f;

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

    // The hallucination flashes cover the whole screen, cameras and all. They share the jumpscare
    // canvas, which nothing else uses during a night.
    mHallucinationQuad = mRoot->CreateChild<Quad>("Hallucination");
    mHallucinationQuad->SetTexture(mJumpCanvas.GetTexture());
    mHallucinationQuad->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
    mHallucinationQuad->SetVisible(false);

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
                                "cam2a_dark", "cam2a_empty", "cam2a_bonnie",
                                // the hallucination flashes swap pictures many times a second
                                "hallucination_0", "hallucination_1", "hallucination_2", "hallucination_3" })
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
    queueSprite("spr/menu_sixth.rgx", &mMenuSixthSprite);
    queueSprite("spr/menu_star.rgx", &mMenuStarSprite);
    queueSprite("spr/menu_eye.rgx", &mCreepyEyeSprite);
    queueSprite("spr/menu_nightword.rgx", &mMenuNightWordSprite);
    for (int32_t d = 0; d < 10; ++d)
    {
        char digit[32];
        snprintf(digit, sizeof(digit), "spr/menu_digit%d.rgx", d);
        queueSprite(digit, &mMenuDigitSprites[d]);
    }
    queueSprite("spr/menu_arrows.rgx", &mMenuArrowsSprite);
    queueSprite("spr/menu_copyright.rgx", &mMenuCopyrightSprite);
    // (The night intro, 6 AM clock and game over are drawn with our own text, not the original's
    // pictures.)
    // Foxy's run (33 frames, ~880 KB at quality 50) stays in RAM: read from the SD while the
    // cameras are up, a frame took 50-117 ms and the run lagged. From RAM every frame is only the
    // ~22 ms decode. Room comes from the engine freeing its boot splash texture (~1.3 MB).
    for (int32_t i = 0; i < mCounts["foxyrun"]; ++i)
    {
        snprintf(name, sizeof(name), "foxyrun_%02d", i);
        queueImage(name);
    }
    // Tablet map overlay: the floor plan's two blink pictures, the camera button (green when
    // watched, grey otherwise) and the 11 camera names.
    queueSprite("spr/map_plain.rgx", &mMapPlainSprite);
    queueSprite("spr/map_cones.rgx", &mMapConesSprite);
    queueSprite("spr/map_btn_on.rgx", &mMapButtonOnSprite);
    queueSprite("spr/map_btn_off.rgx", &mMapButtonOffSprite);
    for (int32_t i = 0; i < kNumCameras; ++i)
    {
        std::string id = kCameras[i].mId;
        for (char& c : id) { c = (char)tolower((unsigned char)c); }
        queueSprite("spr/map_lbl_" + id + ".rgx", &mMapLabelSprites[i]);
    }

    queueSprite("spr/cam_rec.rgx", &mCamRecSprite);
    queueSprite("spr/flip_bar.rgx", &mFlipBarSprite);
    for (int32_t i = 0; i < 5; ++i)
    {
        char usage[32];
        snprintf(usage, sizeof(usage), "spr/hud_usage_%d.rgx", i + 1);
        queueSprite(usage, &mUsageSprites[i]);
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

    // Golden Freddy sits in the office, in front of the desk and under the tablet.
    mGoldenQuad = mRoot->CreateChild<Quad>("GoldenFreddy");
    mGoldenQuad->SetVisible(false);

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

    // Tablet map, its buttons and names, then the camera-switch flash over everything on the tablet.
    mMap = mRoot->CreateChild<Quad>("Map");
    for (int32_t i = 0; i < kNumCameras; ++i)
    {
        mMapButtons[i] = mRoot->CreateChild<Quad>("MapButton");
        mMapLabels[i] = mRoot->CreateChild<Quad>("MapLabel");
    }
    for (Quad*& edge : mCamBorder)
    {
        edge = mRoot->CreateChild<Quad>("CameraBorder");
        edge->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        edge->SetVisible(false);
    }
    mCamRec = mRoot->CreateChild<Quad>("CameraRec");
    mCamRec->SetVisible(false);

    mFlipBar = mRoot->CreateChild<Quad>("FlipBar");
    mFlipBar->SetVisible(false);

    mUsageMeter = mRoot->CreateChild<Quad>("UsageMeter");
    mUsageMeter->SetVisible(false);

    for (Quad*& band : mFlashBands)
    {
        band = mRoot->CreateChild<Quad>("CameraFlash");
        band->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        band->SetVisible(false);
    }

    // Above the map, where the original puts its camera name (its "location" object at (832, 292)).
    // Sits above the map, where the original puts its own camera name.
    mCameraText = makeText("CameraName", mScreenWidth * 0.63f, mScreenHeight * 0.39f, mScreenWidth * 0.36f, 80.0f, 18.0f);

    // The Kitchen's sign: centred near the top, like the original's "audio only" picture at (464, 69).
    mAudioOnlyText = makeText("AudioOnly", 0.0f, mScreenHeight * 0.10f, mScreenWidth, 60.0f, 20.0f);
    mAudioOnlyText->SetHorizontalJustification(Justification::Center);

    // Right-aligned, close to the screen's border like the original's clock.
    mTimeText = makeText("Time", mScreenWidth - 260.0f, 16.0f, 236.0f, 40.0f, 28.0f);
    mTimeText->SetHorizontalJustification(Justification::Right);
    mNightText = makeText("Night", mScreenWidth - 260.0f, 50.0f, 236.0f, 30.0f, 18.0f);
    mNightText->SetHorizontalJustification(Justification::Right);
    // PORT: the clock, night, power, usage and camera name are the game's own text. The original
    // draws them with picture fonts and a digit counter, which are used where they were worth
    // converting (the menu, "Night N" beside Continue) but not for the whole HUD.
    mPowerText = makeText("Power", 24.0f, mScreenHeight - 80.0f, 300.0f, 30.0f, 20.0f);
    mUsageText = makeText("Usage", 24.0f, mScreenHeight - 50.0f, 300.0f, 30.0f, 20.0f);

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

    SetFade(0.0f);      // whatever screen we came from, the night itself has no transition
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

    // Freddy starts on the stage with the others. His level is rolled once for the night.
    mFreddy = Animatronic();
    mFreddy.mName = "freddyoffice";
    mFreddy.mLeftSide = false;
    mFreddy.mMoveInterval = 3.02f;      // his move roll, the original's 3020 ms
    const NightActivity& activity = GetNightActivity(mNight);
    mFreddyActivity = activity.mFreddy + (activity.mFreddyRandom > 1 ? rand() % activity.mFreddyRandom : 0);
    mFreddyWait = 0.0f;
    mFreddyReady = false;
    mFreddyPending = false;
    mFreddyWasInKitchen = false;
    mFreddyInOffice = false;
    mFreddyKillTimer = 0.0f;
    mFreddyMusicTimer = 0.0f;
    mPowerDrainTimer = 0.0f;

    mFoxyStage = 0;
    mFoxyMoveTimer = 0.0f;
    mFoxyLockTimer = 0.0f;
    mFoxyRunTimer = 0.0f;
    mFoxyRunning = false;
    mFoxyRunFrame = 0;
    mFoxyRunFrameTimer = 0.0f;
    mFoxyKnocks = 0;
    mFoxyAtDoor = false;
    // The original rolls this whenever the cameras go down (#347), which includes the night's first
    // frame: its counter never stays at 0, and 0 would pass every rare picture's test.
    mRandomForPic = (rand() % 100) + 1;

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
    // Channel volume 100 (2.4 on the fan's 25 = 0.6 scale), capped at the mixer's 2.0. Nights 1-5
    // each have their own call (#361-#365); night 6 has none.
    // Each call plays once per game, not once per attempt: the original's "play voice N" counters
    // are only cleared by New Game, so dying and continuing gives you the night in silence.
    const char* call = (mNight >= 1 && mNight <= 6 && !mCallPlayed[mNight]) ? GetCallFile() : nullptr;
    if (call != nullptr)
    {
        mCallPlayed[mNight] = true;
        char sizeKey[24];
        snprintf(sizeKey, sizeof(sizeKey), "size_%s", call);
        mCall.Start((std::string("snd/") + call + ".pcm").c_str(), (uint32_t)mCounts[sizeKey], false, 2.0f);
    }
    mCameraFresh = true;
    mMapBlinkTime = 0.0f;
    mFlashTime = -1.0f;
    mGlitchRoll = 1;
    mGlitchTimer = 0.0f;
    mVoiceRollTimer = 0.0f;

    // From night 4 the 2B and 4B pictures flicker between three versions every 50 ms, so those
    // stay in RAM instead of being read from the disc each time.
    if (mNight >= 4)
    {
        for (const char* glitch : { "cam2b_bonnie", "cam2b_bonnie_glitch1", "cam2b_bonnie_glitch2",
                                    "cam4b_chica", "cam4b_chica_glitch1", "cam4b_chica_glitch2" })
        {
            if (mImages.find(glitch) == mImages.end())
            {
                ReadDataFile(std::string("img/") + glitch + ".jpg", mImages[glitch]);
            }
        }
    }
    mPotsTimer = 3.0f;
    mPirateSongTimer = 4.0f;
    mCircusTimer = 5.0f;
    mPoundingTimer = 10.0f;
    mGroanTimer = 5.0f;
    mCameraCutTimer = 0.0f;
    mLaughed = false;

    mYellowBear = 0;
    mYellowBearShownTime = 0.0f;
    mYellowBearShown = false;
    mYellowBearWasShown = false;
    mYellowBearRollTimer = 0.0f;
    mYellowBearArmed = false;
    mYellowBearHallucinated = false;
    mHudRevealed = false;
    mMoveWho = 0;
    mHallucination = false;
    mHallucinationTime = 0.0f;
    mHallucinationStepTimer = 0.0f;
    mHallucinationVisible = false;
    mHallucinationRollTimer = 0.0f;
    mRobotVoiceOn = false;
    mHallucinationShown.clear();
    mHallucinationQuad->SetVisible(false);
    mGoldenQuad->SetVisible(false);
    mGoldenQuad->SetTexture(nullptr);
    mGoldenSprite = Sprite();       // frees his picture (loaded again if he's armed)
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
    mRobotVoice.Update();
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
    static const char* kStateNames[] = { "Loading", "Menu", "Newspaper", "NightIntro", "Playing", "PowerOut", "Jumpscare", "GameOver", "Win", "CreepyEnd", "Ending", "CreepyStart" };
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

    case State::CreepyEnd:
        UpdateCreepyEnd(deltaTime);
        break;

    case State::Win:
        UpdateWin(deltaTime);
        break;

    case State::Ending:
        UpdateEnding(deltaTime);
        break;

    case State::CreepyStart:
        UpdateCreepyStart(deltaTime);
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
            StartWin();
            return;
        }
    }

    UpdateGoldenFreddy(deltaTime);
    if (mState == State::CreepyEnd)
    {
        return;
    }

    if (mState == State::PowerOut)
    {
        // Their move rolls (#187, #188) and the rules those feed carry no power condition, so
        // Bonnie and Chica keep wandering in the dark, footsteps and all — down the same path the
        // rest of the night uses, one shared move slot included. This used to run its own copy of
        // the roll, which quietly put them back to moving independently once the lights went out.
        UpdateAnimatronics(deltaTime);
        if (mState != State::PowerOut)
        {
            return;     // she took you
        }

        UpdateRandomSounds(deltaTime);
        UpdatePowerOut(deltaTime);
        return;
    }

    // Debug keys: X = power out, Y = Bonnie's jumpscare, D-pad down = Chica's,
    // D-pad up = Bonnie and Chica at the doors, START = complete the night.
    if (Pressed(GAMEPAD_START))
    {
        // Jump the clock to 6 AM: the hour check above wins the night on the next frame.
        mNightTime = kFirstHourSeconds + 5.0f * kHourSeconds;
        OctLog("FNAF1: debug: night complete");
    }

    // C-stick down = Foxy runs down the West Hall, shown on CAM 2A (cameras raised if needed), to
    // check the run animation. He reaches the left door when it ends, as usual.
    static bool sCStickDownHeld = false;
    const bool cStickDown = INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_Y, 0) < -0.6f;
    if (cStickDown && !sCStickDownHeld)
    {
        RaiseTablet();
        mCameraIndex = (int32_t)Room::WestHall;
        mCameraFresh = true;
        mFoxyStage = 3;
        mFoxyAtDoor = false;
        mFoxyRunning = true;
        mFoxyRunTimer = 0.0f;
        mFoxyRunFrame = 0;
        mFoxyRunFrameTimer = 0.0f;
        PlaySound("run", false, 2.0f);
        OctLog("FNAF1: debug: foxy runs");
    }
    sCStickDownHeld = cStickDown;

    // C-stick up = Golden Freddy: arm his event and open the cameras on CAM 2B, where his poster
    // shows (unless Bonnie is there). Lower the tablet to see him in the office; leave it down for
    // 5 s for the creepy end (which resets the GameCube), or raise it to make him go away.
    static bool sCStickUpHeld = false;
    const bool cStickUp = INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_Y, 0) > 0.6f;
    if (cStickUp && !sCStickUpHeld)
    {
        mYellowBear = 1;
        LoadGoldenSprite();
        RaiseTablet();
        mCameraIndex = (int32_t)Room::WestCorner;
        mCameraFresh = true;
        mStaticTimer = kStaticSeconds;
        OctLog("FNAF1: debug: golden freddy armed");
    }
    sCStickUpHeld = cStickUp;

    // C-stick left = Freddy's office attack, the one kill that needs him to have walked all the way
    // round. Fired the way his own roll does it (#408's path): both lights go out first, then the
    // animation, which runs at 30 fps and holds its scream until picture 7.
    static bool sCStickLeftHeld = false;
    const bool cStickLeft = INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_X, 0) < -0.6f;
    if (cStickLeft && !sCStickLeftHeld)
    {
        sCStickLeftHeld = true;
        SetLight(true, false);
        SetLight(false, false);
        OctLog("FNAF1: debug: freddy's office attack");
        StartJumpscare("freddyoffice");
        return;
    }
    sCStickLeftHeld = cStickLeft;

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
    UpdateFreddy(deltaTime);
    if (mState != State::Playing)
    {
        return;     // he took you
    }
    UpdateFoxy(deltaTime);

    if (mState != State::Playing)
    {
        return;
    }

    UpdateRandomSounds(deltaTime);

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

    UpdateEerieAndPower(deltaTime);
}

void FnafGame::UpdateRandomSounds(float deltaTime)
{
    // These events don't check the power, so they keep playing during a power-out too.

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

    // Rare music, as in the original's events: "every 4 s, Random(30) = 1" plays Foxy's
    // pirate song, and "every 5 s, Random(30) = 1" plays the faint circus tune. The pirate
    // song event also checks a counter we haven't mapped; here it needs Foxy in the cove.
    // #268 tests "fox progress = 0" before its timer, so the 4 s only accumulates while he is
    // still behind the curtain; leaving the cove doesn't bank rolls for when he returns.
    if (mFoxyStage == 0 && !mFoxyRunning)
    {
        mPirateSongTimer -= deltaTime;
    }
    else
    {
        mPirateSongTimer = 4.0f;
    }
    if (mPirateSongTimer <= 0.0f)
    {
        mPirateSongTimer += 4.0f;
        if ((rand() % 30) == 0 && !mRareMusic.IsPlaying())
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
}

void FnafGame::UpdateEerieAndPower(float deltaTime)
{

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
    // Freddy in the office drowns out the rest of it (#358).
    const int32_t channelVolume = mFreddyInOffice ? 100 : kEerieChannelVolume[danger];
    const float eerieVolume = glm::min(2.0f, channelVolume * 0.03f);

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

    // From night 2 the night itself eats power on a timer, whatever you have switched on.
    const float drainInterval = GetExtraDrainInterval(mNight);
    if (drainInterval > 0.0f)
    {
        mPowerDrainTimer += deltaTime;
        while (mPowerDrainTimer >= drainInterval)
        {
            mPowerDrainTimer -= drainInterval;
            // The original's power counter runs 0..999 and is shown as counter/10 (#174, #175),
            // so its "subtract 1" here is a tenth of a percent, not a whole one.
            mPower -= 0.1f;
        }
    }

    if (mPower <= 0.0f)
    {
        StartPowerOut();
    }
}

void FnafGame::StartPowerOut()
{
    mPower = 0.0f;
    SetFade(0.0f);      // no transition on this one: the lights just go
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
            mMusicBox.Start("snd/musicbox.pcm", (uint32_t)mCounts["size_musicbox"], true, 1.0f);
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
            mRareMusic.Stop();      // the original's "stop all sounds" (#293)
            AudioManager::StopAllSounds();
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
            mRareMusic.Stop();      // the original's "stop all sounds" (#297)
            AudioManager::StopAllSounds();
        }
        break;

    default:    // silent darkness: the jumpscare comes on a 1-in-5 roll every 2 s, or after 20 s
        if (roll(2.0f, 5, 20.0f))
        {
            // This death has its own frame in the original (#300/#301 jump to the XSCREAM one),
            // but it amounts to what we already do: the lunge plays once, the static takes over,
            // and the game over screen follows about 12 s after it began.
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
        if (mTabletUp)
        {
            LowerTablet();
        }
        else
        {
            RaiseTablet();
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
                StartCameraFlash();     // plays the blip with it
                OctLog("FNAF1: camera %s", kCameras[mCameraIndex].mId);
            }
        }
        return;
    }

    // Doors: L / R. Lights: D-pad left / right.
    // PORT: no click cooldown. The original locks all four buttons for 10 frames after any press
    // (#94); ours respond instantly, which was preferred over matching it.
    for (int32_t side = 0; side < 2; ++side)
    {
        // (Foxy at the left door hides the door's *graphic* in the original (#327), not its button:
        // #95 and #101 have no condition on his progress, so the slam stays possible.)
        const bool doorPressed = Pressed(side == 0 ? GAMEPAD_L1 : GAMEPAD_R1);
        const bool lightPressed = Pressed(side == 0 ? GAMEPAD_LEFT : GAMEPAD_RIGHT);

        // Bonnie inside kills that side's light and stops you *shutting* the door (#95, #99 guard
        // on her; #96, #100 buzz instead). Opening it again is allowed: #101 and #103 carry no
        // such condition, so you can stop the drain while you wait her out.
        const bool inside = IsAt(side == 0 ? mBonnie : mChica, Room::Office);
        if (inside && (lightPressed || (doorPressed && !mDoors[side].mClosed)))
        {
            PlaySound("error");
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

void FnafGame::RaiseTablet()
{
    if (mTabletUp)
    {
        return;
    }

    mTabletUp = true;
    mTabletUpTime = 0.0f;
    mHudRevealed = true;    // the power and usage displays are shown by the first raise (#5)
    PlaySound("camup");
    // Cameras open: the original plays the MiniDV tape sound (stereo, full volume) on its own
    // channel and turns the fan's channel down (to 10, from 25).
    mTapeSound.Start("snd/minidv.pcm", (uint32_t)mCounts["size_minidv"], false, 1.0f);
    mFanSound.SetVolume(0.24f);
    mCall.SetVolume(1.0f);          // the call's channel goes from 100 to 50 with the cameras up
    SetLight(true, false);
    SetLight(false, false);
    mStaticTimer = kStaticSeconds;
    mCameraFresh = true;
    StartCameraFlash();     // the original's blip flash plays with the first camera too
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
    mCall.SetVolume(2.0f);
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
    // The night's level, plus the hourly bumps.
    const NightActivity& night = GetNightActivity(mNight);
    if (&a == &mBonnie)
        return night.mBonnie + (mHour >= 2) + (mHour >= 3) + (mHour >= 4);

    return night.mChica + (mHour >= 3) + (mHour >= 4);
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

        // For the 10 frames after a successful move roll, every frame she spends on the camera
        // you're watching sets the blackout to 300 frames again (#193/#194). So the feed stays
        // cut for 5 s measured from the last of those frames, not the first.
        if (a->mMoveFlash > 0.0f)
        {
            a->mMoveFlash -= deltaTime;
            const bool cameraOn = mTabletUp && mTabletProgress >= 1.0f;
            if (cameraOn && a->mRoom == (Room)mCameraIndex)
            {
                mCameraCutTimer = 5.0f;
            }
        }

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

        // Window scare (the original's #332/#333): once per visit to the doorway, the first time the
        // office picture actually shows her lit there (light on, cameras down, not a light dropout
        // step). It plays on channel 9 at volume 100, the mixer's maximum here.
        const Room door = a->mLeftSide ? Room::LeftDoor : Room::RightDoor;

        // Both lights go out the moment she arrives at the doorway, and again when she leaves
        // (#225/#226 and #258/#259, edge-triggered on her being there). So you can't hold a light
        // on and watch her walk in: the light drops and you have to press it again. It only
        // happens with the cameras down, and not while Foxy is at the door or Freddy is inside.
        const bool atDoor = a->mRoom == door;
        if (atDoor != a->mWasAtDoor)
        {
            a->mWasAtDoor = atDoor;
            if (!mTabletUp && !mFoxyAtDoor && !mFreddyInOffice)
            {
                SetLight(true, false);
                SetLight(false, false);
            }
        }

        const bool litInDoorway = atDoor && mDoors[side].mLight && !mLightDropout &&
                                  !mTabletUp && mTabletProgress <= 0.0f;
        if (litInDoorway && !a->mSeenAtDoor)
        {
            a->mSeenAtDoor = true;
            PlaySound("windowscare", false, 2.0f);
        }

        // The roll doesn't move her itself: it claims the original's one shared "move who?" slot
        // (#187, #188), and the move rules consume it. Chica's roll runs after Bonnie's, so when
        // both come up on the same tick hers overwrites his and his move is lost — while the
        // camera cut and the pose re-roll he already triggered still stand (#191, #192).
        a->mMoveTimer += deltaTime;
        if (a->mMoveTimer >= a->mMoveInterval)
        {
            a->mMoveTimer -= a->mMoveInterval;
            if ((rand() % 20) + 1 <= GetAi(*a))
            {
                mMoveWho = a->mLeftSide ? 1 : 2;
                a->mMoveFlash = 10.0f / 60.0f;
                a->mPose = (rand() % 2) + 1;
                CutFeedIfWatched(a->mRoom);
            }
        }
    }

    // A claimed move waits until one of her rules matches, rather than being spent on the frame it
    // was rolled: she may be in the office, where no rule covers her, or standing at a doorway
    // while the door is still moving, which the rules read as neither open nor shut.
    if (mMoveWho != 0)
    {
        Animatronic& mover = (mMoveWho == 1) ? mBonnie : mChica;
        if (MoveAnimatronic(mover))
        {
            mMoveWho = 0;
        }
    }
}

const char* FnafGame::GetCallFile() const
{
    // One recording per night (#361-#365). Night 6 has none.
    switch (mNight)
    {
    case 1:  return "call";
    case 2:  return "call2";
    case 3:  return "call3";
    case 4:  return "call4";
    case 5:  return "call5";
    default: return nullptr;    // PORT: night 6 has none here either, and there is no custom night
    }
}

void FnafGame::UpdateFreddy(float deltaTime)
{
    // "viewing" in the original: non-zero while the tablet is up.
    const bool watching = mTabletUp;

    if (mFreddyInOffice)
    {
        // He stands in the dark corner whispering (#404). Every second there's a one-in-four
        // chance he takes you (#405), but only with the cameras down, and not while Foxy is
        // already at the door.
        mFreddyKillTimer += deltaTime;
        if (mFreddyKillTimer >= 1.0f)
        {
            mFreddyKillTimer -= 1.0f;
            if (!watching && !mFoxyAtDoor && (rand() % 4) == 1)
            {
                SetLight(true, false);
                SetLight(false, false);
                StartJumpscare("freddyoffice");
            }
        }
        return;
    }

    // The move roll (#189): every 3.02 s, Random(20) + 1 against his level, with the cameras down.
    mFreddy.mMoveTimer += deltaTime;
    if (mFreddy.mMoveTimer >= mFreddy.mMoveInterval)
    {
        mFreddy.mMoveTimer -= mFreddy.mMoveInterval;
        if (!watching && (rand() % 20) + 1 <= mFreddyActivity)
        {
            mFreddyReady = true;
        }
    }

    // Once the roll comes up he counts down before moving (#396, #397): 1000 frames at level 0
    // down to 400 at level 6, at 60 fps, and only while the cameras are down. Watching the camera
    // he is on puts that count back to zero (#400) — the way you stall him.
    if (mFreddyReady && !mFreddyPending)
    {
        if (watching && (Room)mCameraIndex == mFreddy.mRoom)
        {
            mFreddyWait = 0.0f;
        }
        else
        {
            mFreddyWait += deltaTime * 60.0f;
        }

        if (mFreddyWait >= (float)(1000 - mFreddyActivity * 100) && !watching)
        {
            mFreddyWait = 0.0f;
            mFreddyReady = false;
            mFreddyPending = true;
        }
    }

    // The original holds that "move now" state until one of his room's move events accepts it, so
    // a blocked move waits rather than being thrown away: the stage needs Bonnie and Chica gone
    // (#388), 4A needs the right light off (#392), and the corner needs the cameras UP and pointed
    // somewhere else (#393, #394). So keep offering the move every frame, cameras up or down.
    if (mFreddyPending && MoveFreddy())
    {
        mFreddyPending = false;
    }

    // The music box in the kitchen: once when he arrives (#398), then every 5 minutes he stays
    // (#399). Its channel is 5 normally and 50 while you watch CAM 6 (#251-#257).
    const bool inKitchen = mFreddy.mRoom == Room::Kitchen;
    if (inKitchen)
    {
        const bool watched = watching && (Room)mCameraIndex == Room::Kitchen;
        const float volume = watched ? 1.2f : 0.12f;
        bool play = !mFreddyWasInKitchen;
        if (!play)
        {
            mFreddyMusicTimer += deltaTime;
            if (mFreddyMusicTimer >= 300.0f)
            {
                mFreddyMusicTimer -= 300.0f;
                play = true;
            }
        }

        if (play)
        {
            mFreddyMusicTimer = 0.0f;
            mMusicBox.Start("snd/musicbox.pcm", (uint32_t)mCounts["size_musicbox"], false, volume);
        }
        else if (mMusicBox.IsPlaying())
        {
            mMusicBox.SetVolume(volume);    // it follows the camera while it plays
        }
    }
    else
    {
        mFreddyMusicTimer = 0.0f;
    }
    mFreddyWasInKitchen = inKitchen;
}

bool FnafGame::MoveFreddy()
{
    // His route round the east side, one room per move (#388-#394). The laugh and the footsteps
    // get louder the closer he is: the original's channel volumes, on the mixer's 0..2 scale.
    const Room before = mFreddy.mRoom;
    Room next = before;
    int32_t laughVolume = 0;
    int32_t stepVolume = 0;

    switch (before)
    {
    case Room::ShowStage:
        // He leaves the stage last, once Bonnie and Chica have both gone (#388).
        if (IsAt(mBonnie, Room::ShowStage) || IsAt(mChica, Room::ShowStage))
        {
            return false;
        }
        next = Room::DiningArea; laughVolume = 15; stepVolume = 30;
        break;
    case Room::DiningArea: next = Room::Restrooms;  laughVolume = 20; stepVolume = 35; break;
    case Room::Restrooms:  next = Room::Kitchen;    laughVolume = 30; stepVolume = 40; break;
    case Room::Kitchen:    next = Room::EastHall;   laughVolume = 40; stepVolume = 60; break;
    case Room::EastHall:
        // The right hall light holds him at 4A (#392).
        if (mDoors[1].mLight)
        {
            return false;
        }
        next = Room::EastCorner; laughVolume = 60; stepVolume = 75;
        break;
    case Room::EastCorner:
        // From the corner he only moves while you're on the cameras and not watching him (#393,
        // #394): an open right door lets him in, a closed one sends him back up the hall.
        if (!mTabletUp || (Room)mCameraIndex == Room::EastCorner)
        {
            return false;
        }
        if (mDoors[1].mClosed)
        {
            if ((Room)mCameraIndex == Room::EastHall)
            {
                return false;
            }
            next = Room::EastHall; laughVolume = 60; stepVolume = 75;
        }
        else
        {
            next = Room::Office; laughVolume = 80; stepVolume = 100;
        }
        break;
    default:
        return false;
    }

    mFreddy.mRoom = next;
    OctLog("FNAF1: freddy -> room %d (level %d, hour %d)", (int)next, mFreddyActivity, mHour);

    // One of his three laughs (#401-#403), with the running footsteps under it.
    static const char* kLaughs[] = { "laugh", "laugh2", "laugh3" };
    const char* laugh = kLaughs[rand() % 3];
    char sizeKey[24];
    snprintf(sizeKey, sizeof(sizeKey), "size_%s", laugh);
    mJingle.Start((std::string("snd/") + laugh + ".pcm").c_str(), (uint32_t)mCounts[sizeKey],
                  false, glm::min(2.0f, laughVolume * 0.024f));
    PlaySound("freddysteps", false, glm::min(2.0f, stepVolume * 0.024f));

    if (next == Room::Office)
    {
        mFreddyInOffice = true;
        mFreddyKillTimer = 0.0f;
        // His whispering loops until he takes you. It shares the player with Bonnie's and
        // Chica's breathing: only one of them is ever in the room.
        // One-shot, as the original plays it on the edge of him getting in (#404), on a channel it
        // never turns down. Looping it would also block the office groans, which skip while this
        // player is busy.
        mBreath.Start("snd/whisper.pcm", (uint32_t)mCounts["size_whisper"], false, 2.0f);
    }

    return true;
}

// Feed cut when someone moves on the camera you're watching: the picture is hidden for 300 frames
// (only the faint static shows), with one of the four garble sounds and the same white bands a
// camera switch flashes (#193-#195, #218-#221).
bool FnafGame::CutFeedIfWatched(Room room)
{
    const bool cameraOn = mTabletUp && mTabletProgress >= 1.0f;
    if (cameraOn && room == (Room)mCameraIndex)
    {
        mCameraCutTimer = 5.0f;
        StartCameraFlash();
        // PORT: one sound. The original re-rolls and re-fires these every frame for ~10 frames
        // while she is on the watched camera; ten overlapping sounds is voice pressure here for
        // little audible gain.
        // Random(4) + 1: 1 plays COMPUTER_DIGITAL, 2-4 play garble1-3.
        const int32_t roll = (rand() % 4) + 1;
        static const char* kMoveSounds[] = { "camhum", "garble1", "garble2", "garble3" };
        PlaySound(kMoveSounds[roll - 1], false, 0.7f);
        return true;
    }
    return false;
}

bool FnafGame::MoveAnimatronic(Animatronic& a)
{
    // Nothing covers her once she's inside, so a move claimed while she's in the office stays
    // claimed — as the original's counter does — until she takes you or the other one's roll
    // takes the slot from her.
    if (a.mRoom == Room::Office)
    {
        return false;
    }

    // At a doorway her rules test the door as fully open or fully shut (#213, #214); while it is
    // still moving neither matches, so the move waits rather than being spent.
    const Room doorRoom = a.mLeftSide ? Room::LeftDoor : Room::RightDoor;
    const Door& herDoor = mDoors[a.mLeftSide ? 0 : 1];
    if (a.mRoom == doorRoom && herDoor.mProgress != (herDoor.mClosed ? 1.0f : 0.0f))
    {
        return false;
    }

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

    if (a.mRoom != before)
    {
        a.mSeenAtDoor = false;
        a.mOfficeTimer = 0.0f;

        // Arriving on the camera you're watching; leaving one was already cut when the roll came up.
        CutFeedIfWatched(a.mRoom);
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

    return true;
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
        // PORT: his lunge snaps the view to the left door (in UpdateJumpscare). The original drifts
        // there instead, because the cursor is left sitting on the tablet bar inside a pan zone —
        // a controller has no equivalent. No snap here: a shut door only gets banged on, and the
        // view should stay where you left it.
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
            PlaySound("foxybang", false, 2.0f);
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
        // One picture per update at most (no skipping), as with the jumpscares.
        if (mFoxyRunFrameTimer >= kFoxyRunFrameSeconds)
        {
            mFoxyRunFrameTimer = glm::min(mFoxyRunFrameTimer - kFoxyRunFrameSeconds, kFoxyRunFrameSeconds);
            mFoxyRunFrame++;
        }
        mFoxyRunTimer += deltaTime;
        if (mFoxyRunTimer >= kFoxyRunSeconds)
        {
            LogAnimStats("foxy run");
            AnimPreloadStop();
            FoxyArrive();
        }
        return;
    }

    if (mFoxyStage < 3)
    {
        // Any camera up re-arms his lock-out (#328: 50 + Random(1000) frames), and it counts down
        // a frame at a time (#312).
        if (mTabletUp)
        {
            mFoxyLockTimer = (50 + rand() % 1000) / 60.0f;
        }
        else if (mFoxyLockTimer > 0.0f)
        {
            mFoxyLockTimer -= deltaTime;
        }

        // #190 tests "not watching CAM 1C" before its 5.01 s timer, so only Pirate Cove itself
        // stops the clock; the lock-out is its *last* condition, so a tick that lands during the
        // lock-out is spent and lost rather than saved for later. Pausing the timer instead would
        // hand him a free move after every look at the cameras.
        const bool watchingCove = mTabletUp && mTabletProgress >= 1.0f && (Room)mCameraIndex == Room::PirateCove;
        if (!watchingCove)
        {
            mFoxyMoveTimer += deltaTime;
        }
        if (mFoxyMoveTimer >= kFoxyMoveInterval && !watchingCove)
        {
            mFoxyMoveTimer -= kFoxyMoveInterval;
            const int32_t ai = GetNightActivity(mNight).mFoxy + (mHour >= 3) + (mHour >= 4);
            if ((rand() % 20) + 1 <= ai && mFoxyLockTimer <= 0.0f)
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
        PlaySound("run", false, 2.0f);
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

    // Bonnie and Chica set two counters when they get you (#224, #230): one ticks down 10 frames
    // to the scream (#227, #228) and the other 40 frames to the death screen (#261, #262). So the
    // scream lands about 150 ms after the picture, and the cut comes at a flat 0.67 s however long
    // the animation is. Foxy screams immediately (#322), and Freddy's office attack waits for his
    // animation to reach picture 7 (#408).
    // PORT: the scream's countdown is kept, but not the 40-frame one: the original counts game ticks at
    // 60 fps with its pictures already in memory, while ours are read from the disc and decoded, so
    // a fixed 0.67 s of wall clock cuts the animation off part-way instead of landing on its end.
    // These two therefore still cut when their animation has played out, as the other two do.
    const bool counted = (who == "bonnie" || who == "chica");
    mJumpScreamDelay = counted ? 10.0f / 60.0f : ((who == "freddyoffice") ? 7.0f / 30.0f : -1.0f);
    mJumpCutTime = -1.0f;
    if (mJumpScreamDelay < 0.0f)
    {
        PlaySound("scream");
    }

    SetFade(0.0f);      // the death screens cut, they don't fade
    mState = State::Jumpscare;
    mTabletUp = false;
    mTabletProgress = 0.0f;
    // Bonnie's and Chica's attacks center the view (the original's #229, #230); Foxy's doesn't, the
    // view keeps drifting toward the left door where he lunges in.
    if (who != "foxy")
    {
        mOfficePan = 0.5f;
    }

    // The original's jumpscare animations hide Golden Freddy (#426, #427).
    mYellowBearShown = false;
    mHallucinationVisible = false;
    mRobotVoiceOn = false;
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
    StartAnimPreload("jump_" + who, mCounts["jump_" + who], 1);
    mMenuShown.clear();
    mJump->SetVisible(true);
}

void FnafGame::UpdateJumpscare(float deltaTime)
{
    const int32_t frames = mCounts["jump_" + mJumpWho];
    mJumpTimer += deltaTime;
    if (mJumpWho == "foxy")
    {
        mOfficePan = 0.0f;      // he lunges in at the left door
    }

    // The original's animation speeds at 60 fps: Bonnie 75 (45 fps), Chica 99 (59 fps), Foxy 50
    // (30 fps), Freddy's power-out lunge 60 (36 fps).
    float fps = 24.0f;
    if (mJumpWho == "bonnie")       fps = 45.0f;
    else if (mJumpWho == "chica")   fps = 59.4f;
    else if (mJumpWho == "foxy")    fps = 30.0f;
    else if (mJumpWho == "freddy")  fps = 36.0f;
    else if (mJumpWho == "freddyoffice") fps = 30.0f;   // the office attack's animation speed 50
    // The scream on its own countdown, and the cut to the death screen on another.
    if (mJumpScreamDelay > 0.0f)
    {
        mJumpScreamDelay -= deltaTime;
        if (mJumpScreamDelay <= 0.0f)
        {
            PlaySound("scream");
        }
    }
    if (mJumpCutTime > 0.0f)
    {
        mJumpCutTime -= deltaTime;
        if (mJumpCutTime <= 0.0f)
        {
            EndJumpscare();
            return;
        }
    }

    const float frameSeconds = 1.0f / fps;
    if (mJumpTimer >= frameSeconds && mJumpFrame + 1 < frames)
    {
        // PORT: at most one picture per update. These frames are read from the disc and decoded as
        // they are shown, so a slow one delays the next picture rather than being skipped — every
        // picture is seen, and an animation takes as long as the reads take.
        mJumpTimer = glm::min(mJumpTimer - frameSeconds, frameSeconds);
        mJumpFrame++;
        char name[64];
        snprintf(name, sizeof(name), "jump_%s_%02d", mJumpWho.c_str(), mJumpFrame);
        std::string shown;
        ShowImage(mJumpCanvas, name, shown);
    }
    else if (mJumpFrame + 1 >= frames && mJumpTimer > 0.6f)
    {
        // The ones without a countdown (Foxy, Freddy) cut when their animation has played out.
        EndJumpscare();
    }
}

void FnafGame::EndJumpscare()
{
    // Game over: full-screen static with its sound, then the game over screen.
    LogAnimStats(("jumpscare " + mJumpWho).c_str());
    AnimPreloadStop();
    mJumpScreamDelay = -1.0f;
    mJumpCutTime = -1.0f;
    mJump->SetVisible(false);
    AudioManager::StopAllSounds();      // the scream ends with the animation
    SetFade(0.0f);      // the static runs unfaded; the picture after it does the fading in
    mState = State::GameOver;
    mGameOverTimer = 0.0f;
    // The died screen has the same blip flash as the "what day" one, over its static: eleven
    // pictures, once, then gone.
    mBlipFrame = 0;
    mBlipFrameTimer = 0.0f;
    mGameOverRollTimer = 0.0f;
    mGameOverRare = false;
    mMenuShown.clear();
    ShowMenuWidgets(false, false, false);
    mMenuBlack->SetVisible(true);
    mMenuStaticQuad->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    mMenuStaticQuad->SetVisible(true);
    mJingle.Start("snd/deadstatic.pcm", (uint32_t)mCounts["size_deadstatic"], false, 1.0f);
}

void FnafGame::LoadGoldenSprite()
{
    // His office picture (252x244, 240 KB) is only in RAM while his event can happen: read from the
    // disc when he's armed, freed when the next night starts.
    if (mGoldenSprite.Get() != nullptr)
    {
        return;
    }
    if (!LoadSprite("spr/golden_office.rgx", mGoldenSprite))
    {
        LogError("FNAF1: could not load golden_office (free %u KB)", GetFreeMemoryKb());
        OctLog("FNAF1: could not load golden_office (free %u KB)", GetFreeMemoryKb());
    }
}

void FnafGame::UpdateGoldenFreddy(float deltaTime)
{
    // Golden Freddy, from the original's events (numbers are its event lines). Its "viewing" is 0
    // from the moment the tablet starts down until a camera shows again.
    const bool viewing = mState == State::Playing && mTabletUp && mTabletProgress >= 1.0f;

    // #5: a camera showing hides him.
    if (viewing)
    {
        mYellowBearShown = false;
    }

    // #42, #43: while armed, CAM 2B shows his poster (see GetCameraImage). Seeing it (Bonnie not
    // there) plays the giggle once and puts him in the office. The giggle's channel 27 is at 100.
    if (viewing && (Room)mCameraIndex == Room::WestCorner && !IsAt(mBonnie, Room::WestCorner) && mYellowBear == 1)
    {
        mYellowBear = 2;
        LoadGoldenSprite();     // (a retry, if it couldn't load when he was armed)
        PlaySound("giggle", false, 2.0f);
        OctLog("FNAF1: golden freddy poster seen");
    }

    // #412-#418: the hallucination. Each frame (at 60 fps) rolls Random(10); for 100 frames after it
    // starts, a roll of 1 shows a flash and turns the robot voice's channel 21 up to 100.
    if (mHallucination)
    {
        mHallucinationTime += deltaTime;
        if (mHallucinationTime >= 100.0f / 60.0f)
        {
            mHallucination = false;
            mHallucinationTime = 0.0f;
        }
    }
    // The roll is once per 60 Hz tick, so a 30 fps frame owes two of them; rolling once per frame
    // gives about half the flashes across the 100-frame window.
    mHallucinationStepTimer += deltaTime;
    if (mHallucinationStepTimer >= 1.0f / 60.0f)
    {
        bool rolledOne = false;
        while (mHallucinationStepTimer >= 1.0f / 60.0f)
        {
            mHallucinationStepTimer -= 1.0f / 60.0f;
            rolledOne = rolledOne || (rand() % 10) == 1;
        }
        mHallucinationVisible = mHallucination && rolledOne;
        if (mHallucinationVisible && !mRobotVoiceOn)
        {
            mRobotVoice.Start("snd/robotvoice.pcm", (uint32_t)mCounts["size_robotvoice"], true, 2.0f);
            mRobotVoiceOn = true;
        }
    }
    else if (!mHallucination)
    {
        mHallucinationVisible = false;
    }

    // #381-#384: from night 4, Bonnie on CAM 2B or Chica on CAM 4B drives the voice: every 100 ms
    // its channel is set to 1 + Random(5) x 5 with the cameras down, and to the much louder
    // 1 + Random(5) x 20 while you watch that camera. Channel volume 100 is the mixer's 2.0.
    const bool bonnieGlitching = IsAt(mBonnie, Room::WestCorner);
    const bool chicaGlitching = IsAt(mChica, Room::EastCorner);
    if (mNight >= 4 && (bonnieGlitching || chicaGlitching))
    {
        // Only two cases change it: cameras down (#381, #382), or watching that very camera
        // (#383, #384). On any other camera the volume is left where it was.
        const bool watched = viewing && ((bonnieGlitching && (Room)mCameraIndex == Room::WestCorner) ||
                                         (chicaGlitching && (Room)mCameraIndex == Room::EastCorner));
        mVoiceRollTimer += deltaTime;
        if (mVoiceRollTimer >= 0.1f && (watched || !viewing))
        {
            mVoiceRollTimer -= 0.1f;
            const int32_t channelVolume = 1 + (rand() % 5) * (watched ? 20 : 5);
            if (!mRobotVoiceOn)
            {
                mRobotVoice.Start("snd/robotvoice.pcm", (uint32_t)mCounts["size_robotvoice"], true, 0.0f);
                mRobotVoiceOn = true;
            }
            mRobotVoice.SetVolume(glm::min(2.0f, channelVolume * 0.024f));
        }
    }
    // PORT: the voice is started and stopped. The original starts it looping at the beginning of
    // the night at volume 0 and only ever moves the volume (#14), so it is always playing; a stream
    // here would hold one of a small number of slots all night for something heard once.
    // #380: the voice goes quiet once the hallucination is over and neither of them is there.
    else if (mRobotVoiceOn && !mHallucination && !bonnieGlitching && !chicaGlitching)
    {
        mRobotVoice.Stop();
        mRobotVoiceOn = false;
    }

    // #418: every second, a 1 in 1000 chance of a hallucination, any night.
    mHallucinationRollTimer += deltaTime;
    if (mHallucinationRollTimer >= 1.0f)
    {
        mHallucinationRollTimer -= 1.0f;
        if ((rand() % 1000) == 1)
        {
            mHallucination = true;
            OctLog("FNAF1: hallucination");
        }
    }

    // #419: seen on the poster, he's in the office whenever the cameras are down.
    if (!viewing && mYellowBear == 2)
    {
        mYellowBearShown = true;
    }

    // #420, #421: 300 frames (5 s) in the office with him ends the game, and #420 only counts while
    // he is on screen. #426/#427 hide him while the office is showing Bonnie's or Chica's in-office
    // picture (its animations 35 and 44), which suspends his five seconds — but only while that
    // picture is genuinely the one up. The events that put it there, #116 and #117, want the
    // cameras down, both hall lights off and Freddy not in the room; with the other side's light
    // on, the office shows that lit picture instead and his counter keeps running even though she
    // is standing inside. Keying this off the room alone suspended him for the wider window.
    const bool hiddenByAttack = (IsAt(mBonnie, Room::Office) || IsAt(mChica, Room::Office)) &&
                                !viewing && !mDoors[0].mLight && !mDoors[1].mLight && !mFreddyInOffice;
    if (mYellowBearShown && !hiddenByAttack)
    {
        mYellowBearShownTime += deltaTime;
        if (mYellowBearShownTime >= 300.0f / 60.0f)
        {
            StartCreepyEnd();
            return;
        }
    }

    // #422: raising the cameras after he's appeared makes him leave for good.
    if (mYellowBearShownTime > 0.0f && viewing)
    {
        mYellowBear = 0;
    }

    // #423: showing him starts a hallucination, and that event is ONCE, so a second appearance
    // doesn't start another.
    if (mYellowBearShown && !mYellowBearWasShown && !mYellowBearHallucinated)
    {
        mYellowBearHallucinated = true;
        mHallucination = true;
        OctLog("FNAF1: golden freddy in the office");
    }
    mYellowBearWasShown = mYellowBearShown;

    // #424: every second, a 1 in 100000 chance to arm him — but the event is ONCE, so once it has
    // come up it can't again. Raising the cameras on him (#422) therefore ends him for the night
    // rather than putting him back in the pool.
    if (!mYellowBearArmed)
    {
        mYellowBearRollTimer += deltaTime;
        if (mYellowBearRollTimer >= 1.0f)
        {
            mYellowBearRollTimer -= 1.0f;
            if ((rand() % 100000) == 1)
            {
                mYellowBearArmed = true;
                mYellowBear = 1;
                LoadGoldenSprite();
                OctLog("FNAF1: golden freddy armed");
            }
        }
    }
}

void FnafGame::StartCreepyEnd()
{
    // The original jumps to its "creepy end" frame: his face full screen, all sounds stopped and
    // XSCREAM2 on channel 29. After 1 s it closes the game; here the GameCube resets.
    AudioManager::StopAllSounds();
    StopStreams();
    OctLog("FNAF1: golden freddy creepy end");

    SetFade(0.0f);      // its frame has no transition either
    for (Quad* band : mBlipBands)
    {
        band->SetVisible(false);    // in case this cut in mid-blip on the died screen
    }
    mState = State::CreepyEnd;
    mCreepyEndTimer = 0.0f;
    mTabletUp = false;
    mTabletProgress = 0.0f;
    mYellowBearShown = false;
    mHallucinationVisible = false;
    mRobotVoiceOn = false;
    mJumpWho = "golden";

    std::string shown;
    ShowImage(mJumpCanvas, "golden_end", shown);
    mMenuShown.clear();
    mHallucinationShown.clear();
    mJump->SetVisible(true);
    mJingle.Start("snd/xscream2.pcm", (uint32_t)mCounts["size_xscream2"], false, 2.0f);
}

void FnafGame::UpdateCreepyEnd(float deltaTime)
{
    mCreepyEndTimer += deltaTime;
    if (mCreepyEndTimer < 1.0f)
    {
        return;
    }

#if PLATFORM_DOLPHIN
    // PORT: the original closes the game here. A disc game has nowhere to quit to, so this resets
    // the console instead.
    OctLog("FNAF1: resetting");
    SYS_ResetSystem(SYS_HOTRESET, 0, 0);
#else
    EnterMenu();
#endif
}

// The creepy start's frame runs a 10 s timer, and shows its two pupils at 9.5 s.
static constexpr float kCreepyStartSeconds = 10.0f;
static constexpr float kCreepyStartEyesSeconds = 9.5f;

void FnafGame::StartCreepyStart()
{
    // The 1-in-1000 screen the title rolls for: Bonnie's face on black, in silence (its #1 stops
    // every sound), with a pupil appearing in each socket near the end.
    AudioManager::StopAllSounds();
    StopStreams();
    OctLog("FNAF1: creepy start");

    mState = State::CreepyStart;
    mCreepyStartTimer = 0.0f;
    // Its exit runs the "next day" frame, which adds one to the night counter — so what that
    // counter holds on the way out of the title matters. The title writes it from whichever option
    // is highlighted (#28: New Game writes 1, #29: Continue writes the saved level), and the roll
    // that brings us here runs on that same tick, with the default selection standing.
    mNight = (mMenuSelection == 1) ? mSavedNight : 1;
    ShowMenuWidgets(false, true, false);    // its picture on black, the newspaper's layout
    mMenuShown.clear();
    ShowImage(mJumpCanvas, "creepystart", mMenuShown);
    mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    mMenuBack->SetVisible(true);
    mJump->SetVisible(false);
    SetFade(0.0f);      // that frame carries no transition: it cuts in and cuts out

    // Its two Actives sit at (510, 192) and (804, 196) with a 15,15 hotspot on a 32x32 picture, so
    // their top-left corners are (495, 177) and (789, 181) in the original's 1280x720 screen. Like
    // the night readout's lettering, the picture is stored unscaled and drawn in that space.
    static const float kEyePos[2][2] = { { 495.0f, 177.0f }, { 789.0f, 181.0f } };
    const float sx = mScreenWidth / 1280.0f;
    const float sy = mScreenHeight / 720.0f;
    for (int32_t i = 0; i < 2; ++i)
    {
        mCreepyEyes[i]->SetTexture(mCreepyEyeSprite.Get());
        mCreepyEyes[i]->SetRect(kEyePos[i][0] * sx, kEyePos[i][1] * sy, 32.0f * sx, 32.0f * sy);
        mCreepyEyes[i]->SetVisible(false);
    }
}

void FnafGame::UpdateCreepyStart(float deltaTime)
{
    mCreepyStartTimer += deltaTime;

    // #2: the pupils show at 9.5 s, half a second before the screen ends.
    if (mCreepyStartTimer >= kCreepyStartEyesSeconds)
    {
        for (Quad* eye : mCreepyEyes)
        {
            eye->SetVisible(true);
        }
    }

    if (mCreepyStartTimer >= kCreepyStartSeconds)
    {
        for (Quad* eye : mCreepyEyes)
        {
            eye->SetVisible(false);
        }
        // #0: at ten seconds it goes on to the "next day" frame — the same one 6 AM leads to. So
        // this screen isn't only a scare: it counts the night up and runs straight into that night,
        // without New Game or Continue ever being chosen.
        StartNextDay();
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
        // Once Freddy has left too the stage is empty (#28).
        if (!IsAt(mFreddy, Room::ShowStage)) return "cam1a_empty";
        return (pic <= 10) ? "cam1a_freddy_stare" : "cam1a_freddy";
    case Room::DiningArea:
        // Chica wins when both are there, and both of them win over Freddy (#30-#35).
        if (chica) return (mChica.mPose == 1) ? "cam1b_chica2" : "cam1b_chica";
        if (bonnie) return (mBonnie.mPose == 1) ? "cam1b_bonnie" : "cam1b_bonnie2";
        if (IsAt(mFreddy, Room::DiningArea)) return "cam1b_freddy";
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
        if (IsAt(mFreddy, Room::Restrooms)) return "cam7_freddy";
        return "cam7_empty";
    case Room::Kitchen:      return "";
    case Room::WestHall:
        if (mFoxyRunning)
        {
            char name[24];
            // After the last frame the original's run animation loops its last two frames (the
            // empty hall at the end) until he reaches the door. Those two pictures differ only by
            // compression noise (mean 0.6 of 255), so this holds the last one: flipping between
            // them cost a backwards seek on the SD every frame.
            const int32_t runFrames = glm::max(1, mCounts.at("foxyrun"));
            const int32_t frame = glm::min(mFoxyRunFrame, runFrames - 1);
            snprintf(name, sizeof(name), "foxyrun_%02d", frame);
            return name;
        }
        // Dark, except on the flicker steps when the light catches the hall (and Bonnie).
        if (!mHallLit) return "cam2a_dark";
        return bonnie ? "cam2a_bonnie" : "cam2a_empty";
    case Room::SupplyCloset: return bonnie ? "cam3_bonnie" : "cam3_empty";
    // Rare pictures on empty cameras, by the roll.
    case Room::WestCorner:
        if (bonnie)
        {
            // From night 4 his picture glitches (#44-#47).
            if (mNight >= 4 && mGlitchRoll >= 29) return "cam2b_bonnie_glitch2";
            if (mNight >= 4 && mGlitchRoll >= 25) return "cam2b_bonnie_glitch1";
            return "cam2b_bonnie";
        }
        if (mYellowBear >= 1) return "cam2b_golden";   // his event is armed (or he's already been seen)
        return (pic < 2) ? "cam2b_rare_freddy" : "cam2b_empty";
    case Room::EastHall:
        if (chica) return (mChica.mPose == 1) ? "cam4a_chica" : "cam4a_chica2";
        // The rare pictures are tested first (#73-#75 run before #77), and he shows on 4A only
        // while he's actually there: from the corner (4B) the hall looks empty (#76, #77).
        if (pic == 99) return "cam4a_rare_faces";
        if (pic == 100) return "cam4a_rare_itsme";
        if (IsAt(mFreddy, Room::EastHall)) return "cam4a_freddy";
        return "cam4a_empty";
    case Room::EastCorner:
        if (chica)
        {
            // From night 4 her picture glitches too (#56-#59).
            if (mNight >= 4 && mGlitchRoll >= 29) return "cam4b_chica_glitch2";
            if (mNight >= 4 && mGlitchRoll >= 25) return "cam4b_chica_glitch1";
            return "cam4b_chica";
        }
        if (IsAt(mFreddy, Room::EastCorner)) return "cam4b_freddy";
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

void FnafGame::StartAnimPreload(const std::string& prefix, int32_t count, size_t first)
{
    StartReaderThread();
    std::vector<std::string> paths;
    char name[64];
    for (int32_t i = 0; i < count; ++i)
    {
        snprintf(name, sizeof(name), "img/%s_%02d.jpg", prefix.c_str(), i);
        paths.push_back(name);
    }
    AnimPreloadStart(paths, first);
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
    const bool animation = name.compare(0, 5, "jump_") == 0 || name.compare(0, 8, "foxyrun_") == 0;
    const uint64_t startUs = SYS_GetTimeMicroseconds();
    const std::string path = "img/" + name + ".jpg";
    const bool preloaded = animation && AnimPreloadTake(path, mFrameBuffer);
    const bool read = preloaded || (animation ? ReadAnimationFrame(path, mFrameBuffer)
                                              : ReadDataFile(path, mFrameBuffer));
    const uint64_t readUs = SYS_GetTimeMicroseconds();
    if (read && canvas.Show(mFrameBuffer))
    {
        shown = name;
    }
    const uint64_t endUs = SYS_GetTimeMicroseconds();

    if (animation)
    {
        if (sAnimStats.frames < kAnimStatsMaxFrames)
        {
            // Per frame, kept in memory and logged when the animation ends (logging to the SD here
            // would slow the animation down).
            AnimFrameStat& stat = sAnimStats.perFrame[sAnimStats.frames];
            const size_t underscore = name.rfind('_');
            stat.index = (underscore != std::string::npos) ? atoi(name.c_str() + underscore + 1) : -1;
            stat.preloaded = preloaded;
            stat.readUs = uint32_t(readUs - startUs);
            stat.decodeUs = uint32_t(endUs - readUs);
            stat.sincePreviousUs = sAnimStats.lastShowUs != 0 ? uint32_t(startUs - sAnimStats.lastShowUs) : 0;
        }
        sAnimStats.lastShowUs = startUs;
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
    // PORT: two rolls, not one. The original rolls a single Random(10) every frame and reads it
    // twice — CAM 2A is lit on 3 values in 10, the office light drops out on 1 — so a dropout is
    // always inside a lit frame. Ours rolls them separately, and at 10 Hz for the reason above, so
    // the two are uncorrelated.
    mFlickerTimer -= deltaTime;
    if (mFlickerTimer <= 0.0f)
    {
        mFlickerTimer += 0.1f;
        mHallLit = (rand() % 10) < 3;
        mLightDropout = (rand() % 10) == 0;
    }

    // The original's Random(30) + 1, re-rolled every 50 ms (#387): it picks the glitched pictures
    // of Bonnie on CAM 2B and Chica on CAM 4B from night 4.
    mGlitchTimer -= deltaTime;
    if (mGlitchTimer <= 0.0f)
    {
        mGlitchTimer += 0.05f;
        mGlitchRoll = (rand() % 30) + 1;
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

    const bool night = (mState == State::Playing || mState == State::PowerOut);
    // #426/#427: Bonnie's or Chica's in-office picture hides him while their attack plays out.
    const bool goldenOn = night && mYellowBearShown && mGoldenSprite.Get() != nullptr &&
                          !IsAt(mBonnie, Room::Office) && !IsAt(mChica, Room::Office);
    mGoldenQuad->SetVisible(goldenOn);
    if (goldenOn)
    {
        mGoldenQuad->SetTexture(mGoldenSprite.Get());
        mGoldenQuad->SetRect(kGoldenX * scale - panX, kGoldenY * scale, kGoldenWidth * scale, kGoldenHeight * scale);
    }

    // Hallucination flashes: the original's animation runs at speed 75 (45 fps) through four pictures.
    const bool hallucinationOn = night && mHallucinationVisible;
    mHallucinationQuad->SetVisible(hallucinationOn);
    if (hallucinationOn)
    {
        char name[32];
        snprintf(name, sizeof(name), "hallucination_%d", (int32_t)(mCameraPanTime * 45.0f) % 4);
        ShowImage(mJumpCanvas, name, mHallucinationShown);
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

    UpdateTabletUi(deltaTime, cameraOn);

    // Our camera name, where the original draws its own: just above the map.
    mCameraText->SetVisible(cameraOn);
    if (cameraOn)
    {
        char label[96];
        snprintf(label, sizeof(label), "CAM %s  %s", kCameras[mCameraIndex].mId, kCameras[mCameraIndex].mName);
        mCameraText->SetText(label);
    }

    mAudioOnlyText->SetVisible(cameraOn && room == Room::Kitchen);
    if (mAudioOnlyText->IsVisible())
    {
        mAudioOnlyText->SetText("-CAMERA DISABLED-\nAUDIO ONLY");
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
            mStaticFrameTimer = kStaticFrameSeconds;
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
        if (mJumpWho == "freddy" || mJumpWho == "golden")
        {
            // The power-out jumpscare frames are screen-sized (1280x720), not office-wide.
            mJump->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
        }
        else
        {
            // The frames are office-wide. Bonnie's and Chica's are drawn with the view centered (set
            // in StartJumpscare); Foxy's follow the view as it drifts toward the left door.
            mJump->SetRect(-panX, 0.0f, officeWidth, mScreenHeight);
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

    // The "what day" screen lists its blip flash first, so its bands go under the night's name.
    for (Quad*& band : mBlipBands)
    {
        band = mRoot->CreateChild<Quad>("BlipFlash");
        band->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        band->SetVisible(false);
    }

    // The creepy start's two pupils. They sit on that screen's picture and nothing else is on it,
    // so anywhere above the backdrop will do; here keeps them with the other menu-side quads.
    for (Quad*& eye : mCreepyEyes)
    {
        eye = mRoot->CreateChild<Quad>("CreepyEye");
        eye->SetVisible(false);
    }

    // The glitch bars sit above Freddy's face and the static, below the title and the options —
    // the order the title frame lists its objects in.
    for (Quad*& band : mTitleGlitchBands)
    {
        band = mRoot->CreateChild<Quad>("TitleGlitch");
        band->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        band->SetVisible(false);
    }

    mMenuTitle = mRoot->CreateChild<Quad>("MenuTitle");
    mMenuNewGame = mRoot->CreateChild<Quad>("MenuNewGame");
    mMenuContinue = mRoot->CreateChild<Quad>("MenuContinue");
    mMenuSixth = mRoot->CreateChild<Quad>("MenuSixth");
    for (Quad*& star : mMenuStars)
    {
        star = mRoot->CreateChild<Quad>("MenuStar");
        star->SetVisible(false);
    }
    mMenuNightWord = mRoot->CreateChild<Quad>("MenuNightWord");
    mMenuNightDigit = mRoot->CreateChild<Quad>("MenuNightDigit");
    mMenuArrows = mRoot->CreateChild<Quad>("MenuArrows");
    mMenuCopyright = mRoot->CreateChild<Quad>("MenuCopyright");

    // The scan bar is the last object the title frame lists, so it passes over the text too.
    mTitleScanBar = mRoot->CreateChild<Quad>("TitleScanBar");
    mTitleScanBar->SetVisible(false);
    // The night intro, the 6 AM clock and the game over label are our own text.
    auto makeMenuText = [this](const char* name, float size) -> Text*
    {
        Text* text = mRoot->CreateChild<Text>(name);
        text->SetTextSize(size);
        text->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        text->SetHorizontalJustification(Justification::Center);
        text->SetVisible(false);
        return text;
    };
    mIntroClockText = makeMenuText("IntroClock", 34.0f);
    mIntroNightText = makeMenuText("IntroNight", 34.0f);
    mGameOverLabel = makeMenuText("GameOverLabel", 34.0f);

    // Created last so it covers everything on these screens: the frames' fade transitions are a
    // property of the frame, not something its events do, so they cover the text too.
    mFadeQuad = mRoot->CreateChild<Quad>("Fade");
    mFadeQuad->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
    mFadeQuad->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    mFadeQuad->SetVisible(false);

    // 6 AM screen: only the digit rolls up behind the black masks; "AM" sits beside it.
    mWinFiveText = makeMenuText("WinFive", 40.0f);
    mWinFiveText->SetHorizontalJustification(Justification::Right);
    mWinSixText = makeMenuText("WinSix", 40.0f);
    mWinSixText->SetHorizontalJustification(Justification::Right);
    mWinAmText = makeMenuText("WinAm", 40.0f);
    mWinAmText->SetHorizontalJustification(Justification::Left);
    mWinMaskBottom = mRoot->CreateChild<Quad>("WinMaskBottom");
    mWinMaskTop = mRoot->CreateChild<Quad>("WinMaskTop");
    for (Quad* mask : { mWinMaskBottom, mWinMaskTop })
    {
        mask->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    }

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
    // The "what day" screen has no static of its own — only its blip flash — so the static belongs
    // to the title alone.
    mMenuStaticQuad->SetVisible(menu);
    for (Quad* band : mBlipBands)
    {
        if (band != nullptr && !intro)
        {
            band->SetVisible(false);
        }
    }
    // The pupils belong to the creepy start alone, and that screen places and shows them itself.
    for (Quad* eye : mCreepyEyes)
    {
        if (eye != nullptr)
        {
            eye->SetVisible(false);
        }
    }

    if (mMenuSixth != nullptr)
    {
        mMenuSixth->SetVisible(menu && mBeatGame);
    }

    // A star for each of the two the original tracks separately: beating night 5 lights the first
    // (alongside the 6th night line), beating night 6 the second (#44-#47).
    if (mMenuStars[0] != nullptr)
    {
        mMenuStars[0]->SetVisible(menu && mBeatGame);
        mMenuStars[1]->SetVisible(menu && mBeatSix);
    }

    // The night readout only belongs to the menu, and only while Continue is picked (its #50/#51).
    for (Quad* quad : { mMenuNightWord, mMenuNightDigit })
    {
        if (quad != nullptr && !menu)
        {
            quad->SetVisible(false);
        }
    }

    // The glitch overlay belongs to the title alone: the newspaper and the night intro don't have
    // it, even though they share these widgets.
    for (Quad* band : mTitleGlitchBands)
    {
        if (band != nullptr && !menu)
        {
            band->SetVisible(false);
        }
    }
    if (mTitleScanBar != nullptr)
    {
        mTitleScanBar->SetVisible(menu);
    }

    for (Quad* quad : { mMenuTitle, mMenuNewGame, mMenuContinue, mMenuArrows, mMenuCopyright })
    {
        quad->SetVisible(menu);
    }
    for (Text* text : { mIntroClockText, mIntroNightText })
    {
        text->SetVisible(intro);
    }
    mGameOverLabel->SetVisible(false);      // shown by UpdateGameOver
    mWinFiveText->SetVisible(false);        // shown by StartWin
    mWinSixText->SetVisible(false);
    mWinAmText->SetVisible(false);
    for (Quad* quad : { mWinMaskTop, mWinMaskBottom })
    {
        quad->SetVisible(false);
    }
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

    SetFade(0.0f);      // the title has no fade of its own
    mState = State::Menu;
    // Continue is picked for you once there's progress to continue (#42/#43).
    mMenuSelection = (mSavedNight > 1) ? 1 : 0;
    mMenuTimer = 0.0f;
    mMenuFrameTimer = 0.0f;
    mTitleGlitchFrame = 0;
    mTitleGlitchFrameTimer = 0.0f;
    mTitleGlitchRollTimer = 0.0f;
    mTitleGlitchAlphaTimer = 0.0f;
    mTitleGlitchOn = false;
    mTitleScanTime = 0.0f;
    mMenuShown.clear();     // the jumpscare canvas may hold something else now
    mJump->SetVisible(false);
    ShowMessage("");

    PlaceSprite(mMenuTitle, mMenuTitleSprite, 175.0f, 80.0f);
    PlaceSprite(mMenuNewGame, mMenuNewGameSprite, 175.0f, 400.0f);
    PlaceSprite(mMenuContinue, mMenuContinueSprite, 175.0f, 470.0f);
    LoadProgress();
    // Its object sits at (285, 571) with a (113, 22) hotspot, so its top-left is (172, 549) —
    // the same left edge as New Game and Continue.
    PlaceSprite(mMenuSixth, mMenuSixthSprite, 172.0f, 549.0f);
    // Their objects sit at (200, 338) and (277, 338) with a (28, 27) hotspot.
    PlaceSprite(mMenuStars[0], mMenuStarSprite, 172.0f, 311.0f);
    PlaceSprite(mMenuStars[1], mMenuStarSprite, 249.0f, 311.0f);
    PlaceNightReadout();
    PlaceSprite(mMenuCopyright, mMenuCopyrightSprite, 1260.0f - mMenuCopyrightSprite.mWidth * 2.0f, 690.0f);
    ShowMenuWidgets(true, false, false);

    // #61: the title rolls Random(1000) on the frame it opens, and a 1 goes to the creepy start
    // instead. Rolled here, so it can come up on any arrival at the title — including the one the
    // game over screen makes after its ten seconds, which is where it is usually seen. Rolled
    // before the music starts: that screen silences everything anyway, so starting the two streams
    // first would only cost a disc read. It never comes back here — its own exit runs into a night.
    if ((rand() % 1000) == 1)
    {
        StartCreepyStart();
        return;
    }

    mMenuMusic.Start("snd/menumusic.pcm", (uint32_t)mCounts["size_menumusic"], true, 0.8f);
    // PORT: the original plays its title static once and lets it end, leaving the music underneath.
    // Ours loops (and the converter crossfades the file for it), because that was liked better.
    mMenuHum.Start("snd/menustatic.pcm", (uint32_t)mCounts["size_menustatic"], true, 0.4f);
    OctLog("FNAF1: main menu");
}

void FnafGame::StartNightIntro()
{
    StopStreams();
    SetFade(0.0f);      // it cuts in; only its fade out is a transition
    mState = State::NightIntro;
    mMenuTimer = 0.0f;

    // "12:00 AM" centred, the night's name centred below it.
    static const char* kNightNames[] = { "1st Night", "2nd Night", "3rd Night", "4th Night", "5th Night", "6th Night" };
    mIntroClockText->SetRect(0.0f, mScreenHeight * 0.40f, mScreenWidth, 50.0f);
    mIntroClockText->SetText("12:00 AM");
    mIntroNightText->SetRect(0.0f, mScreenHeight * 0.50f, mScreenWidth, 50.0f);
    mIntroNightText->SetText(kNightNames[glm::clamp(mNight, 1, 6) - 1]);
    mBlipFrame = 0;
    mBlipFrameTimer = 0.0f;
    ShowMenuWidgets(false, false, true);
    PlaySound("blip");
}

// The title's glitch overlay: eight pictures of white bars across the whole screen, played as a
// looping animation at speed 10 (6 fps). Rows are in the original's 720-line screen; a band of
// (0, 0) means that frame has fewer than three.
static constexpr int32_t kTitleGlitchFrames = 8;
static constexpr float kTitleGlitchFps = 6.0f;
// The scan bar's path: 768 px at 0.625 px per frame (37.5 px/s at 60 fps), then back to the top.
static constexpr float kTitleScanSpeed = 37.5f;
static constexpr float kTitleScanTravel = 768.0f;
static constexpr float kTitleScanAlpha = 0.16f;
static const float kTitleGlitchBands[kTitleGlitchFrames][3][2] = {
    { { 182.0f, 218.0f }, {   0.0f,   0.0f }, {   0.0f,   0.0f } },
    { { 469.0f, 494.0f }, {   0.0f,   0.0f }, {   0.0f,   0.0f } },
    { {  35.0f,  63.0f }, { 102.0f, 108.0f }, { 467.0f, 539.0f } },
    { { 206.0f, 320.0f }, { 350.0f, 362.0f }, {   0.0f,   0.0f } },
    { {  18.0f,  33.0f }, { 194.0f, 206.0f }, { 568.0f, 589.0f } },
    { { 425.0f, 438.0f }, {   0.0f,   0.0f }, {   0.0f,   0.0f } },
    { { 192.0f, 237.0f }, { 271.0f, 280.0f }, {   0.0f,   0.0f } },
    { { 433.0f, 517.0f }, {   0.0f,   0.0f }, {   0.0f,   0.0f } },
};

// The "what day" screen's blip flash: eleven pictures of white bands at speed 75 (45 fps), played
// once — about a quarter of a second — and then gone. Its event on "animation finished" stops the
// animation rather than restarting it, the same action the power-out death's frame uses on Freddy's
// face; running it as a loop strobes the whole screen for two seconds, which the original doesn't.
// Rows are in the original's 720-line screen, and its first three pictures are the whole screen
// white, which is what makes the blip land hard. (0, 0) means that frame has only one band.
static constexpr int32_t kBlipFrames = 11;
static constexpr float kBlipFps = 45.0f;
static constexpr float kBlipAlpha = 1.0f;
static const float kBlipBandRows[kBlipFrames][2][2] = {
    { {   0.0f, 720.0f }, {   0.0f,   0.0f } },
    { {   0.0f, 720.0f }, {   0.0f,   0.0f } },
    { {   0.0f, 720.0f }, {   0.0f,   0.0f } },
    { { 154.0f, 434.0f }, { 436.0f, 521.0f } },
    { {  98.0f, 378.0f }, { 569.0f, 653.0f } },
    { { 118.0f, 203.0f }, { 241.0f, 521.0f } },
    { {   0.0f, 120.0f }, { 497.0f, 582.0f } },
    { { 258.0f, 343.0f }, { 379.0f, 659.0f } },
    { { 529.0f, 613.0f }, {   0.0f,   0.0f } },
    { { 103.0f, 187.0f }, {   0.0f,   0.0f } },
    { { 433.0f, 517.0f }, {   0.0f,   0.0f } },
};

void FnafGame::UpdateBlipFlash(float deltaTime)
{
    // Once it has played out it stays gone for the rest of the screen.
    if (mBlipFrame >= kBlipFrames)
    {
        for (Quad* band : mBlipBands)
        {
            band->SetVisible(false);
        }
        return;
    }

    mBlipFrameTimer += deltaTime;
    if (mBlipFrameTimer >= 1.0f / kBlipFps)
    {
        mBlipFrameTimer -= 1.0f / kBlipFps;
        mBlipFrame++;
        if (mBlipFrame >= kBlipFrames)
        {
            return;     // hidden on the next update
        }
    }

    const float scaleY = mScreenHeight / 720.0f;
    for (int32_t b = 0; b < 2; ++b)
    {
        const float top = kBlipBandRows[mBlipFrame][b][0];
        const float bottom = kBlipBandRows[mBlipFrame][b][1];
        mBlipBands[b]->SetVisible(bottom > top);
        if (bottom > top)
        {
            mBlipBands[b]->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, kBlipAlpha));
            mBlipBands[b]->SetRect(0.0f, top * scaleY, mScreenWidth, (bottom - top) * scaleY);
        }
    }
}

void FnafGame::UpdateTitleGlitch(float deltaTime)
{
    // Every 300 ms it rolls Random(3) and only shows itself on a 1, and every 80 ms its
    // transparency is re-rolled to 100 + Random(100) of 255.
    mTitleGlitchRollTimer += deltaTime;
    if (mTitleGlitchRollTimer >= 0.3f)
    {
        mTitleGlitchRollTimer -= 0.3f;
        mTitleGlitchOn = (rand() % 3) == 1;
    }

    mTitleGlitchAlphaTimer += deltaTime;
    if (mTitleGlitchAlphaTimer >= 0.08f)
    {
        mTitleGlitchAlphaTimer -= 0.08f;
        // 100 + Random(100) is how see-through it is, not how solid: 0 would be opaque and 255
        // invisible, so the bars sit between 22% and 61% and never read as solid white.
        mTitleGlitchAlpha = 1.0f - (100.0f + (float)(rand() % 100)) / 255.0f;
    }

    mTitleGlitchFrameTimer += deltaTime;
    if (mTitleGlitchFrameTimer >= 1.0f / kTitleGlitchFps)
    {
        mTitleGlitchFrameTimer -= 1.0f / kTitleGlitchFps;
        mTitleGlitchFrame = (mTitleGlitchFrame + 1) % kTitleGlitchFrames;
    }

    const float scaleY = mScreenHeight / 720.0f;

    // The scan bar: a 1328x32 white band starting just off the top-left at (-19, -38), walking
    // 768 px down its path at 0.625 px per frame and jumping back to the top, so a pass takes
    // about 20.5 s. (Its picture is solid white; how see-through the object itself is isn't in
    // the events, so this alpha is set by eye.)
    const float scaleX = mScreenWidth / 1280.0f;
    mTitleScanTime = fmod(mTitleScanTime + deltaTime * kTitleScanSpeed, kTitleScanTravel);
    mTitleScanBar->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, kTitleScanAlpha));
    mTitleScanBar->SetRect(-19.0f * scaleX, (-38.0f + mTitleScanTime) * scaleY,
                           1328.0f * scaleX, 32.0f * scaleY);

    for (int32_t b = 0; b < 3; ++b)
    {
        const float top = kTitleGlitchBands[mTitleGlitchFrame][b][0];
        const float bottom = kTitleGlitchBands[mTitleGlitchFrame][b][1];
        const bool on = mTitleGlitchOn && bottom > top;
        mTitleGlitchBands[b]->SetVisible(on);
        if (on)
        {
            mTitleGlitchBands[b]->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, mTitleGlitchAlpha));
            mTitleGlitchBands[b]->SetRect(0.0f, top * scaleY, mScreenWidth, (bottom - top) * scaleY);
        }
    }
}

void FnafGame::PlaceNightReadout()
{
    // Drawn at their own size in the original's 1280x720 space, so no scaling touches the pixels.
    const float sx = mScreenWidth / 1280.0f;
    const float sy = mScreenHeight / 720.0f;

    mMenuNightWord->SetTexture(mMenuNightWordSprite.Get());
    mMenuNightWord->SetRect(kMenuNightWordX * sx, kMenuNightWordY * sy,
                            kMenuNightWordW * sx, kMenuNightWordH * sy);

    const Sprite& digit = mMenuDigitSprites[glm::clamp(mSavedNight, 0, 9)];
    mMenuNightDigit->SetTexture(digit.Get());
    mMenuNightDigit->SetRect(kMenuNightDigitX * sx, kMenuNightDigitY * sy,
                             kMenuNightDigitW * sx, kMenuNightDigitH * sy);
}

// The frames' own fade transitions, in milliseconds, read from each frame's transition chunks
// rather than its events: the help-wanted ad fades in and out over 2 s, the "what day" screen cuts
// in and fades out over 1010 ms, the game over screen fades in over 1010 ms, and the three ending
// screens fade both ways over 2 s. (The night, the title and the death screens have none, so they
// cut.) The 6 AM screen's 1010/900 pair is the same data, and matches what was worked out for it
// by eye earlier.
static constexpr float kAdFadeSeconds = 2.0f;
static constexpr float kIntroFadeOutSeconds = 1.01f;
static constexpr float kGameOverFadeInSeconds = 1.01f;
static constexpr float kEndingFadeSeconds = 2.0f;

void FnafGame::SetFade(float blackAmount)
{
    const float amount = glm::clamp(blackAmount, 0.0f, 1.0f);
    mFadeQuad->SetVisible(amount > 0.0f);
    if (amount > 0.0f)
    {
        mFadeQuad->SetRect(0.0f, 0.0f, mScreenWidth, mScreenHeight);
        mFadeQuad->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, amount));
    }
}

void FnafGame::UpdateMenu(float deltaTime)
{
    mMenuTimer += deltaTime;

    // Static over the menu and the night intro, animated like the cameras'.
    mStaticFrameTimer -= deltaTime;
    if (mStaticFrameTimer <= 0.0f)
    {
        mStaticFrameTimer = kStaticFrameSeconds;
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
            // Random(100) every 80 ms, and only three of its hundred values glitch the face: 97
            // twitches one way, 98 the other, 99 shows the endoskeleton. Everything else is the
            // normal picture, so each glitch is 1 in 100, not the 3/2/1 we had.
            const int32_t roll = rand() % 100;
            const int32_t frame = (roll == 99) ? 3 : (roll == 98) ? 2 : (roll == 97) ? 1 : 0;
            char name[32];
            snprintf(name, sizeof(name), "menu_freddy%d", frame);
            ShowImage(mJumpCanvas, name, mMenuShown);
            mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 0.55f + (rand() % 46) / 100.0f));
            mMenuStaticQuad->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 0.15f + (rand() % 20) / 100.0f));
        }

        UpdateTitleGlitch(deltaTime);

        // "Night N" appears next to Continue while it's the highlighted option.
        const bool showNight = (mMenuSelection == 1);
        mMenuNightWord->SetVisible(showNight);
        mMenuNightDigit->SetVisible(showNight);
        if (showNight)
        {
            PlaceNightReadout();
        }

        // New Game, Continue, and the 6th night once night 5 has been beaten (the original's
        // menu grows the same way).
        const int32_t options = mBeatGame ? 3 : 2;
        if (Pressed(GAMEPAD_DOWN))
        {
            mMenuSelection = (mMenuSelection + 1) % options;
            PlaySound("blip");
        }
        else if (Pressed(GAMEPAD_UP))
        {
            mMenuSelection = (mMenuSelection + options - 1) % options;
            PlaySound("blip");
        }
        static const float kArrowY[] = { 402.0f, 474.0f, 553.0f };
        PlaceSprite(mMenuArrows, mMenuArrowsSprite, 95.0f, kArrowY[glm::clamp(mMenuSelection, 0, 2)]);

        // PORT: this acts on the press. The original counts 20 frames first (#36-#40, about a third
        // of a second) before it leaves the menu, and with the next frame's own fade following it
        // that reads as a lag on every selection.
        if (Pressed(GAMEPAD_A) || Pressed(GAMEPAD_START))
        {
            const int32_t chosen = mMenuSelection;
            PlaySound("blip");
            if (chosen == 0)
            {
                // New Game: back to night 1 (the original rewrites its saved level), after the
                // help-wanted ad. Its frame touches no sound at all, so the title's music carries
                // on underneath it.
                mNight = 1;
                mSavedNight = 1;
                mMenuTimer = 0.0f;
                for (bool& played : mCallPlayed)
                {
                    played = false;     // New Game clears the original's "play voice" counters
                }
                // (New Game rewrites the saved level but leaves both stars alone: its #23 writes
                // "level" only, and #48's hold-to-wipe is what clears "beatgame" and "beat6".)
                SaveProgress();
                mState = State::Newspaper;
                ShowImage(mJumpCanvas, "newspaper", mMenuShown);
                // The picture itself is drawn at full strength; the fading is the frame's own
                // transition, done with the black overlay below.
                mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                ShowMenuWidgets(false, true, false);
                SetFade(1.0f);  // black from this frame: the fade starts here, not on the next update
            }
            else
            {
                mNight = (chosen == 2) ? 6 : mSavedNight;
                StartNightIntro();
            }
        }
        break;
    }

    case State::Newspaper:
    {
        // Fades up over 2 s, sits for its five seconds, then fades out over 2 s. A or START skips
        // ahead to the fade rather than cutting, which is what leaving the frame early does.
        const float holdEnd = kAdFadeSeconds + kNewspaperSeconds;
        if (mMenuTimer < kAdFadeSeconds)
        {
            SetFade(1.0f - mMenuTimer / kAdFadeSeconds);
        }
        else if (mMenuTimer < holdEnd)
        {
            SetFade(0.0f);
            if (Pressed(GAMEPAD_A) || Pressed(GAMEPAD_START))
            {
                mMenuTimer = holdEnd;
            }
        }
        else
        {
            SetFade((mMenuTimer - holdEnd) / kAdFadeSeconds);
            if (mMenuTimer >= holdEnd + kAdFadeSeconds)
            {
                SetFade(0.0f);
                StartNightIntro();
            }
        }
        break;
    }

    case State::NightIntro:
        // Cuts in, and fades out over its last 1010 ms into the night.
        UpdateBlipFlash(deltaTime);
        SetFade(glm::max(0.0f, (mMenuTimer - kNightIntroSeconds) / kIntroFadeOutSeconds));
        if (mMenuTimer >= kNightIntroSeconds + kIntroFadeOutSeconds)
        {
            SetFade(0.0f);
            StartNight();
        }
        break;

    default:
        break;
    }
}

// Progress, as the original keeps it in its .ini: the night Continue starts ("level", never past
// 6) and whether night 5 has been beaten ("beatgame", which offers the 6th night). It's a small
// file next to the game on the SD; on a disc, or in Dolphin without one, saving just fails and
// every run starts fresh.
static const char* kSavePaths[] = { "/FNAF1.sav", "FNAF1/FNAF1.sav", "FNAF1.sav" };

void FnafGame::LoadProgress()
{
    void OctLockFileIo();
    void OctUnlockFileIo();

    for (const char* path : kSavePaths)
    {
        OctLockFileIo();
        FILE* file = fopen(path, "rb");
        int32_t night = 0;
        int32_t beat = 0;
        int32_t beatSix = 0;
        // A file from before the second star was tracked has two numbers; treat its missing third
        // as zero rather than refusing the whole save.
        const int32_t fields = (file != nullptr) ? fscanf(file, "%d %d %d", &night, &beat, &beatSix) : 0;
        const bool read = fields >= 2;
        if (file != nullptr)
        {
            fclose(file);
        }
        OctUnlockFileIo();

        if (read)
        {
            // The original clamps Continue to night 5 (its title frame's #57); the 6th night is
            // only reachable from its own menu entry.
            mSavedNight = glm::clamp(night, 1, 5);
            mBeatGame = beat != 0;
            mBeatSix = beatSix != 0;
            OctLog("FNAF1: progress from %s: night %d, beaten %d, six %d", path, mSavedNight,
                   (int)mBeatGame, (int)mBeatSix);
            return;
        }
    }
}

void FnafGame::SaveProgress()
{
    void OctLockFileIo();
    void OctUnlockFileIo();

    // The original only writes the level while it's going up and still under 6 (#339), so a
    // replay of an earlier night never sets you back.
    if (mNight > mSavedNight && mNight < 6)
    {
        mSavedNight = mNight;
    }
    mBeatGame = mBeatGame || mNight >= 6;    // its "beatgame", written by the night 5 ending
    mBeatSix = mBeatSix || mNight >= 7;      // its "beat6", written when night 6 is finished

    for (const char* path : kSavePaths)
    {
        OctLockFileIo();
        FILE* file = fopen(path, "wb");
        const bool wrote = file != nullptr && fprintf(file, "%d %d %d\n", mSavedNight,
                                                      mBeatGame ? 1 : 0, mBeatSix ? 1 : 0) > 0;
        if (file != nullptr)
        {
            fclose(file);
        }
        OctUnlockFileIo();

        if (wrote)
        {
            OctLog("FNAF1: progress saved to %s: night %d, beaten %d, six %d", path, mSavedNight,
                   (int)mBeatGame, (int)mBeatSix);
            return;
        }
    }
    OctLog("FNAF1: progress could not be saved");
}

// The original's ending frames: the picture, the music box, and 15 s before it goes back to the
// title (any button skips it here).
static constexpr float kEndingSeconds = 15.0f;

void FnafGame::StartEnding(const char* image)
{
    AudioManager::StopAllSounds();
    StopStreams();
    mState = State::Ending;
    mEndingTimer = 0.0f;
    mEndingImage = image;
    OctLog("FNAF1: ending %s (night %d)", image, mNight);

    ShowMessage("");
    ShowMenuWidgets(false, true, false);    // the newspaper's layout: the picture on black
    mMenuShown.clear();
    ShowImage(mJumpCanvas, mEndingImage, mMenuShown);
    mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    mMenuBack->SetVisible(true);
    SetFade(1.0f);  // likewise: its fade in starts on the frame the screen appears
    mMusicBox.Start("snd/musicbox.pcm", (uint32_t)mCounts["size_musicbox"], false, 1.0f);
}

void FnafGame::UpdateEnding(float deltaTime)
{
    mEndingTimer += deltaTime;

    // The ending frames fade both ways over 2 s.
    if (mEndingTimer < kEndingFadeSeconds)
    {
        SetFade(1.0f - mEndingTimer / kEndingFadeSeconds);
    }
    else if (mEndingTimer > kEndingSeconds - kEndingFadeSeconds)
    {
        SetFade((mEndingTimer - (kEndingSeconds - kEndingFadeSeconds)) / kEndingFadeSeconds);
    }
    else
    {
        SetFade(0.0f);
        if (Pressed(GAMEPAD_A) || Pressed(GAMEPAD_START))
        {
            mEndingTimer = kEndingSeconds - kEndingFadeSeconds;  // skip ahead to the fade out
        }
    }

    if (mEndingTimer >= kEndingSeconds)
    {
        SetFade(0.0f);
        EnterMenu();
    }
}

void FnafGame::UpdateGameOver(float deltaTime)
{
    mGameOverTimer += deltaTime;

    // Static animates the whole time.
    mStaticFrameTimer -= deltaTime;
    if (mStaticFrameTimer <= 0.0f)
    {
        mStaticFrameTimer = kStaticFrameSeconds;
        mStaticFrame = (mStaticFrame + 1) % glm::max(1, mCounts["static"]);
        char name[32];
        snprintf(name, sizeof(name), "static_%02d", mStaticFrame);
        std::string unused;
        ShowImage(mStaticCanvas, name, unused);
    }

    if (mGameOverTimer < kGameOverStaticSeconds)
    {
        UpdateBlipFlash(deltaTime);     // its bands draw over the static, as that frame orders them
        return;
    }

    if (!mGameOverLabel->IsVisible())
    {
        // The game over screen: Freddy in the backstage room, "Game Over" in the corner.
        mJingle.Stop();
        AudioManager::StopAllSounds();
        ShowImage(mJumpCanvas, "gameover", mMenuShown);
        mMenuBack->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        mMenuBack->SetVisible(true);
        mMenuStaticQuad->SetVisible(false);     // a clean cut: no static, no sound
        mGameOverLabel->SetRect(mScreenWidth * 0.45f, mScreenHeight * 0.86f, mScreenWidth * 0.5f, 50.0f);
        mGameOverLabel->SetText("Game Over");
        mGameOverLabel->SetVisible(true);
    }

    // The game over screen rolls Random(10000) + 1 every second (its frame's #4); on a 1 it goes
    // to the "creepy end" instead of the title (#2).
    mGameOverRollTimer += deltaTime;
    if (mGameOverRollTimer >= 1.0f)
    {
        mGameOverRollTimer -= 1.0f;
        // Each roll replaces the last: its frame tests the counter once, at the 10 s mark, so only
        // the value standing then counts. Keeping any earlier 1 would make this ten times likelier.
        mGameOverRare = (rand() % 10000) == 1;
    }

    // Its frame has no way past it: only the 10 s timer and the rare roll above, so a button press
    // does nothing here.
    const float shown = mGameOverTimer - kGameOverStaticSeconds;

    // Its frame fades in over 1010 ms; there's no fade out, it cuts to the title.
    SetFade(glm::max(0.0f, 1.0f - shown / kGameOverFadeInSeconds));

    if (shown >= kGameOverSeconds)
    {
        if (mGameOverRare)
        {
            OctLog("FNAF1: rare game over");
            StartCreepyEnd();
            return;
        }
        EnterMenu();
    }
}

// The original's "next day" frame (1280x720, black). Positions are its object positions minus
// their hotspots: the digits' hotspot is (-5, 0), the masks' (74, 55).
static constexpr float kWinFiveX = 549.0f;
static constexpr float kWinFiveY = 298.0f;
static constexpr float kWinSixDx = 4.0f;        // the 6 is placed at the 5 + (4, 110) every frame
static constexpr float kWinSixDy = 110.0f;
static constexpr float kWinScroll = 112.0f;     // the 5's path: straight up 112 px
static constexpr float kWinScrollSpeed = 22.5f; // path speed 3: 0.375 px per frame at 60 fps
static constexpr float kWinCheerSeconds = 201.0f / 60.0f;  // after the 5 stops, "> 200" frames to the next night
// The frame's transitions: a 1010 ms fade in from black and a 900 ms fade out. Clickteam runs the
// start-of-frame events (stop sounds, chimes) before the fade in and pauses the rest during both.
static constexpr float kWinFadeInSeconds = 1.01f;
static constexpr float kWinFadeOutSeconds = 0.9f;

// The original's "next day" frame: it counts the night up and picks where to go next (its #5-#10).
// Nights 1-4 run into the next one, finishing night 5 pays overtime, and finishing night 6 gets you
// fired. Its third ending, the $120.00 cheque, belongs to the custom night, which this port doesn't
// have. Two screens lead here: 6 AM, and the creepy start.
void FnafGame::StartNextDay()
{
    mNight += 1;
    SaveProgress();
    if (mNight == 6)
    {
        StartEnding("paycheck_overtime");   // $120.50 with overtime
    }
    else if (mNight > 6)
    {
        StartEnding("fired");               // the notice of termination
    }
    else
    {
        StartNightIntro();
    }
}

void FnafGame::StartWin()
{
    // Frame start: all sounds stop and the chimes play.
    AudioManager::StopAllSounds();
    StopStreams();
    mJingle.Start("snd/chimes.pcm", (uint32_t)mCounts["size_chimes"], false, 1.0f);
    SetFade(1.0f);      // black from this frame: its 1010 ms fade in starts here
    mState = State::Win;
    mTabletUp = false;
    mTabletProgress = 0.0f;
    mWinPhase = 0;
    mWinFadeTime = 0.0f;
    mWinTimer = 0.0f;
    mWinCheered = false;
    mWinCheerTime = 0.0f;
    OctLog("FNAF1: 6 AM");

    ShowMessage("");
    ShowMenuWidgets(false, false, false);
    mMenuBlack->SetVisible(true);
    // The masks hide the clock above and below the slot it rolls through. Our text is centred
    // across the screen, so they span its whole width.
    const float scaleY = mScreenHeight / 720.0f;
    mWinMaskTop->SetRect(0.0f, 169.0f * scaleY, mScreenWidth, 118.0f * scaleY);
    mWinMaskBottom->SetRect(0.0f, 385.0f * scaleY, mScreenWidth, 118.0f * scaleY);
    // The original's layout: the digit's right edge at x 602, "AM" from x 645, both on its
    // 1280x720 screen.
    mWinFiveText->SetText("5");
    mWinSixText->SetText("6");
    mWinAmText->SetText("AM");
    // (Closer than the original's 645: our font is narrower than its pixel digits, so its gap
    // looked too wide.)
    mWinAmText->SetRect(618.0f * mScreenWidth / 1280.0f, 296.0f * scaleY, mScreenWidth * 0.3f, 60.0f);
    mWinFiveText->SetVisible(true);
    mWinSixText->SetVisible(true);
    mWinAmText->SetVisible(true);
    mWinMaskTop->SetVisible(true);
    mWinMaskBottom->SetVisible(true);
    UpdateWin(0.0f);
}

void FnafGame::UpdateWin(float deltaTime)
{
    float alpha = 1.0f;
    switch (mWinPhase)
    {
    case 0:     // fading in from black
        mWinFadeTime += deltaTime;
        alpha = glm::clamp(mWinFadeTime / kWinFadeInSeconds, 0.0f, 1.0f);
        if (mWinFadeTime >= kWinFadeInSeconds)
        {
            mWinPhase = 1;
        }
        break;

    case 1:
    {
        mWinTimer += deltaTime;
        const float moved = glm::min(kWinScroll, floor(mWinTimer * kWinScrollSpeed));

        // The 5 stopped: the kids cheer (once), then after the countdown the frame fades out.
        if (!mWinCheered && moved >= kWinScroll)
        {
            mWinCheered = true;
            mCheer.Start("snd/cheer.pcm", (uint32_t)mCounts["size_cheer"], false, 0.9f);
        }
        if (mWinCheered)
        {
            mWinCheerTime += deltaTime;
            if (mWinCheerTime >= kWinCheerSeconds)
            {
                mWinPhase = 2;
                mWinFadeTime = 0.0f;
            }
        }
        break;
    }

    default:    // fading out to black; the sounds keep playing until the next frame stops them
        mWinFadeTime += deltaTime;
        alpha = 1.0f - glm::clamp(mWinFadeTime / kWinFadeOutSeconds, 0.0f, 1.0f);
        if (mWinFadeTime >= kWinFadeOutSeconds)
        {
            StartNextDay();
            return;
        }
        break;
    }

    // The path moves in whole pixels.
    const float moved = glm::min(kWinScroll, floor(mWinTimer * kWinScrollSpeed));
    const float scaleY = mScreenHeight / 720.0f;
    const float digitRight = 602.0f * mScreenWidth / 1280.0f;
    mWinFiveText->SetRect(0.0f, (kWinFiveY - moved) * scaleY, digitRight, 60.0f);
    mWinSixText->SetRect(0.0f, (kWinFiveY - moved + kWinSixDy) * scaleY, digitRight, 60.0f);
    // Its fade is the frame's own transition (1010 ms in, 900 ms out, the same pair the transition
    // chunk holds), so it goes through the black overlay like every other screen rather than being
    // applied to these three widgets — which left the masks and everything else unfaded.
    for (Text* text : { mWinFiveText, mWinSixText, mWinAmText })
    {
        text->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
    SetFade(1.0f - alpha);
}

// The tablet's map overlay, from the original's objects: the floor plan ("Active 9") at (848, 313),
// 400x400 on its 1280x720 screen, blinking between two pictures; a 60x40 button per camera (green
// while you watch it, blinking, grey otherwise) and its name beside it. Positions below are the
// objects' top-left corners in the original's screen.
struct MapCamera
{
    float mButtonX, mButtonY;
    float mLabelX, mLabelY;
};
static const MapCamera kMapCameras[kNumCameras] = {
    { 954.0f, 334.0f, 962.0f, 341.0f },     // 1A Show Stage
    { 934.0f, 390.0f, 940.0f, 397.0f },     // 1B Dining Area
    { 902.0f, 468.0f, 909.0f, 475.0f },     // 1C Pirate Cove
    { 828.0f, 417.0f, 835.0f, 424.0f },     // 5 Backstage
    { 1166.0f, 418.0f, 1173.0f, 424.0f },   // 7 Restrooms
    { 1157.0f, 549.0f, 1164.0f, 556.0f },   // 6 Kitchen
    { 954.0f, 584.0f, 961.0f, 590.0f },     // 2A W. Hall
    { 870.0f, 566.0f, 878.0f, 574.0f },     // 3 Supply Closet
    { 954.0f, 624.0f, 961.0f, 630.0f },     // 2B W. Hall Corner
    { 1060.0f, 585.0f, 1067.0f, 592.0f },   // 4A E. Hall
    { 1060.0f, 625.0f, 1067.0f, 632.0f },   // 4B E. Hall Corner
};
static constexpr float kMapX = 848.0f;
static constexpr float kMapY = 313.0f;
static constexpr float kMapSize = 400.0f;
static constexpr float kMapBlinkSeconds = 1.0f / 1.2f;      // animation speed 2 at 60 fps
static constexpr float kMapButtonBlinkSeconds = 1.0f / 1.8f; // animation speed 3

// The camera-switch flash ("Active 5"): 9 full-width white bands at speed 70, over the tablet. Rows
// are in the original's 720-line screen; (0, 0) means no band that frame.
static constexpr float kFlashFps = 42.0f;
static constexpr int32_t kFlashFrames = 9;
static const float kFlashBands[kFlashFrames][2][2] = {
    { { 0.0f, 720.0f }, { 0.0f, 0.0f } },
    { { 155.0f, 434.0f }, { 437.0f, 520.0f } },
    { { 98.0f, 377.0f }, { 569.0f, 653.0f } },
    { { 119.0f, 202.0f }, { 241.0f, 520.0f } },
    { { 0.0f, 120.0f }, { 498.0f, 581.0f } },
    { { 259.0f, 342.0f }, { 380.0f, 659.0f } },
    { { 529.0f, 613.0f }, { 0.0f, 0.0f } },
    { { 103.0f, 187.0f }, { 0.0f, 0.0f } },
    { { 433.0f, 517.0f }, { 0.0f, 0.0f } },
};

void FnafGame::StartCameraFlash()
{
    // In the original the flash and the blip are one thing: #15 creates the white bands and plays
    // blip3 together, for a camera switch (#17), the first camera of a raise, and someone moving
    // on the camera you're watching (#195).
    mFlashTime = 0.0f;
    PlaySound("blip");
}

void FnafGame::UpdateTabletUi(float deltaTime, bool cameraOn)
{
    const float scaleX = mScreenWidth / 1280.0f;
    const float scaleY = mScreenHeight / 720.0f;

    mMap->SetVisible(cameraOn && mMapPlainSprite.Get() != nullptr);
    if (mMap->IsVisible())
    {
        mMapBlinkTime += deltaTime;
        const bool cones = (int32_t)(mMapBlinkTime / kMapBlinkSeconds) % 2 != 0;
        const Sprite& sprite = (cones && mMapConesSprite.Get() != nullptr) ? mMapConesSprite : mMapPlainSprite;
        // Drawn at the sprite's own pixel size, so its lines stay crisp instead of being stretched.
        PlaceSprite(mMap, sprite, kMapX, kMapY);
    }

    const bool buttonOn = (int32_t)(mMapBlinkTime / kMapButtonBlinkSeconds) % 2 == 0;
    for (int32_t i = 0; i < kNumCameras; ++i)
    {
        const bool watched = (i == mCameraIndex) && buttonOn;
        const Sprite& sprite = watched ? mMapButtonOnSprite : mMapButtonOffSprite;
        mMapButtons[i]->SetVisible(cameraOn && sprite.Get() != nullptr);
        if (mMapButtons[i]->IsVisible())
        {
            PlaceSprite(mMapButtons[i], sprite, kMapCameras[i].mButtonX, kMapCameras[i].mButtonY);
        }

        mMapLabels[i]->SetVisible(cameraOn && mMapLabelSprites[i].Get() != nullptr);
        if (mMapLabels[i]->IsVisible())
        {
            PlaceSprite(mMapLabels[i], mMapLabelSprites[i], kMapCameras[i].mLabelX, kMapCameras[i].mLabelY);
        }
    }

    // The tablet bar at the bottom: shown while the power is on, in the office and with the cameras
    // up (retail shows a bar in both views; the original's own objects for the camera view are
    // magenta mouse zones, so this reuses the office bar's picture). Hidden during the flip.
    const bool barOn = mState == State::Playing && mFlipBarSprite.Get() != nullptr &&
                       (mTabletProgress <= 0.0f || mTabletProgress >= 1.0f);
    mFlipBar->SetVisible(barOn);
    if (barOn)
    {
        // The original puts it at x 255-855 of its 1280 screen, left of centre. Our view is
        // narrower (4:3), where that reads as off to one side, so it's centred instead.
        const float barWidth = 600.0f * scaleX;
        mFlipBar->SetTexture(mFlipBarSprite.Get());
        mFlipBar->SetRect((mScreenWidth - barWidth) * 0.5f, 638.0f * scaleY, barWidth, 60.0f * scaleY);
    }

    // The white frame around the feed (the original's "frame" object: a 2 px rectangle inset 17 px)
    // and the recording dot, which blinks on and off with the map.
    const float kBorder[4][4] = {
        { 17.0f, 16.0f, 1247.0f, 2.0f },        // top
        { 17.0f, 702.0f, 1247.0f, 2.0f },       // bottom
        { 17.0f, 16.0f, 2.0f, 688.0f },         // left
        { 1262.0f, 16.0f, 2.0f, 688.0f },       // right
    };
    for (int32_t e = 0; e < 4; ++e)
    {
        mCamBorder[e]->SetVisible(cameraOn);
        if (cameraOn)
        {
            mCamBorder[e]->SetRect(kBorder[e][0] * scaleX, kBorder[e][1] * scaleY,
                                   kBorder[e][2] * scaleX, kBorder[e][3] * scaleY);
        }
    }

    // The usage meter uses the original's five coloured pictures, drawn next to our own
    // "Usage:" text rather than at the original's HUD position.
    const Sprite& usageSprite = mUsageSprites[glm::clamp(mUsage, 1, 5) - 1];
    mUsageMeter->SetVisible(mState == State::Playing && mHudRevealed && usageSprite.Get() != nullptr);
    if (mUsageMeter->IsVisible())
    {
        mUsageMeter->SetTexture(usageSprite.Get());
        mUsageMeter->SetRect(100.0f, mScreenHeight - 48.0f,
                             usageSprite.mWidth * mScreenWidth / 640.0f, usageSprite.mHeight * mScreenHeight / 480.0f);
    }

    const bool recOn = (int32_t)(mMapBlinkTime / kMapBlinkSeconds) % 2 == 0;
    mCamRec->SetVisible(cameraOn && recOn && mCamRecSprite.Get() != nullptr);
    if (mCamRec->IsVisible())
    {
        mCamRec->SetTexture(mCamRecSprite.Get());
        mCamRec->SetRect(68.0f * scaleX, 52.0f * scaleY, 50.0f * scaleX, 50.0f * scaleY);
    }

    // The flash plays over the tablet and stops on its own.
    int32_t flashFrame = -1;
    if (mFlashTime >= 0.0f && cameraOn)
    {
        mFlashTime += deltaTime;
        flashFrame = (int32_t)(mFlashTime * kFlashFps);
        if (flashFrame >= kFlashFrames)
        {
            flashFrame = -1;
            mFlashTime = -1.0f;
        }
    }
    else if (!cameraOn)
    {
        mFlashTime = -1.0f;
    }

    for (int32_t b = 0; b < 2; ++b)
    {
        const float top = (flashFrame >= 0) ? kFlashBands[flashFrame][b][0] : 0.0f;
        const float bottom = (flashFrame >= 0) ? kFlashBands[flashFrame][b][1] : 0.0f;
        mFlashBands[b]->SetVisible(bottom > top);
        if (bottom > top)
        {
            mFlashBands[b]->SetRect(0.0f, top * scaleY, mScreenWidth, (bottom - top) * scaleY);
        }
    }
}

void FnafGame::UpdateHud()
{
    // #285 hides the whole readout when the power goes: no clock, no night, no power, no usage —
    // only the dark office is left. The clock keeps running underneath (#263, #302).
    const bool playing = (mState == State::Playing || mState == State::PowerOut);
    const bool hudOn = (mState == State::Playing);
    mTimeText->SetVisible(hudOn);
    mNightText->SetVisible(hudOn);
    // Nothing shows the power and usage at the start of a night: they appear when the cameras are
    // first raised (#5) and only the power-out hides them again, so the night opens without them.
    mPowerText->SetVisible(hudOn && mHudRevealed);
    mUsageText->SetVisible(hudOn && mHudRevealed);

    if (!playing || !hudOn)
    {
        return;
    }

    char text[64];
    snprintf(text, sizeof(text), "%d AM", mHour == 0 ? 12 : mHour);
    mTimeText->SetText(text);
    char nightLabel[16];
    snprintf(nightLabel, sizeof(nightLabel), "Night %d", mNight);
    mNightText->SetText(nightLabel);

    snprintf(text, sizeof(text), "Power left: %d%%", (int)ceil(mPower));
    mPowerText->SetText(text);
    mUsageText->SetText("Usage:");      // the meter's picture is drawn next to it
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
    mRobotVoice.Stop();
    mRobotVoiceOn = false;
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
