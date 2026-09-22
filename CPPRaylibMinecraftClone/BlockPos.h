#pragma once

struct BlockPos
{
    int x;
    int y;
    int z;

    bool operator==(const BlockPos& other) const
    {
        return x == other.x &&
            y == other.y &&
            z == other.z;
    }
};