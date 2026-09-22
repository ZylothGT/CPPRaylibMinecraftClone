#include "World.h"
#include "raymath.h"
#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_set>

using json = nlohmann::json;

struct BlockKey
{
    int x;
    int y;
    int z;

    bool operator==(const BlockKey& other) const
    {
        return x == other.x &&
            y == other.y &&
            z == other.z;
    }
};

struct BlockKeyHash
{
    size_t operator()(const BlockKey& p) const
    {
        size_t h1 = std::hash<int>{}(p.x);
        size_t h2 = std::hash<int>{}(p.y);
        size_t h3 = std::hash<int>{}(p.z);

        return h1 ^
            (h2 << 1) ^
            (h3 << 2);
    }
};

std::vector<Block> blocks;
std::vector<Chunk> chunks;
std::unordered_map<BlockKey, Block*, BlockKeyHash> blockLookup;

// =====================================================
// TEXTURE DATA
// =====================================================

struct FaceUV
{
    float u0;
    float v0;
    float u1;
    float v1;
};

struct BlockTextures
{
    FaceUV top;
    FaceUV bottom;
    FaceUV north;
    FaceUV south;
    FaceUV east;
    FaceUV west;
};

std::unordered_map<std::string, BlockTextures> blockTextures;


// =====================================================
// LOAD BLOCK TEXTURES
// =====================================================

bool LoadBlockTextures()
{
    blockTextures.clear();

    const std::string blockDirectory = "generated/blocks/";

    for (const auto& entry : std::filesystem::directory_iterator(blockDirectory))
    {
        if (!entry.is_regular_file())
            continue;

        if (entry.path().extension() != ".json")
            continue;

        std::string blockName = entry.path().stem().string();
        std::string path = entry.path().string();

        std::ifstream file(path);

        if (!file.is_open())
            continue;

        try
        {
            json data;
            file >> data;

            BlockTextures textures{};

            auto ReadFace = [&](const char* face) -> FaceUV
                {
                    auto uv = data["textures"][face]["uv"];

                    return {
                        uv["u0"].get<float>(),
                        uv["v0"].get<float>(),
                        uv["u1"].get<float>(),
                        uv["v1"].get<float>()
                    };
                };

            textures.top = ReadFace("top");
            textures.bottom = ReadFace("bottom");
            textures.north = ReadFace("north");
            textures.south = ReadFace("south");
            textures.east = ReadFace("east");
            textures.west = ReadFace("west");

            blockTextures[blockName] = textures;

            TraceLog(
                LOG_INFO,
                "Loaded block textures: %s",
                blockName.c_str()
            );
        }
        catch (const std::exception& error)
        {
            TraceLog(
                LOG_ERROR,
                "Failed to parse %s: %s",
                path.c_str(),
                error.what()
            );
        }
    }

    return !blockTextures.empty();
}


// =====================================================
// CHUNK COORDINATES
// =====================================================

int WorldToChunk(float position)
{
    return (int)floorf(position / (float)CHUNK_SIZE);
}


int GetLocalBlockCoordinate(float position)
{
    int coordinate = (int)floorf(position);

    int local = coordinate % CHUNK_SIZE;

    if (local < 0)
        local += CHUNK_SIZE;

    return local;
}


// =====================================================
// GET CHUNK
// =====================================================

Chunk* GetChunkAt(int chunkX, int chunkY, int chunkZ)
{
    for (Chunk& chunk : chunks)
    {
        if (chunk.x == chunkX &&
            chunk.y == chunkY &&
            chunk.z == chunkZ)
        {
            return &chunk;
        }
    }

    return nullptr;
}


Chunk* GetChunkContainingBlock(Vector3 position)
{
    int chunkX = WorldToChunk(position.x);
    int chunkY = WorldToChunk(position.y);
    int chunkZ = WorldToChunk(position.z);

    return GetChunkAt(chunkX, chunkY, chunkZ);
}


// =====================================================
// CREATE CHUNKS
// =====================================================

void CreateChunks()
{
    chunks.clear();

    if (blocks.empty())
        return;

    // Find the chunk coordinates that contain the existing blocks.
    int minChunkX = INT_MAX;
    int maxChunkX = INT_MIN;

    int minChunkY = INT_MAX;
    int maxChunkY = INT_MIN;

    int minChunkZ = INT_MAX;
    int maxChunkZ = INT_MIN;

    for (const Block& block : blocks)
    {
        int chunkX = WorldToChunk(block.position.x);
        int chunkY = WorldToChunk(block.position.y);
        int chunkZ = WorldToChunk(block.position.z);

        minChunkX = std::min(minChunkX, chunkX);
        maxChunkX = std::max(maxChunkX, chunkX);

        minChunkY = std::min(minChunkY, chunkY);
        maxChunkY = std::max(maxChunkY, chunkY);

        minChunkZ = std::min(minChunkZ, chunkZ);
        maxChunkZ = std::max(maxChunkZ, chunkZ);
    }

    for (int chunkX = minChunkX; chunkX <= maxChunkX; chunkX++)
    {
        for (int chunkY = minChunkY; chunkY <= maxChunkY; chunkY++)
        {
            for (int chunkZ = minChunkZ; chunkZ <= maxChunkZ; chunkZ++)
            {
                Chunk chunk{};

                chunk.x = chunkX;
                chunk.y = chunkY;
                chunk.z = chunkZ;

                chunk.mesh = { 0 };
                chunk.dirty = true;

                chunks.push_back(chunk);
            }
        }
    }
}

// =====================================================
// TERRAIN NOISE
// =====================================================

static float Hash2D(int x, int z)
{
    uint32_t h =
        static_cast<uint32_t>(x) * 374761393u +
        static_cast<uint32_t>(z) * 668265263u;

    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;

    return static_cast<float>(h & 0xFFFFFF) / 16777215.0f;
}


static float SmoothStep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}


static float ValueNoise(float x, float z)
{
    int x0 = static_cast<int>(floorf(x));
    int z0 = static_cast<int>(floorf(z));

    int x1 = x0 + 1;
    int z1 = z0 + 1;

    float tx = x - static_cast<float>(x0);
    float tz = z - static_cast<float>(z0);

    tx = SmoothStep(tx);
    tz = SmoothStep(tz);

    float a = Hash2D(x0, z0);
    float b = Hash2D(x1, z0);
    float c = Hash2D(x0, z1);
    float d = Hash2D(x1, z1);

    float ab = a + (b - a) * tx;
    float cd = c + (d - c) * tx;

    return ab + (cd - ab) * tz;
}


// =====================================================
// FRACTAL TERRAIN NOISE
// =====================================================

static float TerrainNoise(float x, float z)
{
    float total = 0.0f;

    float amplitude = 1.0f;
    float frequency = 1.0f;

    float maxAmplitude = 0.0f;

    const int octaves = 5;

    for (int i = 0; i < octaves; i++)
    {
        total += ValueNoise(
            x * frequency,
            z * frequency
        ) * amplitude;

        maxAmplitude += amplitude;

        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return total / maxAmplitude;
}

// =====================================================
// GENERATE TERRAIN
// =====================================================

void GenerateTerrain(
    int minX,
    int maxX,
    int minZ,
    int maxZ)
{
    const float noiseScale = 0.025f;

    const int baseHeight = 0;
    const int heightVariation = 12;

    const int dirtDepth = 3;

    for (int x = minX; x <= maxX; x++)
    {
        for (int z = minZ; z <= maxZ; z++)
        {
            float noise = TerrainNoise(
                x * noiseScale,
                z * noiseScale
            );

            int height =
                baseHeight +
                static_cast<int>(
                    noise * heightVariation
                    );

            // Make sure there is always terrain.
            height = std::max(height, -2);

            for (int y = -2; y <= height; y++)
            {
                std::string blockType;

                if (y == height)
                {
                    // Grass
                    blockType = "grass";
                }
                else if (y >= height - dirtDepth)
                {
                    // Dirt
                    blockType = "dirt";
                }
                else
                {
                    // Stone
                    blockType = "stone";
                }

                blocks.push_back({
                    {
                        static_cast<float>(x),
                        static_cast<float>(y),
                        static_cast<float>(z)
                    },
                    blockType
                    });
            }
        }
    }
}

void BuildBlockLookup()
{
    blockLookup.clear();

    for (Block& block : blocks)
    {
        blockLookup[{
            (int)block.position.x,
                (int)block.position.y,
                (int)block.position.z
        }] = &block;
    }
}

// =====================================================
// SETUP WORLD
// =====================================================

void SetupWorld()
{
    blocks.clear();

    LoadBlockTextures();

    // =====================================================
    // TERRAIN
    // =====================================================

    GenerateTerrain(
        -100,
        100,
        -100,
        100
    );

    BuildBlockLookup();

    CreateChunks();
}


// =====================================================
// GET BLOCK
// =====================================================

Block* GetBlockAt(Vector3 position)
{
    BlockKey key{
        (int)floorf(position.x),
        (int)floorf(position.y),
        (int)floorf(position.z)
    };

    auto it = blockLookup.find(key);

    if (it == blockLookup.end())
        return nullptr;

    return it->second;
}


// =====================================================
// CHECK BLOCK
// =====================================================

bool IsBlockAt(Vector3 position)
{
    return blockLookup.find({
        (int)position.x,
        (int)position.y,
        (int)position.z
        }) != blockLookup.end();
}


// =====================================================
// DESTROY BLOCK
// =====================================================

void DestroyBlock(Block* block)
{
    if (block == nullptr)
        return;

    Vector3 position = block->position;

    BlockKey key{
        (int)position.x,
        (int)position.y,
        (int)position.z
    };

    auto lookupIt = blockLookup.find(key);

    if (lookupIt == blockLookup.end())
        return;

    // Find the vector index of this block.
    size_t index = block - blocks.data();

    // Remove it from the lookup.
    blockLookup.erase(lookupIt);

    // If this isn't the last block, move the last block here.
    if (index != blocks.size() - 1)
    {
        blocks[index] = std::move(blocks.back());

        Block& movedBlock = blocks[index];

        blockLookup[{
            (int)movedBlock.position.x,
                (int)movedBlock.position.y,
                (int)movedBlock.position.z
        }] = &movedBlock;
    }

    blocks.pop_back();

    MarkChunkDirty(position);
    MarkNeighborChunksDirty(position);
}


// =====================================================
// MARK CHUNK DIRTY
// =====================================================

void MarkChunkDirty(Vector3 position)
{
    Chunk* chunk = GetChunkContainingBlock(position);

    if (chunk != nullptr)
    {
        chunk->dirty = true;
    }
}


// =====================================================
// MARK NEIGHBOR CHUNKS DIRTY
// =====================================================

void MarkNeighborChunksDirty(Vector3 position)
{
    int localX = GetLocalBlockCoordinate(position.x);
    int localY = GetLocalBlockCoordinate(position.y);
    int localZ = GetLocalBlockCoordinate(position.z);

    Chunk* chunk = GetChunkContainingBlock(position);

    if (chunk == nullptr)
        return;


    if (localX == 0)
    {
        Chunk* neighbor =
            GetChunkAt(chunk->x - 1, chunk->y, chunk->z);

        if (neighbor != nullptr)
            neighbor->dirty = true;
    }

    if (localX == CHUNK_SIZE - 1)
    {
        Chunk* neighbor =
            GetChunkAt(chunk->x + 1, chunk->y, chunk->z);

        if (neighbor != nullptr)
            neighbor->dirty = true;
    }


    if (localY == 0)
    {
        Chunk* neighbor =
            GetChunkAt(chunk->x, chunk->y - 1, chunk->z);

        if (neighbor != nullptr)
            neighbor->dirty = true;
    }

    if (localY == CHUNK_SIZE - 1)
    {
        Chunk* neighbor =
            GetChunkAt(chunk->x, chunk->y + 1, chunk->z);

        if (neighbor != nullptr)
            neighbor->dirty = true;
    }


    if (localZ == 0)
    {
        Chunk* neighbor =
            GetChunkAt(chunk->x, chunk->y, chunk->z - 1);

        if (neighbor != nullptr)
            neighbor->dirty = true;
    }

    if (localZ == CHUNK_SIZE - 1)
    {
        Chunk* neighbor =
            GetChunkAt(chunk->x, chunk->y, chunk->z + 1);

        if (neighbor != nullptr)
            neighbor->dirty = true;
    }
}


// =====================================================
// RAYCAST
// =====================================================

// =====================================================
// RAYCAST
// =====================================================

RaycastHit RaycastBlock(
    Vector3 origin,
    Vector3 direction,
    float maxDistance
)
{
    RaycastHit hit{};
    hit.block = nullptr;
    hit.normal = { 0.0f, 0.0f, 0.0f };

    direction = Vector3Normalize(direction);

    if (Vector3Length(direction) <= 0.0f)
        return hit;

    // -------------------------------------------------
    // SHIFT INTO THE BLOCK GRID
    //
    // A block at (0,0,0) occupies:
    // -0.5 -> +0.5
    //
    // So we shift the ray by +0.5 before doing DDA.
    // -------------------------------------------------

    Vector3 gridOrigin = {
        origin.x + 0.5f,
        origin.y + 0.5f,
        origin.z + 0.5f
    };

    int x = (int)floorf(gridOrigin.x);
    int y = (int)floorf(gridOrigin.y);
    int z = (int)floorf(gridOrigin.z);

    int stepX = direction.x >= 0.0f ? 1 : -1;
    int stepY = direction.y >= 0.0f ? 1 : -1;
    int stepZ = direction.z >= 0.0f ? 1 : -1;

    float tDeltaX =
        direction.x != 0.0f
        ? fabsf(1.0f / direction.x)
        : INFINITY;

    float tDeltaY =
        direction.y != 0.0f
        ? fabsf(1.0f / direction.y)
        : INFINITY;

    float tDeltaZ =
        direction.z != 0.0f
        ? fabsf(1.0f / direction.z)
        : INFINITY;

    // -------------------------------------------------
    // FIRST GRID BOUNDARIES
    // -------------------------------------------------

    float nextBoundaryX =
        direction.x >= 0.0f
        ? (float)(x + 1)
        : (float)x;

    float nextBoundaryY =
        direction.y >= 0.0f
        ? (float)(y + 1)
        : (float)y;

    float nextBoundaryZ =
        direction.z >= 0.0f
        ? (float)(z + 1)
        : (float)z;

    float tMaxX =
        direction.x != 0.0f
        ? (nextBoundaryX - gridOrigin.x) / direction.x
        : INFINITY;

    float tMaxY =
        direction.y != 0.0f
        ? (nextBoundaryY - gridOrigin.y) / direction.y
        : INFINITY;

    float tMaxZ =
        direction.z != 0.0f
        ? (nextBoundaryZ - gridOrigin.z) / direction.z
        : INFINITY;


    // -------------------------------------------------
    // CHECK CURRENT BLOCK
    // -------------------------------------------------

    auto CheckCurrentBlock = [&]() -> Block*
        {
            BlockKey key{
                x,
                y,
                z
            };

            auto it = blockLookup.find(key);

            if (it == blockLookup.end())
                return nullptr;

            return it->second;
        };


    // -------------------------------------------------
    // STARTING BLOCK
    // -------------------------------------------------

    Block* block = CheckCurrentBlock();

    if (block != nullptr)
    {
        hit.block = block;

        // We are already inside a block.
        // Pick the dominant direction as the placement side.
        if (fabsf(direction.x) >= fabsf(direction.y) &&
            fabsf(direction.x) >= fabsf(direction.z))
        {
            hit.normal = {
                direction.x >= 0.0f ? -1.0f : 1.0f,
                0.0f,
                0.0f
            };
        }
        else if (fabsf(direction.y) >= fabsf(direction.z))
        {
            hit.normal = {
                0.0f,
                direction.y >= 0.0f ? -1.0f : 1.0f,
                0.0f
            };
        }
        else
        {
            hit.normal = {
                0.0f,
                0.0f,
                direction.z >= 0.0f ? -1.0f : 1.0f
            };
        }

        return hit;
    }


    // -------------------------------------------------
    // DDA
    // -------------------------------------------------

    while (true)
    {
        // ---------------------------------------------
        // X
        // ---------------------------------------------

        if (tMaxX < tMaxY && tMaxX < tMaxZ)
        {
            if (tMaxX > maxDistance)
                break;

            x += stepX;
            tMaxX += tDeltaX;

            hit.normal = {
                (float)-stepX,
                0.0f,
                0.0f
            };
        }

        // ---------------------------------------------
        // Y
        // ---------------------------------------------

        else if (tMaxY < tMaxZ)
        {
            if (tMaxY > maxDistance)
                break;

            y += stepY;
            tMaxY += tDeltaY;

            hit.normal = {
                0.0f,
                (float)-stepY,
                0.0f
            };
        }

        // ---------------------------------------------
        // Z
        // ---------------------------------------------

        else
        {
            if (tMaxZ > maxDistance)
                break;

            z += stepZ;
            tMaxZ += tDeltaZ;

            hit.normal = {
                0.0f,
                0.0f,
                (float)-stepZ
            };
        }


        // ---------------------------------------------
        // CHECK BLOCK
        // ---------------------------------------------

        BlockKey key{
            x,
            y,
            z
        };

        auto it = blockLookup.find(key);

        if (it != blockLookup.end())
        {
            hit.block = it->second;
            return hit;
        }
    }

    return hit;
}


// =====================================================
// GENERATE CHUNK MESH
// =====================================================

Mesh GenerateChunkMesh(Chunk& chunk)
{
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> texcoords;

    const float s = 0.5f;


    // =====================================================
    // CHUNK WORLD BOUNDS
    // =====================================================

    float minX = chunk.x * CHUNK_SIZE;
    float maxX = minX + CHUNK_SIZE - 1;

    float minY = chunk.y * CHUNK_SIZE;
    float maxY = minY + CHUNK_SIZE - 1;

    float minZ = chunk.z * CHUNK_SIZE;
    float maxZ = minZ + CHUNK_SIZE - 1;


    // =====================================================
    // ADD FACE
    // =====================================================

    auto AddFace =
        [&](Vector3 a,
            Vector3 b,
            Vector3 c,
            Vector3 d,
            Vector3 normal,
            FaceUV uv)
        {
            vertices.push_back(a.x);
            vertices.push_back(a.y);
            vertices.push_back(a.z);

            vertices.push_back(b.x);
            vertices.push_back(b.y);
            vertices.push_back(b.z);

            vertices.push_back(c.x);
            vertices.push_back(c.y);
            vertices.push_back(c.z);

            vertices.push_back(a.x);
            vertices.push_back(a.y);
            vertices.push_back(a.z);

            vertices.push_back(c.x);
            vertices.push_back(c.y);
            vertices.push_back(c.z);

            vertices.push_back(d.x);
            vertices.push_back(d.y);
            vertices.push_back(d.z);


            for (int i = 0; i < 6; i++)
            {
                normals.push_back(normal.x);
                normals.push_back(normal.y);
                normals.push_back(normal.z);
            }


            texcoords.push_back(uv.u0);
            texcoords.push_back(uv.v1);

            texcoords.push_back(uv.u1);
            texcoords.push_back(uv.v1);

            texcoords.push_back(uv.u1);
            texcoords.push_back(uv.v0);

            texcoords.push_back(uv.u0);
            texcoords.push_back(uv.v1);

            texcoords.push_back(uv.u1);
            texcoords.push_back(uv.v0);

            texcoords.push_back(uv.u0);
            texcoords.push_back(uv.v0);
        };


    // =====================================================
    // GENERATE BLOCK FACES
    // =====================================================

    for (const Block& block : blocks)
    {
        Vector3 p = block.position;

        float x = p.x;
        float y = p.y;
        float z = p.z;


        // -------------------------------------------------
        // ONLY GENERATE BLOCKS INSIDE THIS CHUNK
        // -------------------------------------------------

        if (x < minX || x > maxX ||
            y < minY || y > maxY ||
            z < minZ || z > maxZ)
        {
            continue;
        }


        // -------------------------------------------------
        // FIND BLOCK TEXTURE
        // -------------------------------------------------

        auto textureIt =
            blockTextures.find(block.blockType);

        if (textureIt == blockTextures.end())
        {
            TraceLog(
                LOG_WARNING,
                "No textures found for block: %s",
                block.blockType.c_str()
            );

            continue;
        }

        const BlockTextures& textures =
            textureIt->second;


        // =================================================
        // BOTTOM
        // =================================================

        if (!IsBlockAt({ x, y - 1.0f, z }))
        {
            AddFace(
                { x - s, y - s, z - s },
                { x + s, y - s, z - s },
                { x + s, y - s, z + s },
                { x - s, y - s, z + s },
                { 0.0f, -1.0f, 0.0f },
                textures.bottom
            );
        }


        // =================================================
        // TOP
        // =================================================

        if (!IsBlockAt({ x, y + 1.0f, z }))
        {
            AddFace(
                { x - s, y + s, z + s },
                { x + s, y + s, z + s },
                { x + s, y + s, z - s },
                { x - s, y + s, z - s },
                { 0.0f, 1.0f, 0.0f },
                textures.top
            );
        }


        // =================================================
        // FRONT (+Z)
        // =================================================

        if (!IsBlockAt({ x, y, z + 1.0f }))
        {
            AddFace(
                { x - s, y - s, z + s },
                { x + s, y - s, z + s },
                { x + s, y + s, z + s },
                { x - s, y + s, z + s },
                { 0.0f, 0.0f, 1.0f },
                textures.south
            );
        }


        // =================================================
        // BACK (-Z)
        // =================================================

        if (!IsBlockAt({ x, y, z - 1.0f }))
        {
            AddFace(
                { x + s, y - s, z - s },
                { x - s, y - s, z - s },
                { x - s, y + s, z - s },
                { x + s, y + s, z - s },
                { 0.0f, 0.0f, -1.0f },
                textures.north
            );
        }


        // =================================================
        // RIGHT (+X)
        // =================================================

        if (!IsBlockAt({ x + 1.0f, y, z }))
        {
            AddFace(
                { x + s, y - s, z + s },
                { x + s, y - s, z - s },
                { x + s, y + s, z - s },
                { x + s, y + s, z + s },
                { 1.0f, 0.0f, 0.0f },
                textures.east
            );
        }


        // =================================================
        // LEFT (-X)
        // =================================================

        if (!IsBlockAt({ x - 1.0f, y, z }))
        {
            AddFace(
                { x - s, y - s, z - s },
                { x - s, y - s, z + s },
                { x - s, y + s, z + s },
                { x - s, y + s, z - s },
                { -1.0f, 0.0f, 0.0f },
                textures.west
            );
        }
    }


    // =====================================================
    // CREATE RAYLIB MESH
    // =====================================================

    Mesh mesh = { 0 };

    mesh.vertexCount =
        static_cast<int>(vertices.size() / 3);

    mesh.triangleCount =
        mesh.vertexCount / 3;


    // =====================================================
    // VERTICES
    // =====================================================

    if (!vertices.empty())
    {
        mesh.vertices = (float*)MemAlloc(
            vertices.size() * sizeof(float)
        );

        for (size_t i = 0; i < vertices.size(); i++)
        {
            mesh.vertices[i] = vertices[i];
        }
    }


    // =====================================================
    // NORMALS
    // =====================================================

    if (!normals.empty())
    {
        mesh.normals = (float*)MemAlloc(
            normals.size() * sizeof(float)
        );

        for (size_t i = 0; i < normals.size(); i++)
        {
            mesh.normals[i] = normals[i];
        }
    }


    // =====================================================
    // TEXCOORDS
    // =====================================================

    if (!texcoords.empty())
    {
        mesh.texcoords = (float*)MemAlloc(
            texcoords.size() * sizeof(float)
        );

        for (size_t i = 0; i < texcoords.size(); i++)
        {
            mesh.texcoords[i] = texcoords[i];
        }
    }


    // =====================================================
    // UPLOAD TO GPU
    // =====================================================

    if (mesh.vertexCount > 0)
    {
        UploadMesh(&mesh, false);
    }

    return mesh;
}


// =====================================================
// REBUILD CHUNK
// =====================================================

void RebuildChunk(Chunk& chunk)
{
    if (chunk.mesh.vertexCount > 0)
    {
        UnloadMesh(chunk.mesh);
    }

    chunk.mesh = GenerateChunkMesh(chunk);

    chunk.dirty = false;
}


// =====================================================
// REBUILD ALL DIRTY CHUNKS
// =====================================================

void RebuildAllDirtyChunks()
{
    const int maxChunksPerFrame = 2;

    int rebuilt = 0;

    for (Chunk& chunk : chunks)
    {
        if (!chunk.dirty)
            continue;

        RebuildChunk(chunk);

        rebuilt++;

        if (rebuilt >= maxChunksPerFrame)
            break;
    }
}

bool IsChunkVisible(const Chunk& chunk, const Camera3D& camera)
{
    float minX = chunk.x * CHUNK_SIZE - 0.5f;
    float minY = chunk.y * CHUNK_SIZE - 0.5f;
    float minZ = chunk.z * CHUNK_SIZE - 0.5f;

    float maxX = (chunk.x + 1) * CHUNK_SIZE - 0.5f;
    float maxY = (chunk.y + 1) * CHUNK_SIZE - 0.5f;
    float maxZ = (chunk.z + 1) * CHUNK_SIZE - 0.5f;

    BoundingBox bounds = {
        { minX, minY, minZ },
        { maxX, maxY, maxZ }
    };

    Matrix view = GetCameraMatrix(camera);

    Matrix projection = MatrixPerspective(
        camera.fovy * DEG2RAD,
        (float)GetScreenWidth() / GetScreenHeight(),
        0.01f,
        1000.0f
    );

    Matrix viewProjection = MatrixMultiply(view, projection);

    // Extract frustum planes.
    Vector4 planes[6];

    planes[0] = {
        viewProjection.m3 + viewProjection.m0,
        viewProjection.m7 + viewProjection.m4,
        viewProjection.m11 + viewProjection.m8,
        viewProjection.m15 + viewProjection.m12
    };

    planes[1] = {
        viewProjection.m3 - viewProjection.m0,
        viewProjection.m7 - viewProjection.m4,
        viewProjection.m11 - viewProjection.m8,
        viewProjection.m15 - viewProjection.m12
    };

    planes[2] = {
        viewProjection.m3 + viewProjection.m1,
        viewProjection.m7 + viewProjection.m5,
        viewProjection.m11 + viewProjection.m9,
        viewProjection.m15 + viewProjection.m13
    };

    planes[3] = {
        viewProjection.m3 - viewProjection.m1,
        viewProjection.m7 - viewProjection.m5,
        viewProjection.m11 - viewProjection.m9,
        viewProjection.m15 - viewProjection.m13
    };

    planes[4] = {
        viewProjection.m3 + viewProjection.m2,
        viewProjection.m7 + viewProjection.m6,
        viewProjection.m11 + viewProjection.m10,
        viewProjection.m15 + viewProjection.m14
    };

    planes[5] = {
        viewProjection.m3 - viewProjection.m2,
        viewProjection.m7 - viewProjection.m6,
        viewProjection.m11 - viewProjection.m10,
        viewProjection.m15 - viewProjection.m14
    };

    for (Vector4& plane : planes)
    {
        float length = sqrtf(
            plane.x * plane.x +
            plane.y * plane.y +
            plane.z * plane.z
        );

        if (length > 0.0f)
        {
            plane.x /= length;
            plane.y /= length;
            plane.z /= length;
            plane.w /= length;
        }

        // Find the corner furthest in the plane's direction.
        Vector3 positive = {
            plane.x >= 0.0f ? bounds.max.x : bounds.min.x,
            plane.y >= 0.0f ? bounds.max.y : bounds.min.y,
            plane.z >= 0.0f ? bounds.max.z : bounds.min.z
        };

        float distance =
            plane.x * positive.x +
            plane.y * positive.y +
            plane.z * positive.z +
            plane.w;

        if (distance < 0.0f)
            return false;
    }

    return true;
}

// =====================================================
// DRAW WORLD CHUNKS
// =====================================================

void DrawWorldChunks(
    Material material,
    const Camera3D& camera
)
{
    for (Chunk& chunk : chunks)
    {
        if (chunk.mesh.vertexCount <= 0)
            continue;

        if (!IsChunkVisible(chunk, camera))
            continue;

        DrawMesh(
            chunk.mesh,
            material,
            MatrixIdentity()
        );
    }
}


// =====================================================
// UNLOAD WORLD CHUNKS
// =====================================================

void UnloadWorldChunks()
{
    for (Chunk& chunk : chunks)
    {
        if (chunk.mesh.vertexCount > 0)
        {
            UnloadMesh(chunk.mesh);
        }

        chunk.mesh = { 0 };
    }

    chunks.clear();
}

void DrawChunkBorders()
{
    const float borderHeight = 1000.0f;

    for (const Chunk& chunk : chunks)
    {
        float minX = chunk.x * CHUNK_SIZE - 0.5f;
        float maxX = (chunk.x + 1) * CHUNK_SIZE - 0.5f;

        float minZ = chunk.z * CHUNK_SIZE - 0.5f;
        float maxZ = (chunk.z + 1) * CHUNK_SIZE - 0.5f;

        DrawLine3D(
            { minX, -borderHeight, minZ },
            { minX,  borderHeight, minZ },
            RED
        );

        DrawLine3D(
            { maxX, -borderHeight, minZ },
            { maxX,  borderHeight, minZ },
            RED
        );

        DrawLine3D(
            { minX, -borderHeight, maxZ },
            { minX,  borderHeight, maxZ },
            RED
        );

        DrawLine3D(
            { maxX, -borderHeight, maxZ },
            { maxX,  borderHeight, maxZ },
            RED
        );
    }
}

void AddBlock(Vector3 position, const std::string& blockType)
{
    Block* oldData = blocks.empty() ? nullptr : blocks.data();

    blocks.push_back({
        position,
        blockType
        });

    // push_back can reallocate the vector,
    // which invalidates every Block*.
    if (blocks.data() != oldData)
    {
        BuildBlockLookup();
    }
    else
    {
        Block* block = &blocks.back();

        blockLookup[{
            (int)position.x,
                (int)position.y,
                (int)position.z
        }] = block;
    }

    MarkChunkDirty(position);
    MarkNeighborChunksDirty(position);
}