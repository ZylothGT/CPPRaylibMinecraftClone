#pragma once

#include "raylib.h"
#include <string>

struct Block
{
    Vector3 position;
    std::string blockType;
};

struct RaycastHit
{
    bool hit = false;
    Vector3 blockPosition = { 0 };
    Vector3 normal = { 0 };
    std::string blockType;
};
