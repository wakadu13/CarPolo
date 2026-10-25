// French town signs (EB10 entry / EB20 exit) generated from the route's built-up sections,
// plus the start marker (Google-Maps-like white disc) and the red arrival pin.
#include "signs.h"
#include "world.h"
#include "render.h"
#include "scene.h"
#include "raymath.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_SIGNS 48

typedef struct {
    Texture2D tex;
    Matrix board;      // transform of the unit board (x: -0.5..0.5, y: 0..1)
    Vector3 pos;
    float w, h;
} Sign;

static Sign signs[MAX_SIGNS];
static int nSigns;
static Mesh boardMesh, postsMesh;
static Material boardMat, litMat;
static bool ready;

static Mesh makeBoard(void)
{
    Mesh m = { 0 };
    m.vertexCount = 8;
    m.triangleCount = 4;
    float v[] = { -0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 1, 0, -0.5f, 1, 0,          // front (+z)
                  0.5f, 0, -0.02f, -0.5f, 0, -0.02f, -0.5f, 1, -0.02f, 0.5f, 1, -0.02f };  // back
    float t[] = { 0, 1, 1, 1, 1, 0, 0, 0, 0.98f, 0.98f, 0.99f, 0.98f, 0.99f, 0.99f, 0.98f, 0.99f };
    float n[] = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1 };
    unsigned short idx[] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    m.vertices = MemAlloc(sizeof(v)); memcpy(m.vertices, v, sizeof(v));
    m.texcoords = MemAlloc(sizeof(t)); memcpy(m.texcoords, t, sizeof(t));
    m.normals = MemAlloc(sizeof(n)); memcpy(m.normals, n, sizeof(n));
    m.indices = MemAlloc(sizeof(idx)); memcpy(m.indices, idx, sizeof(idx));
    UploadMesh(&m, false);
    return m;
}

static Texture2D townTexture(Font font, bool fontOk, const char *name, bool exitSign)
{
    int W_ = 512, H_ = 200;
    Image im = GenImageColor(W_, H_, (Color){ 214, 30, 36, 255 });
    ImageDrawRectangle(&im, 14, 14, W_ - 28, H_ - 28, WHITE);
    ImageDrawRectangle(&im, 0, 0, W_, 4, (Color){ 120, 120, 120, 255 });
    char up[96];
    int k = 0;
    for (int i = 0; name[i] && k < 90; i++) {
        unsigned char c = (unsigned char)name[i];
        up[k++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    up[k] = 0;
    // accented lowercase (UTF-8 2-byte, U+00E0..U+00FF) -> uppercase by subtracting 0x20 from the 2nd byte
    for (int i = 0; up[i] && up[i + 1]; i++) {
        if ((unsigned char)up[i] == 0xC3 && (unsigned char)up[i + 1] >= 0xA0 && (unsigned char)up[i + 1] <= 0xBE) { up[i + 1] = (char)((unsigned char)up[i + 1] - 0x20); i++; }
    }
    float fs = 64;
    if (fontOk) {
        Vector2 m = MeasureTextEx(font, up, fs, 1);
        while (m.x > W_ - 60 && fs > 24) { fs -= 2; m = MeasureTextEx(font, up, fs, 1); }
        ImageDrawTextEx(&im, font, up, (Vector2){ (W_ - m.x) / 2, (H_ - m.y) / 2 }, fs, 1, (Color){ 20, 20, 24, 255 });
    } else {
        int w = MeasureText(up, 40);
        ImageDrawText(&im, up, (W_ - w) / 2, H_ / 2 - 20, 40, BLACK);
    }
    if (exitSign) ImageDrawLineEx(&im, (Vector2){ 30, H_ - 30 }, (Vector2){ W_ - 30, 30 }, 16, (Color){ 214, 30, 36, 255 });
    Texture2D t = LoadTextureFromImage(im);
    UnloadImage(im);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

static Texture2D panelTexture(Font font, bool fontOk, const char *l1, const char *l2, Color bg)
{
    int W_ = 512, H_ = 256;
    Image im = GenImageColor(W_, H_, WHITE);
    ImageDrawRectangle(&im, 10, 10, W_ - 20, H_ - 20, bg);
    if (fontOk) {
        Vector2 m1 = MeasureTextEx(font, l1, 72, 1), m2 = MeasureTextEx(font, l2, 34, 1);
        ImageDrawTextEx(&im, font, l1, (Vector2){ (W_ - m1.x) / 2, 50 }, 72, 1, WHITE);
        ImageDrawTextEx(&im, font, l2, (Vector2){ (W_ - m2.x) / 2, 150 }, 34, 1, WHITE);
    }
    Texture2D t = LoadTextureFromImage(im);
    UnloadImage(im);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

static void addSign(MeshBuf *posts, Texture2D tex, float s, float side, float w, float h, float boardY)
{
    if (nSigns >= MAX_SIGNS) return;
    Vector3 p;
    float hx, hz, width;
    const RoutePt *rp;
    RouteSample(s, &p, &hx, &hz, &width, &rp);
    float off = width * 0.5f + (rp->roadClass >= RC_MOTORWAY ? 4.5f : 2.6f);
    float d = side * off;
    Vector3 base = { p.x - hz * d, 0, p.z + hx * d };
    base.y = SceneGround(base.x, base.z, RouteIndexAtS(s)) - 0.2f;
    if (base.y < p.y - 3) base.y = p.y - 0.3f;
    // board faces the incoming driver: its +z normal points backwards along the route
    float yaw = atan2f(-hx, -hz);
    Matrix m = MatrixMultiply(MatrixScale(w, h, 1), MatrixRotateY(yaw));
    m = MatrixMultiply(m, MatrixTranslate(base.x, base.y + boardY, base.z));
    Sign *sg = &signs[nSigns++];
    sg->tex = tex; sg->board = m; sg->pos = base; sg->w = w; sg->h = h;
    // two posts
    Vector3 r = { cosf(yaw), 0, -sinf(yaw) };
    for (int k = -1; k <= 1; k += 2) {
        Vector3 pp = { base.x + r.x * k * w * 0.3f - hx * 0.05f, base.y, base.z + r.z * k * w * 0.3f - hz * 0.05f };
        MB_Cylinder(posts, pp, 0.045f, 0.045f, boardY + h * 0.9f, 6, (Color){ 150, 154, 160, 255 }, true);
    }
}

void SignsInit(Font font, bool fontOk)
{
    nSigns = 0;
    boardMesh = makeBoard();
    boardMat = LoadMaterialDefault();
    boardMat.shader = gLit;
    litMat = LoadMaterialDefault();
    litMat.shader = gLit;
    MeshBuf posts;
    MB_Init(&posts, 1024);

    // --- town entry / exit signs from the built-up runs of the route
    int i = 0;
    float lastEntry = -1e9f;
    while (i < W.nRoute) {
        if (!(W.route[i].flags & RF_BUILTUP)) { i++; continue; }
        int j = i;
        while (j < W.nRoute && (W.route[j].flags & RF_BUILTUP)) j++;
        float s0 = W.route[i].s, s1 = W.route[j - 1].s;
        if (s1 - s0 > 250) {
            int mid = (i + j) / 2, pid = W.route[mid].placeId;
            const char *name = WorldStr(pid);
            if (name[0] && s0 - lastEntry > 600 && nSigns + 2 < MAX_SIGNS - 4) {
                Texture2D tin = townTexture(font, fontOk, name, false), tout = townTexture(font, fontOk, name, true);
                if (s0 > 30) addSign(&posts, tin, s0, 1, 1.9f, 0.74f, 1.3f);
                if (s1 < W.length - 30) addSign(&posts, tout, s1, 1, 1.9f, 0.74f, 1.3f);
                lastEntry = s0;
            }
        }
        i = j;
    }
    // --- start panel and arrival panel
    Texture2D tStart = panelTexture(font, fontOk, "DÉPART", "2000 chemin départemental 15", (Color){ 32, 110, 130, 255 });
    addSign(&posts, tStart, 12.0f, 1, 2.6f, 1.3f, 1.2f);
    Texture2D tEnd = panelTexture(font, fontOk, "ARRIVÉE", "CarPolo 3D", (Color){ 200, 40, 40, 255 });
    addSign(&posts, tEnd, W.length - 16.0f, 1, 2.6f, 1.3f, 1.2f);

    // --- 3D markers: start disc (white with grey ring) and the red map pin at the arrival
    {
        Vector3 p; float hx, hz, w;
        RouteSample(4.0f, &p, &hx, &hz, &w, NULL);
        float d = -(w * 0.5f + 3.0f);
        Vector3 b = { p.x - hz * d, p.y - 0.2f, p.z + hx * d };
        MB_Cylinder(&posts, b, 0.08f, 0.08f, 2.6f, 6, (Color){ 140, 144, 150, 255 }, false);
        MB_Blob(&posts, (Vector3){ b.x, b.y + 3.2f, b.z }, (Vector3){ 0.75f, 0.75f, 0.2f }, 4, (Color){ 250, 250, 250, 255 }, 0, 1);
        MB_Blob(&posts, (Vector3){ b.x, b.y + 3.2f, b.z }, (Vector3){ 0.5f, 0.5f, 0.24f }, 4, (Color){ 70, 74, 80, 255 }, 0, 2);
        MB_Blob(&posts, (Vector3){ b.x, b.y + 3.2f, b.z }, (Vector3){ 0.32f, 0.32f, 0.27f }, 4, (Color){ 250, 250, 250, 255 }, 0, 3);
    }
    {
        Vector3 p; float hx, hz, w;
        RouteSample(W.length - 2.0f, &p, &hx, &hz, &w, NULL);
        float d = w * 0.5f + 4.0f;
        Vector3 b = { p.x - hz * d, p.y - 0.2f, p.z + hx * d };
        Color red = { 234, 67, 53, 255 };
        MB_Cylinder(&posts, b, 0.0f, 1.25f, 4.2f, 16, red, false);                        // the point
        MB_Blob(&posts, (Vector3){ b.x, b.y + 4.6f, b.z }, (Vector3){ 1.55f, 1.55f, 1.55f }, 6, red, 0, 7);
        MB_Blob(&posts, (Vector3){ b.x - hz * -0.0f, b.y + 4.7f, b.z }, (Vector3){ 0.62f, 0.62f, 1.62f }, 5, (Color){ 140, 30, 24, 255 }, 0, 8);
    }
    postsMesh = MB_Upload(&posts);
    MB_Free(&posts);
    ready = true;
    TraceLog(LOG_INFO, "SIGNS: %d signs", nSigns);
}

void SignsDraw(Camera3D cam)
{
    if (!ready) return;
    RenderSetMat(MAT_PLASTIC);
    DrawMesh(postsMesh, litMat, MatrixIdentity());
    RenderSetMatEx(0.2f, 20, 0.05f, 0.05f, 0.12f);
    for (int i = 0; i < nSigns; i++) {
        if (Vector3DistanceSqr(signs[i].pos, cam.position) > 900.0f * 900.0f) continue;
        boardMat.maps[MATERIAL_MAP_DIFFUSE].texture = signs[i].tex;
        DrawMesh(boardMesh, boardMat, signs[i].board);
    }
    RenderSetMat(MAT_MATTE);
}

void SignsShutdown(void)
{
    if (!ready) return;
    for (int i = 0; i < nSigns; i++) {
        bool dup = false;
        for (int k = 0; k < i; k++) if (signs[k].tex.id == signs[i].tex.id) dup = true;
        if (!dup) UnloadTexture(signs[i].tex);
    }
    UnloadMesh(boardMesh);
    UnloadMesh(postsMesh);
    nSigns = 0;
    ready = false;
}
