#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "World.h"
#include "Block.h"
#include <math.h>
#include <string>
#include <vector>

//#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

static const std::vector<std::string> PLACEABLE_BLOCKS = {
    "cobble_stone",
    "dirt",
    "grass",
    "stone",
    "bedrock"
};

int main(void)
{
    const int screenWidth = 1280;
    const int screenHeight = 720;

    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(screenWidth, screenHeight, "Raylib Minecraft Clone");

    rlEnableBackfaceCulling();

    SetupWorld();

    Shader shader = LoadShader(
        "shaders/lighting.vert",
        "shaders/lighting.frag"
    );

    Texture2D atlas = LoadTexture("generated/blocks_atlas.png");
    GenTextureMipmaps(&atlas);
    SetTextureFilter(atlas, TEXTURE_FILTER_POINT);

    Material material = LoadMaterialDefault();
    material.shader = shader;
    material.maps[MATERIAL_MAP_DIFFUSE].texture = atlas;

    Camera3D camera = { 0 };
    camera.position = { 0.0f, 20.0f, 4.0f };
    camera.target = { 0.0f, 20.0f, 0.0f };
    camera.up = { 0.0f, 1.0f, 0.0f };
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    RebuildAllDirtyChunks(camera);

    DisableCursor();

    float cameraAngleX = -1.5708f;
    float cameraAngleY = 0.0f;
    const float mouseSensitivity = 0.002f;

    SetMousePosition(GetScreenWidth() / 2, GetScreenHeight() / 2);

    int selectedBlock = 0;
    bool showChunkBorders = false;
    bool showDebug = true;
    bool showHelp = false;

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();

        // =================================================
        // MOUSE LOOK
        // =================================================
        int centerX = GetScreenWidth() / 2;
        int centerY = GetScreenHeight() / 2;
        Vector2 mousePos = GetMousePosition();
        float deltaX = mousePos.x - centerX;
        float deltaY = mousePos.y - centerY;

        if (deltaX != 0.0f || deltaY != 0.0f)
        {
            cameraAngleX -= deltaX * mouseSensitivity;
            cameraAngleY += deltaY * mouseSensitivity;
        }
        SetMousePosition(centerX, centerY);

        if (cameraAngleY > 1.48f) cameraAngleY = 1.48f;
        if (cameraAngleY < -1.48f) cameraAngleY = -1.48f;

        Vector3 forward = {
            cosf(cameraAngleX) * -cosf(cameraAngleY),
            -sinf(cameraAngleY),
            sinf(cameraAngleX) * cosf(cameraAngleY)
        };
        forward = Vector3Normalize(forward);

        Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));

        // =================================================
        // MOVEMENT
        // =================================================
        float moveSpeed = 10.0f * dt;
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
            moveSpeed *= 5.0f;

        if (IsKeyDown(KEY_W))
            camera.position = Vector3Add(camera.position, Vector3Scale(forward, moveSpeed));
        if (IsKeyDown(KEY_S))
            camera.position = Vector3Subtract(camera.position, Vector3Scale(forward, moveSpeed));
        if (IsKeyDown(KEY_A))
            camera.position = Vector3Subtract(camera.position, Vector3Scale(right, moveSpeed));
        if (IsKeyDown(KEY_D))
            camera.position = Vector3Add(camera.position, Vector3Scale(right, moveSpeed));
        if (IsKeyDown(KEY_SPACE))
            camera.position.y += moveSpeed;
        if (IsKeyDown(KEY_LEFT_CONTROL))
            camera.position.y -= moveSpeed;

        camera.target = Vector3Add(camera.position, forward);

        // =================================================
        // HOTBAR / BLOCK SELECTION
        // =================================================
        if (IsKeyPressed(KEY_ONE)) selectedBlock = 0;
        if (IsKeyPressed(KEY_TWO)) selectedBlock = 1;
        if (IsKeyPressed(KEY_THREE)) selectedBlock = 2;
        if (IsKeyPressed(KEY_FOUR)) selectedBlock = 3;
        if (IsKeyPressed(KEY_FIVE)) selectedBlock = 4;

        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
        {
            selectedBlock += (wheel > 0.0f) ? -1 : 1;
            if (selectedBlock < 0)
                selectedBlock = (int)PLACEABLE_BLOCKS.size() - 1;
            if (selectedBlock >= (int)PLACEABLE_BLOCKS.size())
                selectedBlock = 0;
        }

        // =================================================
        // RENDER DISTANCE
        // =================================================
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
            SetRenderDistance(GetRenderDistance() + 1);
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
            SetRenderDistance(GetRenderDistance() - 1);

        // =================================================
        // DEBUG TOGGLES
        // =================================================
        if (IsKeyPressed(KEY_F3))
            showDebug = !showDebug;
        if (IsKeyPressed(KEY_F4))
            showChunkBorders = !showChunkBorders;
        if (IsKeyPressed(KEY_H))
            showHelp = !showHelp;

        // =================================================
        // WORLD UPDATE
        // =================================================
        UpdateWorldChunks(camera);

        SetWindowTitle(TextFormat(
            "Raylib Minecraft Clone | FPS: %d | Chunks: %d | RD: %d | Block: %s",
            GetFPS(),
            GetLoadedChunkCount(),
            GetRenderDistance(),
            PLACEABLE_BLOCKS[selectedBlock].c_str()
        ));

        // =================================================
        // RAYCAST + PLACE / BREAK
        // =================================================
        RaycastHit hit = RaycastBlock(camera.position, forward, 10.0f);

        if (hit.hit)
        {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                DestroyBlockAt(hit.blockPosition);

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            {
                Vector3 newPosition = Vector3Add(hit.blockPosition, hit.normal);
                if (!IsBlockAt(newPosition))
                    AddBlock(newPosition, PLACEABLE_BLOCKS[selectedBlock]);
            }
        }

        RebuildAllDirtyChunks(camera);

        // =================================================
        // DRAW
        // =================================================
        BeginDrawing();
        ClearBackground(SKYBLUE);

        BeginMode3D(camera);
        DrawWorldChunks(material, camera);

        if (showChunkBorders)
            DrawChunkBorders();

        if (hit.hit)
        {
            DrawCubeWires(hit.blockPosition, 1.02f, 1.02f, 1.02f, BLACK);
        }
        EndMode3D();

        // Crosshair
        DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 3, WHITE);

        // Hotbar
        {
            const int slotSize = 40;
            const int gap = 6;
            const int count = (int)PLACEABLE_BLOCKS.size();
            const int barWidth = count * slotSize + (count - 1) * gap;
            const int barX = (GetScreenWidth() - barWidth) / 2;
            const int barY = GetScreenHeight() - slotSize - 16;

            for (int i = 0; i < count; i++)
            {
                int x = barX + i * (slotSize + gap);
                Color bg = (i == selectedBlock) ? Color{ 80, 80, 80, 220 } : Color{ 30, 30, 30, 180 };
                Color border = (i == selectedBlock) ? YELLOW : GRAY;
                DrawRectangle(x, barY, slotSize, slotSize, bg);
                DrawRectangleLines(x, barY, slotSize, slotSize, border);
                DrawText(TextFormat("%d", i + 1), x + 4, barY + 2, 12, LIGHTGRAY);

                // Short name
                const char* name = PLACEABLE_BLOCKS[i].c_str();
                DrawText(name, x + 4, barY + slotSize - 14, 10, WHITE);
            }
        }

        // Debug overlay (F3)
        if (showDebug)
        {
            int y = 10;
            const int line = 18;
            DrawRectangle(8, 8, 360, 160, Color{ 0, 0, 0, 140 });
            DrawText(TextFormat("FPS: %d", GetFPS()), 16, y, 16, LIME); y += line;
            DrawText(TextFormat("Pos: %.1f  %.1f  %.1f",
                camera.position.x, camera.position.y, camera.position.z), 16, y, 16, WHITE); y += line;
            DrawText(TextFormat("Chunk: %d, %d",
                (int)floorf(camera.position.x / CHUNK_SIZE),
                (int)floorf(camera.position.z / CHUNK_SIZE)), 16, y, 16, WHITE); y += line;
            DrawText(TextFormat("Loaded chunks: %d  Dirty: %d",
                GetLoadedChunkCount(), GetDirtyChunkCount()), 16, y, 16, WHITE); y += line;
            DrawText(TextFormat("Pending terrain: %d  mesh: %d",
                GetPendingChunkJobs(), GetPendingMeshJobs()), 16, y, 16, WHITE); y += line;
            DrawText(TextFormat("Blocks loaded: %zu  Mods: %d",
                GetTotalBlockCount(), GetWorldModCount()), 16, y, 16, WHITE); y += line;
            DrawText(TextFormat("Render distance: %d  (+/- to change)",
                GetRenderDistance()), 16, y, 16, SKYBLUE); y += line;
            DrawText(TextFormat("Selected: %s", PLACEABLE_BLOCKS[selectedBlock].c_str()), 16, y, 16, YELLOW);
        }

        // Help (H)
        if (showHelp)
        {
            int panelW = 420;
            int panelH = 220;
            int px = GetScreenWidth() - panelW - 12;
            int py = 12;
            DrawRectangle(px, py, panelW, panelH, Color{ 0, 0, 0, 180 });
            int y = py + 10;
            int line = 18;
            DrawText("Controls", px + 12, y, 18, YELLOW); y += line + 4;
            DrawText("WASD / Space / Ctrl  Move", px + 12, y, 16, WHITE); y += line;
            DrawText("Mouse                  Look", px + 12, y, 16, WHITE); y += line;
            DrawText("LMB / RMB              Break / Place", px + 12, y, 16, WHITE); y += line;
            DrawText("1-5 / Scroll           Select block", px + 12, y, 16, WHITE); y += line;
            DrawText("+ / -                  Render distance", px + 12, y, 16, WHITE); y += line;
            DrawText("Shift                  Sprint", px + 12, y, 16, WHITE); y += line;
            DrawText("F3                     Debug overlay", px + 12, y, 16, WHITE); y += line;
            DrawText("F4                     Chunk borders", px + 12, y, 16, WHITE); y += line;
            DrawText("H                      Toggle this help", px + 12, y, 16, WHITE);
        }
        else
        {
            DrawText("H = help", 10, GetScreenHeight() - 24, 16, Color{ 255, 255, 255, 160 });
        }

        EndDrawing();
    }

    EnableCursor();
    ShutdownWorld();
    UnloadShader(shader);
    UnloadTexture(atlas);
    CloseWindow();
    return 0;
}
