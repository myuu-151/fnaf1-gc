#include <stdint.h>

#undef min
#undef max

#include "Engine.h"
#include "Log.h"

#include "FnafGame.h"

// A code-only Octave game: no editor project or scene. The engine boots the
// project "FNAF1" (sd:/FNAF1/FNAF1.octp) for its engine assets, and FnafGame
// builds everything else at runtime from sd:/FNAF1/Data.

static FnafGame* sGame = nullptr;

void OctPreInitialize(EngineConfig& config)
{
    GetEngineState()->mStandalone = true;

    config.mProjectName = "FNAF1";
    config.mUseAssetRegistry = true;

    if (config.mWindowWidth == 0)
        config.mWindowWidth = 1280;

    if (config.mWindowHeight == 0)
        config.mWindowHeight = 720;
}

void OctPostInitialize()
{
    sGame = new FnafGame();

    if (!sGame->Initialize())
    {
        LogError("FNAF1: initialization failed");
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
