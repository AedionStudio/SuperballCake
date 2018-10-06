#pragma once

#include <sfName.h>

using namespace KS::SceneFusion2;

/**
 * Property names
 */
class sfProp
{
public:
    static const sfName Name;
    static const sfName Class;
    static const sfName Label;
    static const sfName Folder;
    static const sfName Location;
    static const sfName Rotation;
    static const sfName Scale;
    static const sfName Mesh;
    static const sfName Materials;
    static const sfName Template;
    static const sfName Id;
    static const sfName Flashlight;
    static const sfName Level;
    static const sfName IsPersistentLevel;
};

/**
 * Object types
 */
class sfType
{
public:
    static const sfName Actor;
    static const sfName Avatar;
    static const sfName Level;
    static const sfName LevelLock;
};