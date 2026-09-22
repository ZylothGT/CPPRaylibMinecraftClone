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
    Block* block;
    Vector3 position;
    Vector3 normal;
};