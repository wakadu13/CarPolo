// Renders every model of assets/trees.bin in a grid:  treeview <dir with trees.bin + trees.png> <out.png>
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "assets";
    const char *out = argc > 2 ? argv[2] : "treeview.png";
    char path[512];
    snprintf(path, sizeof(path), "%s/trees.bin", dir);
    int size = 0;
    unsigned char *d = LoadFileData(path, &size);
    if (!d || size < 8 || memcmp(d, "CPT1", 4)) { printf("bad trees.bin\n"); return 1; }
    unsigned n; memcpy(&n, d + 4, 4);
    InitWindow(1600, 900, "treeview");
    RenderInit();
    snprintf(path, sizeof(path), "%s/trees.png", dir);
    Texture2D atlas = LoadTexture(path);
    GenTextureMipmaps(&atlas);
    Material mat = LoadMaterialDefault();
    mat.shader = gLitCut;
    mat.maps[0].texture = atlas;
    Mesh *meshes = calloc(n, sizeof(Mesh));
    float *heights = calloc(n, sizeof(float));
    size_t off = 8;
    for (unsigned i = 0; i < n; i++) {
        char name[32]; memcpy(name, d + off, 32); off += 32;
        float h, r; unsigned nv;
        memcpy(&h, d + off, 4); memcpy(&r, d + off + 4, 4); memcpy(&nv, d + off + 8, 4); off += 12;
        const float *v = (const float *)(d + off); off += (size_t)nv * 32;
        MeshBuf b; MB_Init(&b, nv); MB_Mat(&b, 0, 0, 0);
        for (unsigned k = 0; k + 2 < nv; k += 3) {
            const float *a = v + k * 8, *c = v + (k + 1) * 8, *e = v + (k + 2) * 8;
            MB_TriUV(&b, (Vector3){ a[0], a[1], a[2] }, (Vector3){ c[0], c[1], c[2] }, (Vector3){ e[0], e[1], e[2] },
                     (Vector3){ a[3], a[4], a[5] }, (Vector3){ c[3], c[4], c[5] }, (Vector3){ e[3], e[4], e[5] }, WHITE, WHITE, WHITE,
                     (Vector2){ a[6], a[7] }, (Vector2){ c[6], c[7] }, (Vector2){ e[6], e[7] });
        }
        meshes[i] = MB_Upload(&b); MB_Free(&b);
        heights[i] = h;
        printf("%2u %-32s h %.2f r %.2f verts %u\n", i, name, h, r, nv);
    }
    Camera3D cam = { { 0, 5, 52 }, { 0, 3.5f, 0 }, { 0, 1, 0 }, 62, CAMERA_PERSPECTIVE };
    for (int f = 0; f < 3; f++) {
        BeginDrawing();
        ClearBackground((Color){ 150, 180, 220, 255 });
        BeginMode3D(cam);
        RenderSetFrame(cam, 1.0f / 9000.0f);
        rlDisableBackfaceCulling();
        int cols = 13;
        for (unsigned i = 0; i < n; i++) {
            float s = 7.0f / (heights[i] > 0.1f ? heights[i] : 1);       // all shown 7 units tall
            Matrix m = MatrixMultiply(MatrixScale(s, s, s), MatrixTranslate(-33 + (i % cols) * 5.5f, 0, 0));
            DrawMesh(meshes[i], mat, m);
        }
        rlEnableBackfaceCulling();
        EndMode3D();
        for (unsigned i = 0; i < n; i++) { Vector3 wp = { -33 + i * 5.5f, -0.8f, 0 }; Vector2 sp = GetWorldToScreen(wp, cam); DrawText(TextFormat("%u", i), (int)sp.x - 8, (int)sp.y, 28, BLACK); }
        EndDrawing();
    }
    TakeScreenshot(out);
    CloseWindow();
    return 0;
}
