#pragma once

#include "raylib.h"
#include "Block.h"
#include <vector>
#include <string>

const int CHUNK_SIZE = 16;

struct Chunk
{
    int x;
    int y;
    int z;

    Mesh mesh = { 0 };

    bool dirty = true;
};

extern std::vector<Block> blocks;
extern std::vector<Chunk> chunks;

void SetupWorld();

Block* GetBlockAt(Vector3 position);
bool IsBlockAt(Vector3 position);

void DestroyBlock(Block* block);

RaycastHit RaycastBlock(
    Vector3 origin,
    Vector3 direction,
    float maxDistance
);

Chunk* GetChunkAt(int chunkX, int chunkY, int chunkZ);
Chunk* GetChunkContainingBlock(Vector3 position);

void CreateChunks();
void RebuildChunk(Chunk& chunk);
void RebuildAllDirtyChunks();

Mesh GenerateChunkMesh(Chunk& chunk);

void MarkChunkDirty(Vector3 position);
void MarkNeighborChunksDirty(Vector3 position);

void DrawWorldChunks(Material material, const Camera3D& camera);

void UnloadWorldChunks();

void DrawChunkBorders();

void AddBlock(Vector3 position, const std::string& blockType);