#pragma once

#include "raylib.h"
#include "Block.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <queue>

const int CHUNK_SIZE = 16;

// Thread counts (tune here)
const int TERRAIN_WORKER_COUNT = 2;  // world / noise generation
const int MESH_WORKER_COUNT = 2;     // CPU mesh vertex building

extern int g_renderDistance;
extern int g_unloadDistance;

struct Chunk
{
    int x = 0;
    int z = 0;
    Mesh mesh = { 0 };
    bool dirty = true;
    bool generated = false;
    bool meshing = false;   // CPU mesh job in flight
    std::vector<Block> blocks;
};

struct ChunkKey
{
    int x;
    int z;

    bool operator==(const ChunkKey& other) const
    {
        return x == other.x && z == other.z;
    }
};

struct ChunkKeyHash
{
    size_t operator()(const ChunkKey& k) const
    {
        return std::hash<int>{}(k.x) ^ (std::hash<int>{}(k.z) << 1);
    }
};

extern std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks;

void SetupWorld();
void ShutdownWorld();
void UpdateWorldChunks(const Camera3D& camera);

void SetRenderDistance(int distance);
int GetRenderDistance();

Block* GetBlockAt(Vector3 position);
bool IsBlockAt(Vector3 position);

void DestroyBlockAt(Vector3 position);

RaycastHit RaycastBlock(
    Vector3 origin,
    Vector3 direction,
    float maxDistance
);

Chunk* GetChunkAt(int chunkX, int chunkZ);
Chunk* GetChunkContainingBlock(Vector3 position);

void RebuildChunk(Chunk& chunk);
void RebuildAllDirtyChunks(const Camera3D& camera);

void MarkChunkDirty(Vector3 position);
void MarkNeighborChunksDirty(Vector3 position);

void DrawWorldChunks(Material material, const Camera3D& camera);

void UnloadWorldChunks();

void DrawChunkBorders();

void AddBlock(Vector3 position, const std::string& blockType);

int GetLoadedChunkCount();
int GetPendingChunkJobs();
int GetPendingMeshJobs();
int GetWorldModCount();
int GetDirtyChunkCount();
size_t GetTotalBlockCount();
