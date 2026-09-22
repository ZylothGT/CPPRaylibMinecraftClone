#pragma once

#include <vector>
#include "Block.h"

struct Chunk
{
    std::vector<Block> blocks;

    Mesh mesh = { 0 };
    bool dirty = true;
};