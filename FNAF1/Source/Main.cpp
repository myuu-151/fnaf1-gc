#include <stdint.h>

#undef min
#undef max

#include "Engine.h"
#include "Log.h"

#include "FnafGame.h"

// Octave's packager generates these; embedded builds ("GameCube Embedded") fill them.
#if __has_include("../Generated/EmbeddedAssets.h")
#include "../Generated/EmbeddedAssets.h"
#include "../Generated/EmbeddedScripts.h"
#define FNAF_HAS_GENERATED 1
#else
#define FNAF_HAS_GENERATED 0
#endif

// No scene: FnafGame builds everything at runtime from FNAF1/Scripts/Data
// (packaged with the project and served from the ISO).

// SD diagnostic log (Octave System_Dolphin.cpp; writes /octiso.log when the local logger is enabled).
void OctLog(const char* format, ...);

static FnafGame* sGame = nullptr;

void OctPreInitialize(EngineConfig& config)
{
    GetEngineState()->mStandalone = true;

    // The project name comes from Config.ini (Project=FNAF1), as in any packaged Octave
    // game. Don't set it here: a project name this early makes the engine look for the
    // disc image while it's still reading the config, before video is up.

    if (config.mWindowWidth == 0)
        config.mWindowWidth = 1280;

    if (config.mWindowHeight == 0)
        config.mWindowHeight = 720;

#if FNAF_HAS_GENERATED
    config.mEmbeddedAssetCount = gNumEmbeddedAssets;
    config.mEmbeddedAssets = gEmbeddedAssets;
    config.mEmbeddedScriptCount = gNumEmbeddedScripts;
    config.mEmbeddedScripts = gEmbeddedScripts;
    config.mEmbeddedConfig = gEmbeddedConfig_Data;
    config.mEmbeddedConfigSize = gEmbeddedConfig_Size;
#endif
}

void OctPostInitialize()
{
    OctLog("FNAF1: engine initialized, screen %dx%d", GetEngineState()->mWindowWidth, GetEngineState()->mWindowHeight);
    sGame = new FnafGame();

    if (!sGame->Initialize())
    {
        OctLog("FNAF1: game initialization FAILED");
        LogError("FNAF1: initialization failed");
    }
    else
    {
        OctLog("FNAF1: game initialized");
    }
}

void OctPreUpdate()
{

}

void OctPostUpdate()
{
    if (sGame != nullptr)
    {
        sGame->Update(GetEngineState()->mGameDeltaTime);
    }
}

void OctPreShutdown()
{
    delete sGame;
    sGame = nullptr;
}

void OctPostShutdown()
{

}
