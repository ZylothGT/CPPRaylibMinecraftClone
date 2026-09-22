#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "World.h"
#include "Block.h"
#include <math.h>

bool showChunkBorders = false;

//#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

int main(void)
{
    const int screenWidth = 1280;
    const int screenHeight = 720;

    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(screenWidth, screenHeight, "Raylib Minecraft Clone");

    rlEnableBackfaceCulling();

    // =====================================================
    // WORLD
    // =====================================================

    SetupWorld();
    RebuildAllDirtyChunks();


    // =====================================================
    // SHADER
    // =====================================================

    Shader shader = LoadShader(
        "shaders/lighting.vert",
        "shaders/lighting.frag"
    );


    // =====================================================
    // TEXTURE ATLAS
    // =====================================================

    Texture2D atlas = LoadTexture(
        "generated/blocks_atlas.png"
    );

    GenTextureMipmaps(&atlas);
    SetTextureFilter(atlas, TEXTURE_FILTER_POINT);


    // =====================================================
    // MATERIAL
    // =====================================================

    Material material = LoadMaterialDefault();

    material.shader = shader;

    material.maps[MATERIAL_MAP_DIFFUSE].texture = atlas;


    // =====================================================
    // CAMERA
    // =====================================================

    Camera3D camera = { 0 };

    camera.position = {
        0.0f,
        2.0f,
        4.0f
    };

    camera.target = {
        0.0f,
        2.0f,
        0.0f
    };

    camera.up = {
        0.0f,
        1.0f,
        0.0f
    };

    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;


    // =====================================================
    // MOUSE LOOK
    // =====================================================

    DisableCursor();

    float cameraAngleX = -1.5708f;
    float cameraAngleY = 0.0f;

    const float mouseSensitivity = 0.002f;

    SetMousePosition(
        GetScreenWidth() / 2,
        GetScreenHeight() / 2
    );


    // =====================================================
    // MAIN LOOP
    // =====================================================

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();


        // =================================================
        // MOUSE
        // =================================================

        int centerX = GetScreenWidth() / 2;
        int centerY = GetScreenHeight() / 2;

        Vector2 mousePos = GetMousePosition();

        float deltaX =
            mousePos.x - centerX;

        float deltaY =
            mousePos.y - centerY;


        if (deltaX != 0.0f || deltaY != 0.0f)
        {
            cameraAngleX -=
                deltaX * mouseSensitivity;

            cameraAngleY +=
                deltaY * mouseSensitivity;
        }

        SetMousePosition(
            centerX,
            centerY
        );


        // =================================================
        // LIMIT VERTICAL LOOK
        // =================================================

        if (cameraAngleY > 1.48f)
            cameraAngleY = 1.48f;

        if (cameraAngleY < -1.48f)
            cameraAngleY = -1.48f;


        // =================================================
        // FORWARD
        // =================================================

        Vector3 forward = {
            cosf(cameraAngleX) *
                -cosf(cameraAngleY),

            -sinf(cameraAngleY),

            sinf(cameraAngleX) *
                cosf(cameraAngleY)
        };

        forward = Vector3Normalize(forward);


        // =================================================
        // RIGHT
        // =================================================

        Vector3 right =
            Vector3CrossProduct(
                forward,
                camera.up
            );

        right = Vector3Normalize(right);


        // =================================================
        // MOVEMENT
        // =================================================

        float moveSpeed =
            10.0f * dt;


        if (IsKeyDown(KEY_W))
        {
            camera.position =
                Vector3Add(
                    camera.position,
                    Vector3Scale(
                        forward,
                        moveSpeed
                    )
                );
        }


        if (IsKeyDown(KEY_S))
        {
            camera.position =
                Vector3Subtract(
                    camera.position,
                    Vector3Scale(
                        forward,
                        moveSpeed
                    )
                );
        }


        if (IsKeyDown(KEY_A))
        {
            camera.position =
                Vector3Subtract(
                    camera.position,
                    Vector3Scale(
                        right,
                        moveSpeed
                    )
                );
        }


        if (IsKeyDown(KEY_D))
        {
            camera.position =
                Vector3Add(
                    camera.position,
                    Vector3Scale(
                        right,
                        moveSpeed
                    )
                );
        }


        // =================================================
        // CAMERA TARGET
        // =================================================

        camera.target =
            Vector3Add(
                camera.position,
                forward
            );


        // =================================================
        // FPS
        // =================================================

        SetWindowTitle(
            TextFormat(
                "Raylib Minecraft Clone | FPS: %d",
                GetFPS()
            )
        );


        // =================================================
        // BLOCK RAYCAST
        // =================================================

        RaycastHit hit = RaycastBlock(
            camera.position,
            forward,
            10.0f
        );


        if (hit.block != nullptr)
        {
            // DESTROY
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                if (hit.block != nullptr)
                {
                    Vector3 position = hit.block->position;

                    DestroyBlock(hit.block);
                }
            }


            // PLACE
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            {
                if (hit.block != nullptr)
                {
                    Vector3 newPosition = Vector3Add(
                        hit.block->position,
                        hit.normal
                    );

                    if (!IsBlockAt(newPosition))
                    {
                        AddBlock(newPosition, "cobble_stone");
                    }
                }
            }
        }

        RebuildAllDirtyChunks();

        if (IsKeyPressed(KEY_F3))
        {
            showChunkBorders = !showChunkBorders;
        }

        BeginDrawing();

        ClearBackground(SKYBLUE);

        BeginMode3D(camera);

        DrawWorldChunks(material, camera);

        if (showChunkBorders)
        {
            DrawChunkBorders();
        }

        if (hit.block != nullptr)
        {
            DrawCubeWires(
                hit.block->position,
                1.02f,
                1.02f,
                1.02f,
                BLACK
            );
        }

        EndMode3D();

        DrawCircle(
            GetScreenWidth() / 2,
            GetScreenHeight() / 2,
            3,
            WHITE
        );

        EndDrawing();
}


EnableCursor();

    // cleanup
    UnloadWorldChunks();


    UnloadShader(shader);

    UnloadTexture(atlas);

    CloseWindow();

    return 0;
}