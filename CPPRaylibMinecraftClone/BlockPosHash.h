#pragma once

#include "BlockPos.h"

struct BlockPosHash
{
    size_t operator()(const BlockPos& p) const
    {
        size_t h1 = std::hash<int>{}(p.x);
        size_t h2 = std::hash<int>{}(p.y);
        size_t h3 = std::hash<int>{}(p.z);

        return h1 ^
            (h2 << 1) ^
            (h3 << 2);
    }
};