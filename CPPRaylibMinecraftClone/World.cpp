#include "World.h"
#include "raymath.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <cstdint>
#include <climits>
#include <random>
#include <chrono>
#include <vector>

uint32_t worldSeed = 0;

using json = nlohmann::json;

const int MIN_BUILD_Y = -10;
const int MAX_BUILD_Y = 365;

struct BlockKey
{
    int x, y, z;
    bool operator==(const BlockKey& o) const
    {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct BlockKeyHash
{
    size_t operator()(const BlockKey& p) const
    {
        return std::hash<int>{}(p.x) ^ (std::hash<int>{}(p.y) << 1) ^ (std::hash<int>{}(p.z) << 2);
    }
};

std::unordered_map<BlockKey, std::string, BlockKeyHash> blockOccupancy;
std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks;
std::unordered_map<ChunkKey, std::unordered_map<BlockKey, std::string, BlockKeyHash>, ChunkKeyHash> chunkMods;

int g_renderDistance = 6;
int g_unloadDistance = 9; // renderDistance + 3 padding ring

static ChunkKey ChunkKeyFromBlock(int bx, int bz)
{
    return {
        (int)floorf((float)bx / (float)CHUNK_SIZE),
        (int)floorf((float)bz / (float)CHUNK_SIZE)
    };
}

static void SetWorldMod(int bx, int by, int bz, const std::string& type)
{
    chunkMods[ChunkKeyFromBlock(bx, bz)][{ bx, by, bz }] = type;
}

void SetRenderDistance(int distance)
{
    if (distance < 2) distance = 2;
    if (distance > 16) distance = 16;
    g_renderDistance = distance;
    // Keep an extra ring of terrain data so border face-culling has neighbours
    g_unloadDistance = distance + 3;
}

int GetRenderDistance()
{
    return g_renderDistance;
}

// =====================================================
// TEXTURES
// =====================================================

struct FaceUV { float u0, v0, u1, v1; };
struct BlockTextures { FaceUV top, bottom, north, south, east, west; };
std::unordered_map<std::string, BlockTextures> blockTextures;

// =====================================================
// TERRAIN WORKERS (1–2 threads)
// =====================================================

struct GeneratedChunkData
{
    int chunkX = 0;
    int chunkZ = 0;
    std::vector<Block> blocks;
};

static std::mutex g_terrainMutex;
static std::condition_variable g_terrainCv;
// Pending jobs – workers always pick the closest to the player
static std::vector<ChunkKey> g_terrainPending;
static std::unordered_set<ChunkKey, ChunkKeyHash> g_terrainPendingSet;
static std::unordered_set<ChunkKey, ChunkKeyHash> g_terrainInFlight;
// Chunks already requested (pending, generating, waiting apply, or loaded).
static std::unordered_set<ChunkKey, ChunkKeyHash> g_terrainScheduled;
static std::vector<GeneratedChunkData> g_terrainCompleted;
static std::atomic<bool> g_terrainRunning{ false };
static std::vector<std::thread> g_terrainThreads;
static int g_lastPlayerChunkX = INT_MAX;
static int g_lastPlayerChunkZ = INT_MAX;

static float Hash2D(int x, int z)
{
    uint32_t h =
        static_cast<uint32_t>(x) * 374761393u +
        static_cast<uint32_t>(z) * 668265263u +
        worldSeed;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFF) / 16777215.0f;
}

static float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

static float ValueNoise(float x, float z)
{
    int x0 = (int)floorf(x), z0 = (int)floorf(z);
    int x1 = x0 + 1, z1 = z0 + 1;
    float tx = SmoothStep(x - (float)x0);
    float tz = SmoothStep(z - (float)z0);
    float a = Hash2D(x0, z0), b = Hash2D(x1, z0);
    float c = Hash2D(x0, z1), d = Hash2D(x1, z1);
    float ab = a + (b - a) * tx;
    float cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
}

static float TerrainNoise(float x, float z)
{
    float total = 0.0f, amplitude = 1.0f, frequency = 1.0f, maxAmp = 0.0f;
    for (int i = 0; i < 4; i++)
    {
        total += ValueNoise(x * frequency, z * frequency) * amplitude;
        maxAmp += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return total / maxAmp;
}

static GeneratedChunkData GenerateTerrainData(int chunkX, int chunkZ)
{
    GeneratedChunkData data;
    data.chunkX = chunkX;
    data.chunkZ = chunkZ;

    const float noiseScale = 0.025f;
    const int baseHeight = 0;
    const int heightVariation = 12;
    const int dirtDepth = 3;

    int minX = chunkX * CHUNK_SIZE;
    int maxX = minX + CHUNK_SIZE - 1;
    int minZ = chunkZ * CHUNK_SIZE;
    int maxZ = minZ + CHUNK_SIZE - 1;

    data.blocks.reserve(CHUNK_SIZE * CHUNK_SIZE * 16);

    for (int x = minX; x <= maxX; x++)
    {
        for (int z = minZ; z <= maxZ; z++)
        {
            float noise = TerrainNoise(x * noiseScale, z * noiseScale);
            int height = baseHeight + (int)(noise * heightVariation);
            height = std::max(height, -10);

            for (int y = -10; y <= height; y++)
            {
                std::string type;
                if (y == height) type = "grass";
                else if (y >= height - dirtDepth) type = "dirt";
                else if (y != -10) type = "stone";
                else type = "bedrock";

                data.blocks.push_back({
                    { (float)x, (float)y, (float)z },
                    type
                    });
            }
        }
    }
    return data;
}

static void TerrainWorkerMain()
{
    while (g_terrainRunning.load())
    {
        ChunkKey job{ 0, 0 };
        bool hasJob = false;

        {
            std::unique_lock<std::mutex> lock(g_terrainMutex);
            g_terrainCv.wait_for(lock, std::chrono::milliseconds(40), [] {
                return !g_terrainPending.empty() || !g_terrainRunning.load();
                });
            if (!g_terrainRunning.load()) break;
            if (g_terrainPending.empty()) continue;

            // Always generate the closest pending chunk to the player first
            int bestIndex = 0;
            int bestDist = INT_MAX;
            const int px = g_lastPlayerChunkX == INT_MAX ? 0 : g_lastPlayerChunkX;
            const int pz = g_lastPlayerChunkZ == INT_MAX ? 0 : g_lastPlayerChunkZ;

            for (int i = 0; i < (int)g_terrainPending.size(); i++)
            {
                int dx = g_terrainPending[i].x - px;
                int dz = g_terrainPending[i].z - pz;
                int dist = dx * dx + dz * dz;
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestIndex = i;
                }
            }

            job = g_terrainPending[bestIndex];
            g_terrainPending[bestIndex] = g_terrainPending.back();
            g_terrainPending.pop_back();
            g_terrainPendingSet.erase(job);
            g_terrainInFlight.insert(job);
            hasJob = true;
        }

        if (!hasJob) continue;

        GeneratedChunkData result = GenerateTerrainData(job.x, job.z);

        {
            std::lock_guard<std::mutex> lock(g_terrainMutex);
            g_terrainInFlight.erase(job);
            g_terrainCompleted.push_back(std::move(result));
        }
    }
}

// =====================================================
// MESH WORKERS (1–2 threads) – CPU only, no OpenGL
// =====================================================

struct MeshJobInput
{
    ChunkKey key;
    std::vector<Block> blocks;
    // Occupancy snapshot for face culling (chunk + 1-block border)
    std::unordered_set<BlockKey, BlockKeyHash> solid;
};

struct MeshJobResult
{
    ChunkKey key;
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> texcoords;
};

static std::mutex g_meshMutex;
static std::condition_variable g_meshCv;
static std::queue<MeshJobInput> g_meshPending;
static std::unordered_set<ChunkKey, ChunkKeyHash> g_meshPendingSet;
static std::unordered_set<ChunkKey, ChunkKeyHash> g_meshInFlight;
static std::vector<MeshJobResult> g_meshCompleted;
static std::atomic<bool> g_meshRunning{ false };
static std::vector<std::thread> g_meshThreads;

static MeshJobResult BuildMeshCPU(const MeshJobInput& input)
{
    MeshJobResult out;
    out.key = input.key;

    auto IsSolid = [&](float x, float y, float z) -> bool
        {
            return input.solid.count({
                (int)floorf(x), (int)floorf(y), (int)floorf(z)
                }) > 0;
        };

    size_t estimate = input.blocks.size() * 4 * 6;
    out.vertices.reserve(estimate * 3);
    out.normals.reserve(estimate * 3);
    out.texcoords.reserve(estimate * 2);

    const float s = 0.5f;

    auto AddFace = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 n, FaceUV uv)
        {
            out.vertices.insert(out.vertices.end(), {
                a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z,
                a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z
                });
            for (int i = 0; i < 6; i++)
            {
                out.normals.push_back(n.x);
                out.normals.push_back(n.y);
                out.normals.push_back(n.z);
            }
            out.texcoords.insert(out.texcoords.end(), {
                uv.u0, uv.v1, uv.u1, uv.v1, uv.u1, uv.v0,
                uv.u0, uv.v1, uv.u1, uv.v0, uv.u0, uv.v0
                });
        };

    for (const Block& block : input.blocks)
    {
        float x = block.position.x;
        float y = block.position.y;
        float z = block.position.z;

        auto texIt = blockTextures.find(block.blockType);
        if (texIt == blockTextures.end())
            continue;
        const BlockTextures& t = texIt->second;

        if (!IsSolid(x, y - 1.0f, z))
            AddFace({ x - s, y - s, z - s }, { x + s, y - s, z - s },
                { x + s, y - s, z + s }, { x - s, y - s, z + s },
                { 0, -1, 0 }, t.bottom);
        if (!IsSolid(x, y + 1.0f, z))
            AddFace({ x - s, y + s, z + s }, { x + s, y + s, z + s },
                { x + s, y + s, z - s }, { x - s, y + s, z - s },
                { 0, 1, 0 }, t.top);
        if (!IsSolid(x, y, z + 1.0f))
            AddFace({ x - s, y - s, z + s }, { x + s, y - s, z + s },
                { x + s, y + s, z + s }, { x - s, y + s, z + s },
                { 0, 0, 1 }, t.south);
        if (!IsSolid(x, y, z - 1.0f))
            AddFace({ x + s, y - s, z - s }, { x - s, y - s, z - s },
                { x - s, y + s, z - s }, { x + s, y + s, z - s },
                { 0, 0, -1 }, t.north);
        if (!IsSolid(x + 1.0f, y, z))
            AddFace({ x + s, y - s, z + s }, { x + s, y - s, z - s },
                { x + s, y + s, z - s }, { x + s, y + s, z + s },
                { 1, 0, 0 }, t.east);
        if (!IsSolid(x - 1.0f, y, z))
            AddFace({ x - s, y - s, z - s }, { x - s, y - s, z + s },
                { x - s, y + s, z + s }, { x - s, y + s, z - s },
                { -1, 0, 0 }, t.west);
    }

    return out;
}

static void MeshWorkerMain()
{
    while (g_meshRunning.load())
    {
        MeshJobInput job;
        bool hasJob = false;

        {
            std::unique_lock<std::mutex> lock(g_meshMutex);
            g_meshCv.wait_for(lock, std::chrono::milliseconds(40), [] {
                return !g_meshPending.empty() || !g_meshRunning.load();
                });
            if (!g_meshRunning.load()) break;
            if (g_meshPending.empty()) continue;

            job = std::move(g_meshPending.front());
            g_meshPending.pop();
            g_meshPendingSet.erase(job.key);
            g_meshInFlight.insert(job.key);
            hasJob = true;
        }

        if (!hasJob) continue;

        MeshJobResult result = BuildMeshCPU(job);

        {
            std::lock_guard<std::mutex> lock(g_meshMutex);
            g_meshInFlight.erase(job.key);
            g_meshCompleted.push_back(std::move(result));
        }
    }
}

// =====================================================
// START / STOP WORKERS
// =====================================================

static void StartWorkers()
{
    if (!g_terrainRunning.load())
    {
        g_terrainRunning = true;
        g_terrainThreads.clear();
        for (int i = 0; i < TERRAIN_WORKER_COUNT; i++)
            g_terrainThreads.emplace_back(TerrainWorkerMain);
    }

    if (!g_meshRunning.load())
    {
        g_meshRunning = true;
        g_meshThreads.clear();
        for (int i = 0; i < MESH_WORKER_COUNT; i++)
            g_meshThreads.emplace_back(MeshWorkerMain);
    }
}

static void StopWorkers()
{
    g_terrainRunning = false;
    g_meshRunning = false;
    g_terrainCv.notify_all();
    g_meshCv.notify_all();

    for (auto& t : g_terrainThreads)
        if (t.joinable()) t.join();
    g_terrainThreads.clear();

    for (auto& t : g_meshThreads)
        if (t.joinable()) t.join();
    g_meshThreads.clear();

    {
        std::lock_guard<std::mutex> lock(g_terrainMutex);
        g_terrainPending.clear();
        g_terrainPendingSet.clear();
        g_terrainInFlight.clear();
        g_terrainScheduled.clear();
        g_terrainCompleted.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_meshMutex);
        while (!g_meshPending.empty()) g_meshPending.pop();
        g_meshPendingSet.clear();
        g_meshInFlight.clear();
        g_meshCompleted.clear();
    }
}

static void QueueChunkGeneration(int chunkX, int chunkZ)
{
    ChunkKey key{ chunkX, chunkZ };

    // Already loaded – nothing to do
    if (chunks.find(key) != chunks.end())
        return;

    std::lock_guard<std::mutex> lock(g_terrainMutex);

    // Already requested (pending, generating, or waiting to be applied on main thread)
    if (g_terrainScheduled.count(key))
        return;

    g_terrainScheduled.insert(key);
    g_terrainPending.push_back(key);
    g_terrainPendingSet.insert(key);
    g_terrainCv.notify_one();
}

// Snapshot only this chunk + loaded neighbours (NOT the whole world map)
static std::unordered_set<BlockKey, BlockKeyHash> SnapshotSolidAround(const Chunk& chunk)
{
    std::unordered_set<BlockKey, BlockKeyHash> solid;
    solid.reserve(chunk.blocks.size() + 512);

    auto AddChunkBlocks = [&](const Chunk* c)
        {
            if (!c) return;
            for (const Block& b : c->blocks)
            {
                solid.insert({
                    (int)floorf(b.position.x),
                    (int)floorf(b.position.y),
                    (int)floorf(b.position.z)
                    });
            }
        };

    AddChunkBlocks(&chunk);
    AddChunkBlocks(GetChunkAt(chunk.x - 1, chunk.z));
    AddChunkBlocks(GetChunkAt(chunk.x + 1, chunk.z));
    AddChunkBlocks(GetChunkAt(chunk.x, chunk.z - 1));
    AddChunkBlocks(GetChunkAt(chunk.x, chunk.z + 1));

    return solid;
}

static void QueueMeshJob(Chunk& chunk)
{
    if (chunk.meshing)
        return;

    ChunkKey key{ chunk.x, chunk.z };

    {
        std::lock_guard<std::mutex> lock(g_meshMutex);
        if (g_meshPendingSet.count(key) || g_meshInFlight.count(key))
            return;
        g_meshPendingSet.insert(key);
    }

    MeshJobInput input;
    input.key = key;
    input.blocks = chunk.blocks;
    input.solid = SnapshotSolidAround(chunk);

    {
        std::lock_guard<std::mutex> lock(g_meshMutex);
        g_meshPending.push(std::move(input));
    }
    chunk.meshing = true;
    chunk.dirty = false;
    g_meshCv.notify_one();
}

// =====================================================
// APPLY MODS (fast, per-chunk index)
// =====================================================

static void ApplyWorldModsToChunk(Chunk& chunk)
{
    ChunkKey key{ chunk.x, chunk.z };
    auto chunkIt = chunkMods.find(key);
    if (chunkIt == chunkMods.end() || chunkIt->second.empty())
        return;

    const auto& mods = chunkIt->second;

    std::unordered_map<BlockKey, size_t, BlockKeyHash> index;
    index.reserve(chunk.blocks.size());
    for (size_t i = 0; i < chunk.blocks.size(); i++)
    {
        BlockKey bk{
            (int)floorf(chunk.blocks[i].position.x),
            (int)floorf(chunk.blocks[i].position.y),
            (int)floorf(chunk.blocks[i].position.z)
        };
        index[bk] = i;
    }

    std::vector<size_t> toRemove;
    for (const auto& mod : mods)
    {
        auto it = index.find(mod.first);
        if (it == index.end())
        {
            if (!mod.second.empty())
            {
                chunk.blocks.push_back({
                    { (float)mod.first.x, (float)mod.first.y, (float)mod.first.z },
                    mod.second
                    });
            }
            continue;
        }
        if (mod.second.empty())
            toRemove.push_back(it->second);
        else
            chunk.blocks[it->second].blockType = mod.second;
    }

    std::sort(toRemove.begin(), toRemove.end(), std::greater<size_t>());
    for (size_t idx : toRemove)
    {
        if (idx >= chunk.blocks.size()) continue;
        chunk.blocks[idx] = std::move(chunk.blocks.back());
        chunk.blocks.pop_back();
    }
}

// =====================================================
// MAIN-THREAD: integrate terrain + mesh results
// =====================================================

static void ProcessCompletedTerrain()
{
    // Apply more per frame at startup so the queue drains instead of looking "stuck generating"
    const size_t maxPerFrame = 6;
    std::vector<GeneratedChunkData> finished;
    {
        std::lock_guard<std::mutex> lock(g_terrainMutex);
        while (!g_terrainCompleted.empty() && finished.size() < maxPerFrame)
        {
            finished.push_back(std::move(g_terrainCompleted.back()));
            g_terrainCompleted.pop_back();
        }
    }

    for (auto& data : finished)
    {
        ChunkKey key{ data.chunkX, data.chunkZ };
        // Duplicate result from an old race – discard, keep scheduled so we don't re-request
        if (chunks.find(key) != chunks.end())
            continue;

        Chunk chunk{};
        chunk.x = data.chunkX;
        chunk.z = data.chunkZ;
        chunk.mesh = { 0 };
        chunk.dirty = true;
        chunk.generated = true;
        chunk.meshing = false;
        chunk.blocks = std::move(data.blocks);

        ApplyWorldModsToChunk(chunk);

        for (const Block& b : chunk.blocks)
        {
            BlockKey bk{
                (int)floorf(b.position.x),
                (int)floorf(b.position.y),
                (int)floorf(b.position.z)
            };
            blockOccupancy[bk] = b.blockType;
        }

        chunks[key] = std::move(chunk);

        // Wake neighbours that were waiting for us before their first mesh
        // (so they mesh once with correct border culling — no remesh wave).
        static const int offsets[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
        for (auto& o : offsets)
        {
            Chunk* n = GetChunkAt(data.chunkX + o[0], data.chunkZ + o[1]);
            if (n && !n->meshing && n->mesh.vertexCount == 0)
                n->dirty = true;
        }
    }
}

static void ProcessCompletedMeshes()
{
    const size_t maxPerFrame = 3;
    std::vector<MeshJobResult> finished;
    {
        std::lock_guard<std::mutex> lock(g_meshMutex);
        while (!g_meshCompleted.empty() && finished.size() < maxPerFrame)
        {
            finished.push_back(std::move(g_meshCompleted.back()));
            g_meshCompleted.pop_back();
        }
    }

    for (auto& result : finished)
    {
        Chunk* chunk = GetChunkAt(result.key.x, result.key.z);
        if (!chunk)
            continue;

        chunk->meshing = false;

        // GPU upload must stay on the main thread
        if (chunk->mesh.vertexCount > 0)
            UnloadMesh(chunk->mesh);

        Mesh mesh = { 0 };
        mesh.vertexCount = (int)(result.vertices.size() / 3);
        mesh.triangleCount = mesh.vertexCount / 3;

        if (!result.vertices.empty())
        {
            mesh.vertices = (float*)MemAlloc(result.vertices.size() * sizeof(float));
            memcpy(mesh.vertices, result.vertices.data(), result.vertices.size() * sizeof(float));
        }
        if (!result.normals.empty())
        {
            mesh.normals = (float*)MemAlloc(result.normals.size() * sizeof(float));
            memcpy(mesh.normals, result.normals.data(), result.normals.size() * sizeof(float));
        }
        if (!result.texcoords.empty())
        {
            mesh.texcoords = (float*)MemAlloc(result.texcoords.size() * sizeof(float));
            memcpy(mesh.texcoords, result.texcoords.data(), result.texcoords.size() * sizeof(float));
        }

        if (mesh.vertexCount > 0)
            UploadMesh(&mesh, false);

        chunk->mesh = mesh;
        // If edited while meshing, leave dirty=true; RebuildAllDirtyChunks will requeue later
    }
}

// =====================================================
// TEXTURES LOAD
// =====================================================

bool LoadBlockTextures()
{
    blockTextures.clear();
    const std::string dir = "generated/blocks/";

    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;

        std::string name = entry.path().stem().string();
        std::ifstream file(entry.path().string());
        if (!file.is_open()) continue;

        try
        {
            json data;
            file >> data;
            BlockTextures textures{};
            auto ReadFace = [&](const char* face) -> FaceUV
                {
                    auto uv = data["textures"][face]["uv"];
                    return {
                        uv["u0"].get<float>(), uv["v0"].get<float>(),
                        uv["u1"].get<float>(), uv["v1"].get<float>()
                    };
                };
            textures.top = ReadFace("top");
            textures.bottom = ReadFace("bottom");
            textures.north = ReadFace("north");
            textures.south = ReadFace("south");
            textures.east = ReadFace("east");
            textures.west = ReadFace("west");
            blockTextures[name] = textures;
            TraceLog(LOG_INFO, "Loaded block textures: %s", name.c_str());
        }
        catch (const std::exception& e)
        {
            TraceLog(LOG_ERROR, "Failed to parse %s: %s",
                entry.path().string().c_str(), e.what());
        }
    }
    return !blockTextures.empty();
}

// =====================================================
// COORD / CHUNK LOOKUP
// =====================================================

int WorldToChunk(float p) { return (int)floorf(p / (float)CHUNK_SIZE); }

int GetLocalBlockCoordinate(float position)
{
    int c = (int)floorf(position);
    int local = c % CHUNK_SIZE;
    if (local < 0) local += CHUNK_SIZE;
    return local;
}

Chunk* GetChunkAt(int chunkX, int chunkZ)
{
    auto it = chunks.find({ chunkX, chunkZ });
    return it == chunks.end() ? nullptr : &it->second;
}

Chunk* GetChunkContainingBlock(Vector3 position)
{
    return GetChunkAt(WorldToChunk(position.x), WorldToChunk(position.z));
}

// =====================================================
// UNLOAD / UPDATE
// =====================================================

void UnloadChunk(int chunkX, int chunkZ)
{
    ChunkKey key{ chunkX, chunkZ };
    auto it = chunks.find(key);
    if (it == chunks.end()) return;

    for (const Block& b : it->second.blocks)
    {
        blockOccupancy.erase({
            (int)floorf(b.position.x),
            (int)floorf(b.position.y),
            (int)floorf(b.position.z)
            });
    }

    if (it->second.mesh.vertexCount > 0)
        UnloadMesh(it->second.mesh);

    chunks.erase(it);

    // Allow this chunk to be generated again only after a real unload
    {
        std::lock_guard<std::mutex> lock(g_terrainMutex);
        g_terrainScheduled.erase(key);
    }
}

// True if chunk is within render distance AND roughly inside the camera view cone.
static bool IsChunkInView(const Chunk& chunk, const Camera3D& camera)
{
    const float minX = chunk.x * CHUNK_SIZE - 0.5f;
    const float maxX = (chunk.x + 1) * CHUNK_SIZE - 0.5f;
    const float minY = -10.5f;
    const float maxY = 40.5f;
    const float minZ = chunk.z * CHUNK_SIZE - 0.5f;
    const float maxZ = (chunk.z + 1) * CHUNK_SIZE - 0.5f;

    Vector3 center = {
        (minX + maxX) * 0.5f,
        (minY + maxY) * 0.5f,
        (minZ + maxZ) * 0.5f
    };

    Vector3 toCenter = Vector3Subtract(center, camera.position);
    float dist = Vector3Length(toCenter);
    float maxDist = (float)((g_renderDistance + 1) * CHUNK_SIZE);
    if (dist > maxDist)
        return false;

    // Always keep chunks under / next to the player
    if (dist < (float)(CHUNK_SIZE * 2))
        return true;

    Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    Vector3 up = Vector3Normalize(Vector3CrossProduct(right, forward));

    // Full 3D FOV: vertical from fovy, horizontal from aspect ratio
    float halfFovV = camera.fovy * DEG2RAD * 0.5f;
    float aspect = 16.0f / 9.0f;
    if (GetScreenHeight() > 0)
        aspect = (float)GetScreenWidth() / (float)GetScreenHeight();
    float halfFovH = atanf(tanf(halfFovV) * aspect);

    const float margin = 15.0f * DEG2RAD;
    float limitV = halfFovV + margin;
    float limitH = halfFovH + margin;

    // Center + 8 corners of the chunk volume (pitch + yaw both matter)
    Vector3 points[9] = {
        center,
        { minX, minY, minZ }, { maxX, minY, minZ },
        { minX, maxY, minZ }, { maxX, maxY, minZ },
        { minX, minY, maxZ }, { maxX, minY, maxZ },
        { minX, maxY, maxZ }, { maxX, maxY, maxZ }
    };

    for (int i = 0; i < 9; i++)
    {
        Vector3 to = Vector3Subtract(points[i], camera.position);
        float len = Vector3Length(to);
        if (len < 0.001f)
            return true;
        to = Vector3Scale(to, 1.0f / len);

        float dForward = Vector3DotProduct(to, forward);
        if (dForward <= 0.0f)
            continue; // behind the camera

        float dUp = Vector3DotProduct(to, up);
        float dRight = Vector3DotProduct(to, right);

        float pitch = fabsf(atan2f(dUp, dForward));
        float yaw = fabsf(atan2f(dRight, dForward));

        if (pitch <= limitV && yaw <= limitH)
            return true;
    }

    return false;
}

void UpdateWorldChunks(const Camera3D& camera)
{
    ProcessCompletedTerrain();
    ProcessCompletedMeshes();

    int playerChunkX = WorldToChunk(camera.position.x);
    int playerChunkZ = WorldToChunk(camera.position.z);
    const int rd = g_renderDistance;
    const int ud = g_unloadDistance;

    // Generate one chunk past render distance so visible chunks have neighbours
    // for correct face culling on the first mesh (no later seam-remesh).
    const int genRadius = rd + 1;
    for (int dx = -genRadius; dx <= genRadius; dx++)
    {
        for (int dz = -genRadius; dz <= genRadius; dz++)
        {
            if (dx * dx + dz * dz > genRadius * genRadius + genRadius)
                continue;
            QueueChunkGeneration(playerChunkX + dx, playerChunkZ + dz);
        }
    }

    // Unload only when clearly outside range (hysteresis via g_unloadDistance)
    if (playerChunkX != g_lastPlayerChunkX || playerChunkZ != g_lastPlayerChunkZ)
    {
        g_lastPlayerChunkX = playerChunkX;
        g_lastPlayerChunkZ = playerChunkZ;

        std::vector<ChunkKey> toUnload;
        for (const auto& pair : chunks)
        {
            int dx = pair.first.x - playerChunkX;
            int dz = pair.first.z - playerChunkZ;
            if (dx * dx + dz * dz > ud * ud)
                toUnload.push_back(pair.first);
        }
        for (const ChunkKey& k : toUnload)
            UnloadChunk(k.x, k.z);
    }
}

void SetupWorld()
{
    blockOccupancy.clear();
    chunks.clear();
    chunkMods.clear();

    std::random_device rd;
    worldSeed = rd() ^ (uint32_t)std::chrono::high_resolution_clock::now()
        .time_since_epoch().count();
    TraceLog(LOG_INFO, "World Seed: %u", worldSeed);
    TraceLog(LOG_INFO, "Terrain workers: %d  Mesh workers: %d",
        TERRAIN_WORKER_COUNT, MESH_WORKER_COUNT);

    LoadBlockTextures();
    StartWorkers();
    Camera3D bootCam = { 0 };
    bootCam.position = { 0, 20, 0 };
    bootCam.target = { 0, 20, 1 };
    bootCam.up = { 0, 1, 0 };
    bootCam.fovy = 60.0f;
    bootCam.projection = CAMERA_PERSPECTIVE;
    UpdateWorldChunks(bootCam);
}

void ShutdownWorld()
{
    StopWorkers();
    UnloadWorldChunks();
}

// =====================================================
// BLOCK QUERIES / EDIT
// =====================================================

Block* GetBlockAt(Vector3 position)
{
    Chunk* chunk = GetChunkContainingBlock(position);
    if (!chunk) return nullptr;

    int ix = (int)floorf(position.x);
    int iy = (int)floorf(position.y);
    int iz = (int)floorf(position.z);

    for (Block& b : chunk->blocks)
    {
        if ((int)floorf(b.position.x) == ix &&
            (int)floorf(b.position.y) == iy &&
            (int)floorf(b.position.z) == iz)
            return &b;
    }
    return nullptr;
}

bool IsBlockAt(Vector3 position)
{
    return blockOccupancy.find({
        (int)floorf(position.x),
        (int)floorf(position.y),
        (int)floorf(position.z)
        }) != blockOccupancy.end();
}

void DestroyBlockAt(Vector3 position)
{
    Chunk* chunk = GetChunkContainingBlock(position);
    if (!chunk) return;

    int ix = (int)floorf(position.x);
    int iy = (int)floorf(position.y);
    int iz = (int)floorf(position.z);

    for (size_t i = 0; i < chunk->blocks.size(); i++)
    {
        Block& b = chunk->blocks[i];
        if ((int)floorf(b.position.x) == ix &&
            (int)floorf(b.position.y) == iy &&
            (int)floorf(b.position.z) == iz)
        {
            blockOccupancy.erase({ ix, iy, iz });
            SetWorldMod(ix, iy, iz, "");
            chunk->blocks[i] = std::move(chunk->blocks.back());
            chunk->blocks.pop_back();
            MarkChunkDirty(position);
            MarkNeighborChunksDirty(position);
            return;
        }
    }
}

void AddBlock(Vector3 position, const std::string& blockType)
{
    if (position.y < MIN_BUILD_Y || position.y > MAX_BUILD_Y - 11)
        return;
    if (IsBlockAt(position))
        return;

    int cx = WorldToChunk(position.x);
    int cz = WorldToChunk(position.z);
    int ix = (int)floorf(position.x);
    int iy = (int)floorf(position.y);
    int iz = (int)floorf(position.z);

    Chunk* chunk = GetChunkAt(cx, cz);
    if (!chunk)
    {
        SetWorldMod(ix, iy, iz, blockType);
        QueueChunkGeneration(cx, cz);
        return;
    }

    chunk->blocks.push_back({ position, blockType });
    blockOccupancy[{ ix, iy, iz }] = blockType;
    SetWorldMod(ix, iy, iz, blockType);
    MarkChunkDirty(position);
    MarkNeighborChunksDirty(position);
}

void MarkChunkDirty(Vector3 position)
{
    Chunk* chunk = GetChunkContainingBlock(position);
    if (chunk) chunk->dirty = true;
}

void MarkNeighborChunksDirty(Vector3 position)
{
    int localX = GetLocalBlockCoordinate(position.x);
    int localZ = GetLocalBlockCoordinate(position.z);
    Chunk* chunk = GetChunkContainingBlock(position);
    if (!chunk) return;

    if (localX == 0)
    {
        Chunk* n = GetChunkAt(chunk->x - 1, chunk->z);
        if (n) n->dirty = true;
    }
    if (localX == CHUNK_SIZE - 1)
    {
        Chunk* n = GetChunkAt(chunk->x + 1, chunk->z);
        if (n) n->dirty = true;
    }
    if (localZ == 0)
    {
        Chunk* n = GetChunkAt(chunk->x, chunk->z - 1);
        if (n) n->dirty = true;
    }
    if (localZ == CHUNK_SIZE - 1)
    {
        Chunk* n = GetChunkAt(chunk->x, chunk->z + 1);
        if (n) n->dirty = true;
    }
}

// =====================================================
// RAYCAST
// =====================================================

RaycastHit RaycastBlock(Vector3 origin, Vector3 direction, float maxDistance)
{
    RaycastHit hit{};
    hit.hit = false;
    direction = Vector3Normalize(direction);
    if (Vector3Length(direction) <= 0.0f) return hit;

    Vector3 gridOrigin = { origin.x + 0.5f, origin.y + 0.5f, origin.z + 0.5f };
    int x = (int)floorf(gridOrigin.x);
    int y = (int)floorf(gridOrigin.y);
    int z = (int)floorf(gridOrigin.z);

    int stepX = direction.x >= 0 ? 1 : -1;
    int stepY = direction.y >= 0 ? 1 : -1;
    int stepZ = direction.z >= 0 ? 1 : -1;

    float tDeltaX = direction.x != 0 ? fabsf(1.0f / direction.x) : INFINITY;
    float tDeltaY = direction.y != 0 ? fabsf(1.0f / direction.y) : INFINITY;
    float tDeltaZ = direction.z != 0 ? fabsf(1.0f / direction.z) : INFINITY;

    float nextX = direction.x >= 0 ? (float)(x + 1) : (float)x;
    float nextY = direction.y >= 0 ? (float)(y + 1) : (float)y;
    float nextZ = direction.z >= 0 ? (float)(z + 1) : (float)z;

    float tMaxX = direction.x != 0 ? (nextX - gridOrigin.x) / direction.x : INFINITY;
    float tMaxY = direction.y != 0 ? (nextY - gridOrigin.y) / direction.y : INFINITY;
    float tMaxZ = direction.z != 0 ? (nextZ - gridOrigin.z) / direction.z : INFINITY;

    float distance = 0.0f;
    Vector3 lastNormal = { 0 };

    auto TryHit = [&](int bx, int by, int bz, Vector3 normal) -> bool
        {
            if (!IsBlockAt({ (float)bx, (float)by, (float)bz })) return false;
            hit.hit = true;
            hit.blockPosition = { (float)bx, (float)by, (float)bz };
            hit.normal = normal;
            auto it = blockOccupancy.find({ bx, by, bz });
            if (it != blockOccupancy.end()) hit.blockType = it->second;
            return true;
        };

    if (TryHit(x, y, z, { 0, 0, 0 }))
    {
        if (fabsf(direction.x) >= fabsf(direction.y) && fabsf(direction.x) >= fabsf(direction.z))
            hit.normal = { direction.x >= 0 ? -1.0f : 1.0f, 0, 0 };
        else if (fabsf(direction.y) >= fabsf(direction.z))
            hit.normal = { 0, direction.y >= 0 ? -1.0f : 1.0f, 0 };
        else
            hit.normal = { 0, 0, direction.z >= 0 ? -1.0f : 1.0f };
        return hit;
    }

    while (distance < maxDistance)
    {
        if (tMaxX < tMaxY)
        {
            if (tMaxX < tMaxZ)
            {
                x += stepX; distance = tMaxX; tMaxX += tDeltaX;
                lastNormal = { (float)-stepX, 0, 0 };
            }
            else
            {
                z += stepZ; distance = tMaxZ; tMaxZ += tDeltaZ;
                lastNormal = { 0, 0, (float)-stepZ };
            }
        }
        else
        {
            if (tMaxY < tMaxZ)
            {
                y += stepY; distance = tMaxY; tMaxY += tDeltaY;
                lastNormal = { 0, (float)-stepY, 0 };
            }
            else
            {
                z += stepZ; distance = tMaxZ; tMaxZ += tDeltaZ;
                lastNormal = { 0, 0, (float)-stepZ };
            }
        }
        if (distance > maxDistance) break;
        if (TryHit(x, y, z, lastNormal)) return hit;
    }
    return hit;
}

// =====================================================
// REBUILD – queues CPU mesh jobs (main thread only starts them)
// =====================================================

// Wait until all 4 orthogonal neighbours exist so border faces cull correctly once.
static bool ChunkHasAllNeighbors(int cx, int cz)
{
    return GetChunkAt(cx - 1, cz) != nullptr
        && GetChunkAt(cx + 1, cz) != nullptr
        && GetChunkAt(cx, cz - 1) != nullptr
        && GetChunkAt(cx, cz + 1) != nullptr;
}

void RebuildChunk(Chunk& chunk)
{
    if (!chunk.meshing)
        QueueMeshJob(chunk);
}

void RebuildAllDirtyChunks(const Camera3D& camera)
{
    const int maxQueue = 2;
    int queued = 0;

    // Pass 1: first-time meshes — only in view + neighbours ready
    for (auto& pair : chunks)
    {
        if (queued >= maxQueue) break;
        Chunk& c = pair.second;
        if (c.meshing) continue;
        if (c.mesh.vertexCount > 0) continue;
        if (!IsChunkInView(c, camera)) continue;
        if (!ChunkHasAllNeighbors(c.x, c.z)) continue;
        if (!c.dirty && c.generated) c.dirty = true;
        if (!c.dirty) continue;
        QueueMeshJob(c);
        queued++;
    }

    // Pass 2: remeshes (place/break) — only if still in view
    for (auto& pair : chunks)
    {
        if (queued >= maxQueue) break;
        Chunk& c = pair.second;
        if (!c.dirty || c.meshing) continue;
        if (c.mesh.vertexCount == 0) continue;
        if (!IsChunkInView(c, camera)) continue;
        QueueMeshJob(c);
        queued++;
    }

    ProcessCompletedMeshes();
}

bool IsChunkVisible(const Chunk& chunk, const Camera3D& camera)
{
    return IsChunkInView(chunk, camera);
}

void DrawWorldChunks(Material material, const Camera3D& camera)
{
    for (auto& pair : chunks)
    {
        Chunk& chunk = pair.second;
        if (chunk.mesh.vertexCount <= 0) continue;
        if (!IsChunkVisible(chunk, camera)) continue;
        DrawMesh(chunk.mesh, material, MatrixIdentity());
    }
}

void UnloadWorldChunks()
{
    for (auto& pair : chunks)
    {
        if (pair.second.mesh.vertexCount > 0)
            UnloadMesh(pair.second.mesh);
        pair.second.mesh = { 0 };
    }
    chunks.clear();
    blockOccupancy.clear();
}

void DrawChunkBorders()
{
    const float h = 1000.0f;
    for (const auto& pair : chunks)
    {
        const Chunk& c = pair.second;
        float minX = c.x * CHUNK_SIZE - 0.5f;
        float maxX = (c.x + 1) * CHUNK_SIZE - 0.5f;
        float minZ = c.z * CHUNK_SIZE - 0.5f;
        float maxZ = (c.z + 1) * CHUNK_SIZE - 0.5f;
        DrawLine3D({ minX, -h, minZ }, { minX, h, minZ }, RED);
        DrawLine3D({ maxX, -h, minZ }, { maxX, h, minZ }, RED);
        DrawLine3D({ minX, -h, maxZ }, { minX, h, maxZ }, RED);
        DrawLine3D({ maxX, -h, maxZ }, { maxX, h, maxZ }, RED);
    }
}

// =====================================================
// DEBUG
// =====================================================

int GetLoadedChunkCount() { return (int)chunks.size(); }

int GetPendingChunkJobs()
{
    std::lock_guard<std::mutex> lock(g_terrainMutex);
    // pending + in-flight only (not already-applied scheduled)
    return (int)g_terrainPending.size() + (int)g_terrainInFlight.size();
}

int GetPendingMeshJobs()
{
    std::lock_guard<std::mutex> lock(g_meshMutex);
    return (int)g_meshPending.size() + (int)g_meshInFlight.size();
}

int GetWorldModCount()
{
    int total = 0;
    for (const auto& p : chunkMods) total += (int)p.second.size();
    return total;
}

int GetDirtyChunkCount()
{
    int n = 0;
    for (const auto& p : chunks)
        if (p.second.dirty || p.second.meshing) n++;
    return n;
}

size_t GetTotalBlockCount()
{
    size_t n = 0;
    for (const auto& p : chunks) n += p.second.blocks.size();
    return n;
}
