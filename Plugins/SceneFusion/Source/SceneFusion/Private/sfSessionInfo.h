#pragma once

#include "SceneFusion.h"
#include "ksRoomInfo.h"
#include <Runtime/Core/Public/Misc/DateTime.h>

using namespace KS::Reactor;

/**
 * Session data that is available without connecting to a room. Consists of room info and level creator.
 */
struct sfSessionInfo
{
public:
    FString ProjectName;
    FString Creator;
    FString LevelName;
    bool CanStop;
    FString RequiredVersion;
    ksRoomInfo::SPtr RoomInfoPtr;
    FDateTime Time;

    sfSessionInfo() :
        ProjectName{ "Default" },
        Creator{ "Test" },
        LevelName{ "Level" },
        CanStop{ false },
        RequiredVersion{ "2.0.0" },
        RoomInfoPtr { nullptr }
    {
        Time = FDateTime::Now();
    }
};