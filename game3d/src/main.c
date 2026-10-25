// Temporary placeholder (replaced by the real game): opens a window and draws a rotating cube.
#include "raylib.h"

int main(void)
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "CarPolo 3D");
    Camera3D cam = { { 4, 3, 4 }, { 0, 0.5f, 0 }, { 0, 1, 0 }, 50, CAMERA_PERSPECTIVE };
    float t = 0;
    while (!WindowShouldClose()) {
        t += GetFrameTime();
        BeginDrawing();
        ClearBackground((Color){ 150, 200, 240, 255 });
        BeginMode3D(cam);
        DrawCubeV((Vector3){ 0, 0.5f, 0 }, (Vector3){ 1 + 0.2f * __builtin_sinf(t), 1, 1 }, (Color){ 30, 110, 130, 255 });
        DrawGrid(10, 1);
        EndMode3D();
        DrawText("CarPolo 3D", 20, 20, 30, WHITE);
        DrawFPS(20, 60);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
