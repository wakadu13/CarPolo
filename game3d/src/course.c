// Race dressing along the route (see course.h). Everything is built once at start-up into a few
// static meshes grouped every 400 m of route, textured with one generated banner atlas.
#include "course.h"
#include "world.h"
#include "render.h"
#include "scene.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define SHOULDER   1.4f
#define GROUP_LEN  400.0f
#define MAX_GROUPS 48

typedef enum {
    SL_FINISH, SL_START, SL_CP1, SL_CP2, SL_CP3, SL_CP4, SL_CP5, SL_CP6, SL_SPONSOR1, SL_SPONSOR2,
    SL_D1000, SL_D500, SL_D200, SL_D100, SL_CHEVRON, SL_CHECKER, SL_ARROW, SL_STRIPES,
    SL_KM1, SL_KM_LAST = SL_KM1 + 15, SL_COUNT
} Slot;

static Rectangle slotUV[SL_COUNT];
static Texture2D atlas;
static Mesh groupMesh[MAX_GROUPS];
static BoundingBox groupBox[MAX_GROUPS];
static int nGroups;
static Material mat, matShadow;
static bool ready;

typedef struct { float x, z, r; const char *what; } Solid;
static Solid *solids;
static int nSolids, capSolids;

static int nCp;
static float cpS[COURSE_MAX_CP];
static char cpName[COURSE_MAX_CP][64];
static float finishS;

// ------------------------------------------------------------------ helpers
static Vector3 v3(float x, float y, float z) { return (Vector3){ x, y, z }; }

typedef struct { Vector3 p, f, r; float hw; int idx; const RoutePt *rp; } Frame;

static Frame frameAt(float s)
{
    Frame fr;
    float hx, hz, w;
    s = Clamp(s, 0.0f, W.length);
    RouteSample(s, &fr.p, &hx, &hz, &w, &fr.rp);
    fr.f = v3(hx, 0, hz);
    fr.r = v3(-hz, 0, hx);
    fr.hw = w * 0.5f + (fr.rp->roadClass >= RC_MOTORWAY ? 2.6f : 0.0f);
    fr.idx = RouteIndexAtS(s);
    return fr;
}

static Vector3 latPos(const Frame *fr, float d) { return Vector3Add(fr->p, Vector3Scale(fr->r, d)); }
static float groundAt(const Frame *fr, Vector3 p) { return SceneGround(p.x, p.z, fr->idx); }

static void addSolid(float x, float z, float r, const char *what)
{
    if (nSolids >= capSolids) { capSolids = capSolids ? capSolids * 2 : 256; solids = realloc(solids, sizeof(Solid) * capSolids); }
    solids[nSolids++] = (Solid){ x, z, r, what };
}

// building footprints: keep spectators, stands and bunting poles out of the houses
static bool insideBuilding(float x, float z, float margin)
{
    for (int b = 0; b < W.nBuildings; b++) {
        const Building *bd = &W.buildings[b];
        float dx = bd->cx - x, dz = bd->cz - z, rr = bd->radius + margin;
        if (dx * dx + dz * dz > rr * rr) continue;
        bool in = false;
        for (int i = 0, j = bd->nv - 1; i < bd->nv; j = i++) {
            float xi = bd->xz[2 * i], zi = bd->xz[2 * i + 1], xj = bd->xz[2 * j], zj = bd->xz[2 * j + 1];
            if (((zi > z) != (zj > z)) && (x < (xj - xi) * (z - zi) / (zj - zi) + xi)) in = !in;
            float ex = xi - xj, ez = zi - zj, L2 = ex * ex + ez * ez;
            float t = L2 > 0 ? Clamp(((x - xj) * ex + (z - zj) * ez) / L2, 0, 1) : 0;
            float qx = x - (xj + ex * t), qz = z - (zj + ez * t);
            if (qx * qx + qz * qz < margin * margin) return true;
        }
        if (in) return true;
    }
    return false;
}

static int groupOf(float s) { int g = (int)(s / GROUP_LEN); return g < 0 ? 0 : g >= nGroups ? nGroups - 1 : g; }

// ------------------------------------------------------------------ atlas
static void upper(const char *in, char *out, int cap)
{
    int k = 0;
    for (int i = 0; in[i] && k < cap - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        out[k++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    out[k] = 0;
    for (int i = 0; out[i] && out[i + 1]; i++)
        if ((unsigned char)out[i] == 0xC3 && (unsigned char)out[i + 1] >= 0xA0 && (unsigned char)out[i + 1] <= 0xBE) { out[i + 1] = (char)((unsigned char)out[i + 1] - 0x20); i++; }
}

static Font gFont;
static bool gFontOk;

static void textC(Image *im, Rectangle r, const char *t, float size, float yFrac, Color c)
{
    if (!gFontOk) { int w = MeasureText(t, (int)size); ImageDrawText(im, t, (int)(r.x + (r.width - w) / 2), (int)(r.y + r.height * yFrac - size / 2), (int)size, c); return; }
    Vector2 m = MeasureTextEx(gFont, t, size, 1);
    while (m.x > r.width - 16 && size > 10) { size -= 2; m = MeasureTextEx(gFont, t, size, 1); }
    ImageDrawTextEx(im, gFont, t, (Vector2){ r.x + (r.width - m.x) / 2, r.y + r.height * yFrac - m.y / 2 }, size, 1, c);
}

static void checker(Image *im, int x, int y, int w, int h, int sq)
{
    for (int j = 0; j < h; j += sq)
        for (int i = 0; i < w; i += sq)
            ImageDrawRectangle(im, x + i, y + j, sq, sq, (((i / sq) + (j / sq)) & 1) ? (Color){ 20, 20, 24, 255 } : WHITE);
}

static void buildAtlas(void)
{
    const int AW = 1024, AH = 1024;
    Image im = GenImageColor(AW, AH, WHITE);
    // shelf packing, in this order
    int sizes[SL_COUNT][2];
    for (int i = 0; i < SL_COUNT; i++) { sizes[i][0] = 128; sizes[i][1] = 64; }
    for (int i = SL_FINISH; i <= SL_CP6; i++) { sizes[i][0] = 512; sizes[i][1] = 96; }
    sizes[SL_SPONSOR1][0] = sizes[SL_SPONSOR2][0] = 512; sizes[SL_SPONSOR1][1] = sizes[SL_SPONSOR2][1] = 64;
    for (int i = SL_D1000; i <= SL_D100; i++) { sizes[i][0] = 256; sizes[i][1] = 128; }
    sizes[SL_CHEVRON][0] = 128; sizes[SL_CHEVRON][1] = 128;
    sizes[SL_CHECKER][0] = 128; sizes[SL_CHECKER][1] = 128;
    sizes[SL_ARROW][0] = 256; sizes[SL_ARROW][1] = 128;
    sizes[SL_STRIPES][0] = 256; sizes[SL_STRIPES][1] = 128;
    Rectangle px[SL_COUNT];
    int x = 0, y = 0, rowH = 0;
    for (int i = 0; i < SL_COUNT; i++) {
        if (x + sizes[i][0] > AW) { x = 0; y += rowH; rowH = 0; }
        px[i] = (Rectangle){ (float)x, (float)y, (float)sizes[i][0], (float)sizes[i][1] };
        x += sizes[i][0];
        if (sizes[i][1] > rowH) rowH = sizes[i][1];
    }
    Color red = { 214, 32, 38, 255 }, blue = { 28, 86, 200, 255 }, green = { 24, 140, 84, 255 }, ink = { 20, 20, 24, 255 };
    // finish: black on white between two chequered bands
    {
        Rectangle r = px[SL_FINISH];
        checker(&im, (int)r.x, (int)r.y, (int)r.width, 16, 16);
        checker(&im, (int)r.x, (int)(r.y + r.height - 16), (int)r.width, 16, 16);
        textC(&im, r, "ARRIVÉE", 64, 0.5f, ink);
    }
    {
        Rectangle r = px[SL_START];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, green);
        checker(&im, (int)r.x, (int)r.y, (int)r.width, 12, 12);
        checker(&im, (int)r.x, (int)(r.y + r.height - 12), (int)r.width, 12, 12);
        textC(&im, r, "DÉPART", 62, 0.52f, WHITE);
    }
    for (int i = 0; i < 6; i++) {
        Rectangle r = px[SL_CP1 + i];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, blue);
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, 6, WHITE);
        ImageDrawRectangle(&im, (int)r.x, (int)(r.y + r.height - 6), (int)r.width, 6, WHITE);
        char t[96], up[96];
        snprintf(t, sizeof(t), "CHECKPOINT %d", i + 1);
        textC(&im, r, t, 34, 0.32f, (Color){ 255, 220, 60, 255 });
        upper(i < nCp ? cpName[i] : "", up, sizeof(up));
        textC(&im, r, up, 34, 0.70f, WHITE);
    }
    for (int k = 0; k < 2; k++) {
        Rectangle r = px[SL_SPONSOR1 + k];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, k ? red : (Color){ 22, 40, 92, 255 });
        ImageDrawRectangle(&im, (int)r.x, (int)(r.y + r.height - 8), (int)r.width, 8, k ? WHITE : red);
        textC(&im, r, k ? "C4 CACTUS · CORNILLON → LANÇON" : "CARPOLO 3D · RALLYE DE PROVENCE", 34, 0.45f, WHITE);
    }
    static const char *dist[4] = { "1 km", "500 m", "200 m", "100 m" };
    for (int i = 0; i < 4; i++) {
        Rectangle r = px[SL_D1000 + i];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, red);
        checker(&im, (int)r.x, (int)r.y, (int)r.width, 16, 16);
        ImageDrawRectangleLines(&im, (Rectangle){ r.x, r.y, r.width, r.height }, 4, WHITE);
        textC(&im, (Rectangle){ r.x, r.y + 16, r.width, 36 }, "ARRIVÉE", 30, 0.55f, WHITE);
        textC(&im, (Rectangle){ r.x, r.y + 48, r.width, 80 }, dist[i], 62, 0.5f, WHITE);
    }
    {
        Rectangle r = px[SL_CHEVRON];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, red);
        for (int k = 0; k < 2; k++) {
            float ox = r.x + 18 + k * 46;
            ImageDrawLineEx(&im, (Vector2){ ox, r.y + 18 }, (Vector2){ ox + 40, r.y + 64 }, 18, WHITE);
            ImageDrawLineEx(&im, (Vector2){ ox + 40, r.y + 64 }, (Vector2){ ox, r.y + 110 }, 18, WHITE);
        }
    }
    checker(&im, (int)px[SL_CHECKER].x, (int)px[SL_CHECKER].y, 128, 128, 16);
    {
        Rectangle r = px[SL_ARROW];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, blue);
        ImageDrawRectangleLines(&im, r, 5, WHITE);
        ImageDrawRectangle(&im, (int)r.x + 30, (int)r.y + 50, 120, 28, WHITE);
        ImageDrawTriangle(&im, (Vector2){ r.x + 150, r.y + 20 }, (Vector2){ r.x + 150, r.y + 108 }, (Vector2){ r.x + 226, r.y + 64 }, WHITE);
        ImageDrawTriangle(&im, (Vector2){ r.x + 150, r.y + 20 }, (Vector2){ r.x + 226, r.y + 64 }, (Vector2){ r.x + 150, r.y + 108 }, WHITE);
    }
    {
        Rectangle r = px[SL_STRIPES];
        for (int i = -4; i < 12; i++) {
            Vector2 a = { r.x + i * 32, r.y + r.height }, b = { r.x + i * 32 + 128, r.y };
            ImageDrawLineEx(&im, a, b, 22, (i & 1) ? WHITE : red);
        }
    }
    for (int i = 0; i < 16; i++) {
        Rectangle r = px[SL_KM1 + i];
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, (int)r.height, (Color){ 250, 250, 246, 255 });
        ImageDrawRectangle(&im, (int)r.x, (int)r.y, (int)r.width, 14, red);
        ImageDrawRectangleLines(&im, r, 3, ink);
        char t[16];
        snprintf(t, sizeof(t), "KM %d", i + 1);
        textC(&im, (Rectangle){ r.x, r.y + 14, r.width, r.height - 14 }, t, 40, 0.5f, ink);
    }
    for (int i = 0; i < SL_COUNT; i++)   // 1 px inset against bleeding between slots
        slotUV[i] = (Rectangle){ (px[i].x + 1) / AW, (px[i].y + 1) / AH, (px[i].width - 2) / AW, (px[i].height - 2) / AH };
    atlas = LoadTextureFromImage(im);
    UnloadImage(im);
    GenTextureMipmaps(&atlas);
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
    SetTextureFilter(atlas, TEXTURE_FILTER_ANISOTROPIC_8X);
#else
    SetTextureFilter(atlas, TEXTURE_FILTER_TRILINEAR);
#endif
    SetTextureWrap(atlas, TEXTURE_WRAP_CLAMP);
}

// ------------------------------------------------------------------ geometry primitives
// textured panel: centre c, 'right' as seen by its viewer, half sizes; flipU mirrors the texture
static void panel(MeshBuf *b, Vector3 c, Vector3 right, float hw, float hh, int slot, Color tint, bool flipU)
{
    Rectangle r = slotUV[slot];
    Vector3 up = { 0, 1, 0 };
    // normal towards the viewer, tilted up so the banners catch the sky light even against the sun
    Vector3 n = Vector3Normalize(Vector3Add(Vector3Normalize(Vector3CrossProduct(right, up)), (Vector3){ 0, 0.8f, 0 }));
    Vector3 a = Vector3Subtract(Vector3Subtract(c, Vector3Scale(right, hw)), Vector3Scale(up, hh));
    Vector3 bb = Vector3Subtract(Vector3Add(c, Vector3Scale(right, hw)), Vector3Scale(up, hh));
    Vector3 cc = Vector3Add(Vector3Add(c, Vector3Scale(right, hw)), Vector3Scale(up, hh));
    Vector3 d = Vector3Add(Vector3Subtract(c, Vector3Scale(right, hw)), Vector3Scale(up, hh));
    float u0 = flipU ? r.x + r.width : r.x, u1 = flipU ? r.x : r.x + r.width;
    MB_Mat(b, 0, 0, 0);
    MB_QuadN(b, a, bb, cc, d, n, tint, (Vector2){ u0, r.y + r.height }, (Vector2){ u1, r.y + r.height }, (Vector2){ u1, r.y }, (Vector2){ u0, r.y });
}

static void box(MeshBuf *b, Vector3 c, Vector3 half, float yaw, Color col)
{
    MB_Mat(b, 0, 1, 0);
    MB_Box(b, c, half, yaw, col);
}

static void post(MeshBuf *b, Vector3 base, float h, float r, Color col)
{
    MB_Mat(b, 0, 1, 0);
    MB_Cylinder(b, base, r, r * 0.9f, h, 6, col, true);
}

static float yawOf(Vector3 f) { return atan2f(f.x, f.z); }   // MB_Box yaw: local +z along f

// ------------------------------------------------------------------ course objects
static void person(MeshBuf *b, Vector3 p, Vector3 facing, unsigned seed, bool cheering)
{
    unsigned s = seed;
    static const Color shirts[] = { { 220, 40, 40, 255 }, { 30, 90, 200, 255 }, { 250, 200, 40, 255 }, { 240, 240, 236, 255 },
                                    { 40, 160, 90, 255 }, { 240, 120, 30, 255 }, { 130, 60, 170, 255 }, { 30, 30, 34, 255 } };
    static const Color legsC[] = { { 40, 56, 96, 255 }, { 30, 30, 34, 255 }, { 196, 176, 140, 255 }, { 80, 84, 92, 255 } };
    static const Color skin[] = { { 238, 196, 160, 255 }, { 214, 166, 126, 255 }, { 160, 112, 80, 255 }, { 110, 76, 54, 255 } };
    float yaw = yawOf(facing);
    float h = 0.92f + Rand01(&s) * 0.16f;
    Color shirt = shirts[(int)(Rand01(&s) * 8) % 8], leg = legsC[(int)(Rand01(&s) * 4) % 4], sk = skin[(int)(Rand01(&s) * 4) % 4];
    Vector3 side = { cosf(yaw), 0, -sinf(yaw) };
    box(b, v3(p.x, p.y + 0.42f * h, p.z), v3(0.16f, 0.42f * h, 0.11f), yaw, leg);
    box(b, v3(p.x, p.y + 1.1f * h, p.z), v3(0.21f, 0.3f * h, 0.13f), yaw, shirt);
    for (int k = -1; k <= 1; k += 2) {
        Vector3 sh = Vector3Add(p, Vector3Scale(side, k * 0.27f));
        if (cheering) box(b, v3(sh.x, p.y + 1.62f * h, sh.z), v3(0.055f, 0.25f * h, 0.055f), yaw, (k > 0 || Rand01(&s) < 0.5f) ? sk : shirt);
        else box(b, v3(sh.x, p.y + 1.08f * h, sh.z), v3(0.055f, 0.27f * h, 0.055f), yaw, shirt);
    }
    MB_Mat(b, 0, 1, 0);
    MB_Blob(b, v3(p.x, p.y + 1.56f * h, p.z), v3(0.12f, 0.14f, 0.12f), 3, sk, 0, seed);
    if (Rand01(&s) < 0.3f) box(b, v3(p.x, p.y + 1.69f * h, p.z), v3(0.13f, 0.03f, 0.13f), yaw, shirt);   // cap
}

// The builders below write into the group buffers.
static MeshBuf gb[MAX_GROUPS];
static MeshBuf *G_(float s) { return &gb[groupOf(s)]; }

static void crowdInto(int side, float s0, float s1, float density, unsigned seed)
{
    unsigned s = seed;
    for (float t = s0; t < s1; t += 1.0f / density) {
        Frame fr = frameAt(t);
        for (int row = 0; row < 2; row++) {
            if (row == 1 && Rand01(&s) < 0.45f) continue;
            float d = side * (fr.hw + SHOULDER + 2.0f + row * 0.9f + Rand01(&s) * 0.4f);
            Vector3 p = latPos(&fr, d);
            p = Vector3Add(p, Vector3Scale(fr.f, (Rand01(&s) - 0.5f) * 0.6f));
            if (insideBuilding(p.x, p.z, 0.6f)) continue;
            p.y = groundAt(&fr, p);
            Vector3 facing = Vector3Normalize(Vector3Add(Vector3Scale(fr.r, (float)-side), Vector3Scale(fr.f, (Rand01(&s) - 0.6f) * 0.8f)));
            person(G_(t), p, facing, s, Rand01(&s) < 0.35f);
        }
    }
}

// advertising boards along one side between s0 and s1 (both faces, the inner one with the logo)
static void boards(int side, float s0, float s1, int slot)
{
    const float L = 6.0f, H = 0.75f;
    for (float t = s0; t + L <= s1 + 0.01f; t += L) {
        Frame a = frameAt(t), b = frameAt(t + L);
        float da = side * (a.hw + SHOULDER + 0.9f), db = side * (b.hw + SHOULDER + 0.9f);
        Vector3 pa = latPos(&a, da), pb = latPos(&b, db);
        Vector3 mid = Vector3Lerp(pa, pb, 0.5f);
        if (insideBuilding(mid.x, mid.z, 0.4f)) continue;
        float ya = groundAt(&a, pa), yb = groundAt(&b, pb);
        mid.y = fminf(ya, yb) + 0.25f + H * 0.5f;
        Vector3 along = Vector3Normalize(Vector3Subtract(pb, pa));
        float len = Vector3Distance(pa, pb);
        MeshBuf *m = G_(t);
        // the face seen from the road: its 'right' is the travel direction on the right side
        Vector3 right = side > 0 ? along : Vector3Negate(along);
        Vector3 in = Vector3Scale(a.r, (float)-side);
        panel(m, Vector3Add(mid, Vector3Scale(in, 0.04f)), right, len * 0.5f, H * 0.5f, slot, WHITE, false);
        box(m, v3(mid.x, mid.y, mid.z), v3(0.03f, H * 0.5f + 0.02f, len * 0.5f), yawOf(along), (Color){ 60, 64, 72, 255 });
        for (int k = 0; k < 2; k++) {
            Vector3 lp = Vector3Lerp(pa, pb, k ? 0.9f : 0.1f);
            lp.y = (k ? yb : ya) - 0.1f;
            post(m, lp, mid.y - lp.y - H * 0.5f + 0.05f, 0.04f, (Color){ 90, 94, 100, 255 });
        }
        for (float q = 0; q <= 1.0f; q += 0.25f) { Vector3 c = Vector3Lerp(pa, pb, q); addSolid(c.x, c.z, 0.35f, "contre une barrière"); }
    }
}

// arch over the road: two pillars and a beam carrying the banner on both faces
static void arch(float s, int slot, Color c1, Color c2, float height)
{
    Frame fr = frameAt(s);
    float hw = fr.hw + SHOULDER + 1.0f;
    MeshBuf *m = G_(s);
    float yaw = yawOf(fr.f);
    float roadY = fr.p.y;
    for (int k = -1; k <= 1; k += 2) {
        Vector3 b = latPos(&fr, k * hw);
        float gy = fminf(groundAt(&fr, b), roadY) - 0.3f;
        int seg = 8;
        float top = roadY + height + 1.3f;
        for (int i = 0; i < seg; i++) {
            float y0 = gy + (top - gy) * i / seg, y1 = gy + (top - gy) * (i + 1) / seg;
            box(m, v3(b.x, (y0 + y1) * 0.5f, b.z), v3(0.38f, (y1 - y0) * 0.5f, 0.38f), yaw, (i & 1) ? c2 : c1);
        }
        box(m, v3(b.x, gy + 0.15f, b.z), v3(0.62f, 0.2f, 0.62f), yaw, (Color){ 70, 74, 80, 255 });
        addSolid(b.x, b.z, 0.6f, "contre l'arche");
    }
    Vector3 c = fr.p;
    c.y = roadY + height + 0.65f;
    box(m, c, v3(hw + 0.4f, 0.65f, 0.28f), yaw, c1);
    // banners: the front one faces the incoming driver, the back one is readable from behind
    panel(m, Vector3Subtract(c, Vector3Scale(fr.f, 0.30f)), fr.r, hw - 0.3f, 0.6f, slot, WHITE, false);
    panel(m, Vector3Add(c, Vector3Scale(fr.f, 0.30f)), Vector3Negate(fr.r), hw - 0.3f, 0.6f, slot, WHITE, false);
}

// roadside sign on two posts, facing the incoming driver
static void roadSign(float s, int side, int slot, float w, float h, float y, bool both)
{
    Frame fr = frameAt(s);
    float d = side * (fr.hw + SHOULDER + 0.8f + w * 0.5f);
    Vector3 b = latPos(&fr, d);
    if (insideBuilding(b.x, b.z, 0.3f)) { d = side * (fr.hw + SHOULDER + 0.4f + w * 0.5f); b = latPos(&fr, d); }
    float gy = groundAt(&fr, b);
    MeshBuf *m = G_(s);
    Vector3 c = v3(b.x, gy + y + h * 0.5f, b.z);
    panel(m, Vector3Subtract(c, Vector3Scale(fr.f, 0.03f)), fr.r, w * 0.5f, h * 0.5f, slot, WHITE, false);
    if (both) panel(m, Vector3Add(c, Vector3Scale(fr.f, 0.03f)), Vector3Negate(fr.r), w * 0.5f, h * 0.5f, slot, WHITE, false);
    else box(m, Vector3Add(c, Vector3Scale(fr.f, 0.02f)), v3(w * 0.5f, h * 0.5f, 0.015f), yawOf(fr.f), (Color){ 150, 154, 160, 255 });
    for (int k = -1; k <= 1; k += 2) {
        Vector3 pp = Vector3Add(b, Vector3Scale(fr.r, k * w * 0.35f));
        pp.y = gy - 0.2f;
        pp = Vector3Add(pp, Vector3Scale(fr.f, 0.06f));
        post(m, pp, y + h * 0.9f + 0.2f, 0.05f, (Color){ 150, 154, 160, 255 });
        addSolid(pp.x, pp.z, 0.2f, "contre un panneau");
    }
}

// red/white kerb on the inside of a bend, from the edge of the asphalt outwards
static void kerb(float s0, float s1, int side)
{
    for (float t = s0; t < s1; t += 1.0f) {
        Frame a = frameAt(t), b = frameAt(fminf(t + 1.0f, s1));
        MeshBuf *m = G_(t);
        Color col = (((int)t) & 1) ? (Color){ 214, 34, 40, 255 } : (Color){ 244, 244, 240, 255 };
        float ea = side * a.hw, eb = side * b.hw, wa = side * (a.hw + 0.55f), wb = side * (b.hw + 0.55f);
        Vector3 p0 = latPos(&a, ea), p1 = latPos(&a, wa), p2 = latPos(&b, wb), p3 = latPos(&b, eb);
        p0.y = a.p.y + 0.06f; p1.y = a.p.y - 0.01f; p2.y = b.p.y - 0.01f; p3.y = b.p.y + 0.06f;
        MB_Mat(m, 0, 1, 0);
        Vector3 n = { 0, 1, 0 };
        Vector2 z = { 0, 0 };
        MB_QuadN(m, p0, p1, p2, p3, n, col, z, z, z, z);
    }
}

// chevron boards on the outside of a bend (texture arrows point to the bend's direction)
static void chevrons(float s0, float s1, int turn)
{
    int n = 0;
    for (float t = s0; t <= s1 && n < 8; t += 7.0f, n++) {
        Frame fr = frameAt(t);
        int side = -turn;                                      // outside of the bend
        float d = side * (fr.hw + SHOULDER + 0.9f);
        Vector3 b = latPos(&fr, d);
        if (insideBuilding(b.x, b.z, 0.3f)) continue;
        float gy = groundAt(&fr, b);
        MeshBuf *m = G_(t);
        // face the driver approaching the board, a bit turned towards the road
        Vector3 view = Vector3Normalize(Vector3Add(fr.f, Vector3Scale(fr.r, (float)side * 0.35f)));
        Vector3 right = { -view.z, 0, view.x };
        Vector3 c = v3(b.x, gy + 1.15f, b.z);
        panel(m, c, right, 0.45f, 0.4f, SL_CHEVRON, WHITE, turn < 0);
        box(m, Vector3Add(c, Vector3Scale(view, 0.02f)), v3(0.45f, 0.4f, 0.012f), yawOf(view), (Color){ 150, 154, 160, 255 });
        post(m, v3(b.x, gy - 0.2f, b.z), 1.15f, 0.04f, (Color){ 150, 154, 160, 255 });
        addSolid(b.x, b.z, 0.25f, "contre un panneau");
    }
}

static void hayBales(float s0, float s1, int side)
{
    for (float t = s0; t <= s1; t += 2.2f) {
        Frame fr = frameAt(t);
        float d = side * (fr.hw + SHOULDER + 2.3f);
        Vector3 b = latPos(&fr, d);
        if (insideBuilding(b.x, b.z, 0.8f)) continue;
        float gy = groundAt(&fr, b);
        MeshBuf *m = G_(t);
        MB_Mat(m, 0, 1, 0);
        MB_Cylinder(m, v3(b.x, gy - 0.1f, b.z), 0.75f, 0.75f, 1.25f, 10, (Color){ 214, 182, 104, 255 }, false);
        MB_Cylinder(m, v3(b.x, gy + 1.15f, b.z), 0.75f, 0.6f, 0.12f, 10, (Color){ 196, 164, 92, 255 }, true);
        addSolid(b.x, b.z, 0.8f, "dans les bottes de foin");
    }
}

// bunting across the road (village festivities)
static void bunting(float s)
{
    Frame fr = frameAt(s);
    float hw = fr.hw + SHOULDER + 0.8f;
    Vector3 a = latPos(&fr, -hw), b = latPos(&fr, hw);
    if (insideBuilding(a.x, a.z, 0.3f) || insideBuilding(b.x, b.z, 0.3f)) return;
    float ya = groundAt(&fr, a), yb = groundAt(&fr, b);
    MeshBuf *m = G_(s);
    post(m, v3(a.x, ya - 0.2f, a.z), 6.0f, 0.06f, (Color){ 80, 84, 90, 255 });
    post(m, v3(b.x, yb - 0.2f, b.z), 6.0f, 0.06f, (Color){ 80, 84, 90, 255 });
    addSolid(a.x, a.z, 0.2f, "contre un poteau");
    addSolid(b.x, b.z, 0.2f, "contre un poteau");
    static const Color cols[5] = { { 220, 40, 40, 255 }, { 250, 200, 40, 255 }, { 30, 90, 200, 255 }, { 244, 244, 240, 255 }, { 40, 160, 90, 255 } };
    int n = (int)(hw * 2 / 0.7f);
    Vector3 prev = { 0 };
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        Vector3 p = Vector3Lerp(a, b, t);
        p.y = ya + (yb - ya) * t + 5.7f - 0.9f * 4 * t * (1 - t);      // sagging line
        if (i > 0) {
            // the line itself (thin dark quad) and a pennant hanging from it
            Vector3 up = { 0, 0.025f, 0 };
            MB_Mat(m, 0, 1, 0);
            Vector3 nn = fr.f;
            Vector2 z = { 0, 0 };
            MB_QuadN(m, Vector3Subtract(prev, up), Vector3Subtract(p, up), Vector3Add(p, up), Vector3Add(prev, up), nn, (Color){ 40, 40, 44, 255 }, z, z, z, z);
            Vector3 mid = Vector3Lerp(prev, p, 0.5f);
            Vector3 tip = v3(mid.x, mid.y - 0.42f, mid.z);
            Color c = cols[i % 5];
            MB_TriN(m, prev, p, tip, nn, nn, nn, c, c, c);
        }
        prev = p;
    }
}

// barrier closing a side road (red/white board on two legs) + a marshal now and then
static void barrier(Vector3 c, Vector3 across, float halfLen, unsigned seed, int idx)
{
    float gy = SceneGround(c.x, c.z, idx);
    MeshBuf *m = G_(RouteProject(c.x, c.z, idx, 200, NULL, NULL));
    Vector3 dir = Vector3Normalize(across);
    float yaw = yawOf(dir);
    Vector3 n = { -dir.z, 0, dir.x };
    for (int face = -1; face <= 1; face += 2) {
        Vector3 cc = Vector3Add(v3(c.x, gy + 0.85f, c.z), Vector3Scale(n, face * 0.03f));
        panel(m, cc, Vector3Scale(dir, (float)-face), halfLen, 0.22f, SL_STRIPES, WHITE, false);
    }
    box(m, v3(c.x, gy + 0.85f, c.z), v3(0.025f, 0.22f, halfLen), yaw, (Color){ 230, 230, 226, 255 });
    for (int k = -1; k <= 1; k += 2) {
        Vector3 lp = Vector3Add(c, Vector3Scale(dir, k * (halfLen - 0.15f)));
        box(m, v3(lp.x, gy + 0.4f, lp.z), v3(0.05f, 0.45f, 0.05f), yaw, (Color){ 200, 204, 208, 255 });
        box(m, v3(lp.x, gy + 0.03f, lp.z), v3(0.35f, 0.04f, 0.08f), yaw, (Color){ 200, 204, 208, 255 });
    }
    for (float q = -halfLen; q <= halfLen; q += 0.7f) { Vector3 p = Vector3Add(c, Vector3Scale(dir, q)); addSolid(p.x, p.z, 0.35f, "contre une barrière"); }
    unsigned s = seed;
    if (Rand01(&s) < 0.35f) {
        Vector3 p = Vector3Add(c, Vector3Add(Vector3Scale(dir, halfLen + 0.8f), Vector3Scale(n, 0.8f)));
        if (!insideBuilding(p.x, p.z, 0.5f)) { p.y = SceneGround(p.x, p.z, idx); person(m, p, Vector3Negate(n), s, false); }
    }
}

// side roads touching the route: closed with a barrier a few metres away from our road
static void closeSideRoads(void)
{
    float *placed = malloc(sizeof(float) * 2 * 4096);
    int nPlaced = 0;
    for (int r = 0; r < W.nRoads; r++) {
        const SideRoad *sr = &W.roads[r];
        if (sr->np < 2 || (sr->flags & 4)) continue;           // bridges cross over / under
        for (int k = 0; k < sr->np && nPlaced < 4096; k++) {
            float x = sr->xz[2 * k], z = sr->xz[2 * k + 1];
            if (SceneRouteDist(x, z) > 30) continue;
            float d;
            float s = RouteProject(x, z, -1, 0, &d, NULL);
            if (s < 30 || s > W.length - 20) continue;
            Frame fr = frameAt(s);
            if (fabsf(d) > fr.hw + 2.5f) continue;              // this vertex touches the route
            // walk along the side road (both directions) to the point 7 m off the asphalt
            for (int dirk = -1; dirk <= 1; dirk += 2) {
                float px = x, pz = z;
                for (int q = k + dirk; q >= 0 && q < sr->np; q += dirk) {
                    float qx = sr->xz[2 * q], qz = sr->xz[2 * q + 1];
                    float L = hypotf(qx - px, qz - pz);
                    int steps = 1 + (int)(L / 0.5f);
                    bool done = false;
                    for (int i = 1; i <= steps; i++) {
                        float tx = px + (qx - px) * i / steps, tz = pz + (qz - pz) * i / steps;
                        float dd;
                        float ss = RouteProject(tx, tz, fr.idx, 120, &dd, NULL);
                        Frame f2 = frameAt(ss);
                        float off = fabsf(dd) - f2.hw;
                        if (off > 7.0f) {
                            bool near = false;
                            for (int j = 0; j < nPlaced && !near; j++) near = hypotf(placed[2 * j] - tx, placed[2 * j + 1] - tz) < 12.0f;
                            if (!near && ss > 25 && ss < W.length - 25) {
                                Vector3 along = Vector3Normalize(v3(qx - px, 0, qz - pz));
                                Vector3 across = { -along.z, 0, along.x };
                                barrier(v3(tx, 0, tz), across, sr->width * 0.5f + 0.4f, (unsigned)(r * 31 + q), f2.idx);
                                placed[2 * nPlaced] = tx; placed[2 * nPlaced + 1] = tz; nPlaced++;
                            }
                            done = true;
                            break;
                        }
                    }
                    if (done) break;
                    px = qx; pz = qz;
                    if (hypotf(px - x, pz - z) > 40) break;
                }
            }
        }
    }
    free(placed);
    TraceLog(LOG_INFO, "COURSE: %d side-road barriers", nPlaced);
}

// sharp bends: chevrons outside, kerbs inside, hay bales outside the sharpest rural ones
static void markBends(void)
{
    int i = 0, nb = 0;
    while (i < W.nRoute) {
        float k = W.route[i].curv;
        if (fabsf(k) < 1.0f / 55.0f) { i++; continue; }
        int sign = k > 0 ? 1 : -1, j = i;
        float kmax = 0;
        while (j < W.nRoute && W.route[j].curv * sign > 1.0f / 70.0f) { kmax = fmaxf(kmax, fabsf(W.route[j].curv)); j++; }
        float s0 = W.route[i].s, s1 = W.route[j - 1].s;
        if (s1 - s0 > 6 && s0 > 20 && s1 < W.length - 15) {
            chevrons(fmaxf(5, s0 - 6), s1, sign);
            kerb(s0, s1, sign);
            if (kmax > 1.0f / 30.0f && !(W.route[i].flags & RF_BUILTUP) && !(W.route[i].flags & RF_ROUNDABOUT)) hayBales(s0, fminf(s1, s0 + 14), -sign);
            nb++;
        }
        i = j + 1;
    }
    TraceLog(LOG_INFO, "COURSE: %d marked bends", nb);
}

// ------------------------------------------------------------------ init / draw
void CourseInit(Font font, bool fontOk)
{
    gFont = font; gFontOk = fontOk;
    nSolids = 0;
    finishS = W.length - 4.0f;
    nCp = 0;
    for (int z = 0; z < W.nZones && nCp < COURSE_MAX_CP && nCp < 6; z++) {
        float s = W.zones[z].s;
        if (s < 400 || s > W.length - 600) continue;
        cpS[nCp] = s;
        snprintf(cpName[nCp], sizeof(cpName[nCp]), "%s", WorldStr(W.zones[z].nameId));
        nCp++;
    }
    buildAtlas();
    nGroups = (int)(W.length / GROUP_LEN) + 1;
    if (nGroups > MAX_GROUPS) nGroups = MAX_GROUPS;
    for (int g = 0; g < nGroups; g++) MB_Init(&gb[g], 4096);

    Color red = { 214, 32, 38, 255 }, white = { 244, 244, 240, 255 }, blue = { 28, 86, 200, 255 }, green = { 24, 140, 84, 255 };
    // start: arch, boards, crowd
    arch(10.0f, SL_START, green, white, 4.6f);
    boards(1, 0.0f, 54.0f, SL_SPONSOR1);
    boards(-1, 0.0f, 54.0f, SL_SPONSOR2);
    crowdInto(1, 0.0f, 40.0f, 1.0f, 11);
    crowdInto(-1, 0.0f, 40.0f, 0.8f, 12);
    // checkpoints
    for (int i = 0; i < nCp; i++) {
        arch(cpS[i], SL_CP1 + i, blue, white, 4.8f);
        boards(1, cpS[i] - 30, cpS[i] + 24, i & 1 ? SL_SPONSOR2 : SL_SPONSOR1);
        boards(-1, cpS[i] - 30, cpS[i] + 24, i & 1 ? SL_SPONSOR1 : SL_SPONSOR2);
        crowdInto(1, cpS[i] - 18, cpS[i] + 12, 0.7f, 100 + i);
        crowdInto(-1, cpS[i] - 18, cpS[i] + 12, 0.5f, 200 + i);
    }
    // finish: arch, chequered flags, boards and a big crowd on the last 180 m, distance boards before
    arch(finishS, SL_FINISH, red, white, 4.8f);
    for (int k = -1; k <= 1; k += 2) {
        Frame fr = frameAt(finishS + 3.0f);
        Vector3 b = latPos(&fr, k * (fr.hw + SHOULDER + 3.4f));
        float gy = groundAt(&fr, b);
        MeshBuf *m = G_(finishS);
        post(m, v3(b.x, gy - 0.2f, b.z), 7.4f, 0.07f, (Color){ 200, 204, 210, 255 });
        Vector3 c = v3(b.x, gy + 6.4f, b.z);
        Vector3 fl = Vector3Add(Vector3Scale(fr.r, (float)-k * 0.9f), Vector3Scale(fr.f, 0.35f));
        panel(m, Vector3Add(c, Vector3Scale(fl, 0.5f)), Vector3Normalize(fl), 0.95f, 0.65f, SL_CHECKER, WHITE, false);
        addSolid(b.x, b.z, 0.25f, "contre un mât");
    }
    boards(1, finishS - 180, finishS + 18, SL_SPONSOR1);
    boards(-1, finishS - 180, finishS + 18, SL_SPONSOR2);
    crowdInto(1, finishS - 150, finishS + 12, 1.4f, 900);
    crowdInto(-1, finishS - 150, finishS + 12, 1.2f, 901);
    roadSign(finishS - 1000, 1, SL_D1000, 2.6f, 1.3f, 1.4f, false);
    roadSign(finishS - 500, 1, SL_D500, 2.6f, 1.3f, 1.4f, false);
    roadSign(finishS - 500, -1, SL_D500, 2.6f, 1.3f, 1.4f, false);
    roadSign(finishS - 200, 1, SL_D200, 2.6f, 1.3f, 1.4f, false);
    roadSign(finishS - 200, -1, SL_D200, 2.6f, 1.3f, 1.4f, false);
    roadSign(finishS - 100, 1, SL_D100, 2.6f, 1.3f, 1.4f, false);
    roadSign(finishS - 100, -1, SL_D100, 2.6f, 1.3f, 1.4f, false);
    // kilometre boards
    for (int km = 1; km <= 16 && km * 1000.0f < finishS - 150; km++) roadSign(km * 1000.0f, 1, SL_KM1 + km - 1, 0.9f, 0.45f, 0.8f, false);
    // bends, side roads, village bunting
    markBends();
    closeSideRoads();
    int nBunt = 0;
    for (float s = 60; s < finishS - 40; s += 55) {
        const RoutePt *rp = &W.route[RouteIndexAtS(s)];
        if (!(rp->flags & RF_BUILTUP) || (rp->flags & RF_ROUNDABOUT) || fabsf(rp->curv) > 1.0f / 80.0f) continue;
        bunting(s);
        nBunt++;
    }
    // upload
    long verts = 0;
    for (int g = 0; g < nGroups; g++) {
        if (gb[g].count == 0) { groupMesh[g] = (Mesh){ 0 }; MB_Free(&gb[g]); continue; }
        BoundingBox bb = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f } };
        for (int i = 0; i < gb[g].count; i++) {
            Vector3 p = { gb[g].pos[3 * i], gb[g].pos[3 * i + 1], gb[g].pos[3 * i + 2] };
            bb.min = Vector3Min(bb.min, p); bb.max = Vector3Max(bb.max, p);
        }
        groupBox[g] = bb;
        verts += gb[g].count;
        groupMesh[g] = MB_Upload(&gb[g]);
        MB_Free(&gb[g]);
    }
    mat = LoadMaterialDefault();
    mat.shader = gLit;
    mat.maps[0].texture = atlas;
    matShadow = LoadMaterialDefault();
    matShadow.shader = gDepth;
    ready = true;
    TraceLog(LOG_INFO, "COURSE: %d checkpoints, %d bunting lines, %d solid objects, %ld vertices", nCp, nBunt, nSolids, verts);
}

void CourseShutdown(void)
{
    if (!ready) return;
    for (int g = 0; g < nGroups; g++) if (groupMesh[g].vertexCount) UnloadMesh(groupMesh[g]);
    UnloadTexture(atlas);
    MemFree(mat.maps); MemFree(matShadow.maps);
    free(solids); solids = NULL; nSolids = capSolids = 0;
    ready = false;
}

static float boxDist(const BoundingBox *b, Vector3 p)
{
    float dx = fmaxf(fmaxf(b->min.x - p.x, 0), p.x - b->max.x);
    float dz = fmaxf(fmaxf(b->min.z - p.z, 0), p.z - b->max.z);
    return sqrtf(dx * dx + dz * dz);
}

void CourseDraw(Camera3D cam)
{
    if (!ready) return;
    RenderSetMatEx(0.12f, 16.0f, 0.02f, 0.04f, 0.08f);
    rlDisableBackfaceCulling();
    for (int g = 0; g < nGroups; g++) {
        if (!groupMesh[g].vertexCount || boxDist(&groupBox[g], cam.position) > 1300.0f) continue;
        DrawMesh(groupMesh[g], mat, MatrixIdentity());
    }
    rlEnableBackfaceCulling();
    RenderSetMat(MAT_MATTE);
}

void CourseDrawShadow(Vector3 centre, float radius)
{
    if (!ready) return;
    rlDisableBackfaceCulling();
    for (int g = 0; g < nGroups; g++) {
        if (!groupMesh[g].vertexCount || boxDist(&groupBox[g], centre) > radius) continue;
        DrawMesh(groupMesh[g], matShadow, MatrixIdentity());
    }
    rlEnableBackfaceCulling();
}

bool CourseCollide(float x, float z, float radius, const char **what)
{
    for (int i = 0; i < nSolids; i++) {
        const Solid *o = &solids[i];
        float dx = o->x - x, dz = o->z - z, rr = o->r + radius;
        if (dx * dx + dz * dz < rr * rr) { if (what) *what = o->what; return true; }
    }
    return false;
}

int CourseCheckpointCount(void) { return nCp; }
float CourseCheckpointS(int i) { return i >= 0 && i < nCp ? cpS[i] : 0; }
const char *CourseCheckpointName(int i) { return i >= 0 && i < nCp ? cpName[i] : ""; }
float CourseFinishS(void) { return finishS; }
