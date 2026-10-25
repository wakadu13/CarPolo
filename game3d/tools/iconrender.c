// iconrender.c — renders the C4 Cactus app icon (1024x1024 PNG) with the game's own shader/model.
// Build (from game3d/):
//   cc -O2 -std=c99 -Isrc -I/opt/raylib-desktop/include tools/iconrender.c src/car_model.c src/render.c \
//      src/car.c src/scene.c src/world.c src/thread.c /opt/raylib-desktop/lib/libraylib.a \
//      -lm -lpthread -ldl -lGL -lX11 -o build-bin/iconrender
// Run: xvfb-run -a -s "-screen 0 1280x1100x24" ./build-bin/iconrender desktop/icon_render.png
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "car_model.h"
#include "render.h"
#include "car.h"

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "icon_render.png";
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIDDEN);
    InitWindow(1024, 1024, "icon");
    RenderInit();
    CarInitShared();
    CarModel car = BuildCactusModel();
    RenderTexture2D rt = LoadRenderTexture(1024, 1024);
    Camera3D cam = { { 4.3f, 1.55f, 5.6f }, { 0.0f, 0.62f, 0.15f }, { 0, 1, 0 }, 34, CAMERA_PERSPECTIVE };
    Matrix world = MatrixRotateY(0.18f);
    MeshBuf g;
    MB_Init(&g, 256);
    // round road-coloured ground disc
    for (int i = 0; i < 48; i++) {
        float a0 = 2 * PI * i / 48, a1 = 2 * PI * (i + 1) / 48;
        MB_Tri(&g, (Vector3){ 0, 0, 0 }, (Vector3){ cosf(a1) * 9, 0, sinf(a1) * 9 }, (Vector3){ cosf(a0) * 9, 0, sinf(a0) * 9 }, (Color){ 84, 86, 92, 255 });
    }
    Mesh ground = MB_Upload(&g);
    Material mat = LoadMaterialDefault();
    mat.shader = gLit;
    for (int f = 0; f < 3; f++) {
        BeginTextureMode(rt);
        ClearBackground((Color){ 120, 180, 230, 255 });
        DrawRectangleGradientV(0, 0, 1024, 640, (Color){ 40, 110, 200, 255 }, (Color){ 170, 214, 240, 255 });
        DrawRectangleGradientV(0, 640, 1024, 384, (Color){ 210, 196, 150, 255 }, (Color){ 186, 168, 120, 255 });
        BeginMode3D(cam);
        RenderSetFrame(cam, 0.0f);
        RenderSetMat(MAT_ROAD);
        DrawMesh(ground, mat, MatrixIdentity());
        DrawBlobShadow(world, car.length, car.width);
        DrawCarModel(&car, world, 0.4f, -0.12f, false);
        EndMode3D();
        EndTextureMode();
    }
    Image im = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&im);
    ExportImage(im, out);
    UnloadImage(im);
    UnloadRenderTexture(rt);
    UnloadCarModel(&car);
    RenderShutdown();
    CloseWindow();
    return 0;
}
