// Streaming world geometry built from world.bin: terrain conformed to the road, road surface and
// markings, roadside furniture, vegetation from land cover, real buildings, side roads, far terrain.
#include "scene.h"
#include "world.h"
#include "render.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define CORRIDOR      720.0f   // near terrain extends this far from the route
#define NEAR_SUBDIV   50.0f    // cells closer than this are subdivided 2x2
#define CONFORM_ZONE  45.0f    // terrain within this distance is conformed to the road
#define SHOULDER      1.4f
#define BLEND         14.0f
// quality-dependent distances (SceneSetQuality)
static float LOAD_RADIUS = 1200.0f, DRAW_RADIUS = 1450.0f, TREE_NEAR = 110.0f, TREE_MID = 300.0f, TREE_FAR = 560.0f;
static int gQuality = 1;

typedef struct { int road, a, b; } RoadPiece;

typedef struct {
    int id;
    float s0, s1;
    int i0, i1, mid, win;
    int cx0, cz0, cx1, cz1;
    float minY, maxY;
    BoundingBox box;
    bool built;
    Mesh terrain, road, detail;
    Obstacle *obs; int nObs, capObs;
    int *bld; int nBld, capBld;
    int *pcs; int nPcs, capPcs;
    int *trees; int nTrees, capTrees;
} Chunk;

static Chunk *chunks;
static int nChunks;
static int16_t *owner;      // per DEM cell, chunk id or -1
static float *cdist;        // per DEM cell, approx distance to the route (m)
static uint8_t *mask;       // per DEM cell: 1 side road, 2 building
static int gnx, gnz;        // cell grid size
static float gx0, gz0, gcell;
static RoadPiece *pieces;
static int nPieces;
static Mesh farMesh[16];
static int nFar;
static Material litMat;

// ------------------------------------------------------------------ small helpers
static void pushInt(int **arr, int *n, int *cap, int v)
{
    if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *arr = realloc(*arr, sizeof(int) * *cap); }
    (*arr)[(*n)++] = v;
}

static void addObs(Chunk *c, float x, float z, float r, unsigned char kind)
{
    if (c->nObs >= c->capObs) { c->capObs = c->capObs ? c->capObs * 2 : 64; c->obs = realloc(c->obs, sizeof(Obstacle) * c->capObs); }
    c->obs[c->nObs++] = (Obstacle){ x, z, r, kind };
}

static inline int cellIndex(float x, float z, int *ci, int *cj)
{
    int i = (int)floorf((x - gx0) / gcell), j = (int)floorf((z - gz0) / gcell);
    if (ci) *ci = i;
    if (cj) *cj = j;
    if (i < 0 || j < 0 || i >= gnx || j >= gnz) return -1;
    return j * gnx + i;
}

float SceneRouteDist(float x, float z)
{
    int k = cellIndex(x, z, NULL, NULL);
    return k < 0 ? 1e9f : cdist[k];
}

static float conformed(float x, float z, int hint, int win, float *outD)
{
    float d;
    int seg;
    float s = RouteProject(x, z, hint, win, &d, &seg);
    float h = DemHeight(x, z);
    Vector3 p;
    float w;
    RouteSample(s, &p, NULL, NULL, &w, NULL);
    if (outD) *outD = d;
    // beyond the ends of the route: no conformation
    if ((s <= 0.01f || s >= W.length - 0.01f)) {
        float ex = 0;
        if (s <= 0.01f) { const RoutePt *a = &W.route[0]; ex = (x - a->x) * a->hx + (z - a->z) * a->hz; ex = -ex; }
        else { const RoutePt *a = &W.route[W.nRoute - 1]; ex = (x - a->x) * a->hx + (z - a->z) * a->hz; }
        if (ex > 2.0f) {
            float t = Clamp((ex - 2.0f) / 30.0f, 0, 1);
            float hw = w * 0.5f + SHOULDER, ad = fabsf(d), roadY = p.y - 0.25f;
            float hh = ad <= hw ? roadY : (ad < hw + BLEND ? roadY + (h - roadY) * ((ad - hw) / BLEND) : h);
            return hh + (h - hh) * t;
        }
    }
    float ad = fabsf(d), hw = w * 0.5f + SHOULDER;
    const RoutePt *rp;
    RouteSample(s, NULL, NULL, NULL, NULL, &rp);
    if (rp->roadClass >= RC_MOTORWAY) hw += 3.0f;   // hard shoulder + barriers
    float roadY = p.y - 0.25f;
    if (ad <= hw) return roadY;
    if (ad < hw + BLEND) {
        float t = (ad - hw) / BLEND;
        t = t * t * (3 - 2 * t);
        return roadY + (h - roadY) * t;
    }
    return h;
}

float SceneGround(float x, float z, int hint)
{
    float cd = SceneRouteDist(x, z);
    if (cd > CONFORM_ZONE + 15) return DemHeight(x, z);
    return conformed(x, z, hint, hint < 0 ? 0 : 90, NULL);
}

static float groundC(const Chunk *c, float x, float z, float cd)
{
    if (cd > CONFORM_ZONE + 12) return DemHeight(x, z);
    return conformed(x, z, c->mid, c->win, NULL);
}


// faces oriented explicitly (robust to the input winding)
static void quadFacing(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Vector3 hint, Color col)
{
    Vector3 n = Vector3CrossProduct(Vector3Subtract(c, a), Vector3Subtract(d, a));
    if (Vector3DotProduct(n, hint) < 0) MB_Quad(b, a, e, d, c, col);
    else MB_Quad(b, a, c, d, e, col);
}
static void triFacing(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 hint, Color col)
{
    Vector3 n = Vector3CrossProduct(Vector3Subtract(c, a), Vector3Subtract(d, a));
    if (Vector3DotProduct(n, hint) < 0) MB_Tri(b, a, d, c, col);
    else MB_Tri(b, a, c, d, col);
}

// ------------------------------------------------------------------ land cover colours
static float vnoise(float x, float z, float scale)
{
    float fx = x / scale, fz = z / scale;
    int ix = (int)floorf(fx), iz = (int)floorf(fz);
    float tx = fx - ix, tz = fz - iz;
    tx = tx * tx * (3 - 2 * tx); tz = tz * tz * (3 - 2 * tz);
    float a = (Hash2(ix, iz) & 1023) / 1023.f, b = (Hash2(ix + 1, iz) & 1023) / 1023.f;
    float c = (Hash2(ix, iz + 1) & 1023) / 1023.f, d = (Hash2(ix + 1, iz + 1) & 1023) / 1023.f;
    return (a + (b - a) * tx) * (1 - tz) + (c + (d - c) * tx) * tz;
}

static Color classColor(int cls, float x, float z)
{
    float n = vnoise(x, z, 37.0f), n2 = vnoise(x + 500, z - 300, 11.0f);
    Color c;
    switch (cls) {
    case LC_FOREST:      c = ColorLerp((Color){ 52, 76, 40, 255 }, (Color){ 72, 88, 48, 255 }, n); break;
    case LC_SHRUB:       c = ColorLerp((Color){ 112, 114, 74, 255 }, (Color){ 138, 132, 92, 255 }, n); break;
    case LC_GRASS:       c = ColorLerp((Color){ 128, 136, 72, 255 }, (Color){ 158, 150, 90, 255 }, n); break;
    case LC_CROP:        c = ColorLerp((Color){ 150, 128, 88, 255 }, (Color){ 176, 160, 104, 255 }, vnoise(x, z, 90.0f)); break;
    case LC_VINEYARD:    c = ColorLerp((Color){ 140, 112, 78, 255 }, (Color){ 156, 132, 92, 255 }, n); break;
    case LC_ORCHARD:     c = ColorLerp((Color){ 138, 128, 86, 255 }, (Color){ 150, 142, 100, 255 }, n); break;
    case LC_RESIDENTIAL: c = ColorLerp((Color){ 120, 128, 80, 255 }, (Color){ 150, 146, 112, 255 }, n2); break;
    case LC_INDUSTRIAL:  c = ColorLerp((Color){ 150, 146, 136, 255 }, (Color){ 170, 166, 154, 255 }, n2); break;
    case LC_BARE:        c = ColorLerp((Color){ 186, 176, 152, 255 }, (Color){ 206, 196, 172, 255 }, n2); break;
    case LC_WATER:       c = ColorLerp((Color){ 46, 92, 104, 255 }, (Color){ 70, 118, 126, 255 }, n2); break;
    case LC_WETLAND:     c = (Color){ 96, 112, 78, 255 }; break;
    case LC_PAVED:       c = (Color){ 120, 120, 116, 255 }; break;
    default:             c = ColorLerp((Color){ 132, 138, 78, 255 }, (Color){ 160, 152, 96, 255 }, n); break;
    }
    float k = 0.92f + 0.16f * n2;
    return (Color){ (unsigned char)Clamp(c.r * k, 0, 255), (unsigned char)Clamp(c.g * k, 0, 255), (unsigned char)Clamp(c.b * k, 0, 255), 255 };
}

static Color terrainColor(float x, float z, float ny, float roadD, float roadHalf)
{
    int cls = GridClass(&W.lcov, x, z);
    Color c = classColor(cls, x, z);
    // steep slopes: limestone shows through
    if (ny < 0.86f) c = ColorLerp(c, (Color){ 176, 168, 146, 255 }, Clamp((0.86f - ny) * 4.0f, 0, 0.8f));
    // dry verge along the road
    if (roadD >= 0) {
        float e = roadD - roadHalf;
        if (e < 1.2f) c = (Color){ 132, 126, 112, 255 };
        else if (e < 7.0f) c = ColorLerp((Color){ 168, 156, 112, 255 }, c, (e - 1.2f) / 5.8f);
    }
    return c;
}

// ------------------------------------------------------------------ vegetation
static void treeTrunk(MeshBuf *b, Vector3 p, float r, float h, Color col, int lod)
{
    MB_Cylinder(b, p, r, r * 0.7f, h, lod == 0 ? 6 : 4, col, false);
}

static void treePine(MeshBuf *b, Vector3 p, float h, unsigned seed, int lod)
{
    unsigned s = seed;
    // Aleppo pine: slender leaning trunk, irregular flat crown
    float lean = (Rand01(&s) - 0.5f) * 0.25f, lean2 = (Rand01(&s) - 0.5f) * 0.25f;
    Vector3 top = { p.x + lean * h, p.y + h * 0.78f, p.z + lean2 * h };
    Color bark = { 96, 78, 62, 255 };
    Color leaf = ColorJitter((Color){ 58, 92, 50, 255 }, seed, 14);
    if (lod == 2) {
        MB_Blob(b, (Vector3){ top.x, top.y, top.z }, (Vector3){ h * 0.32f, h * 0.2f, h * 0.32f }, 2, leaf, 0.2f, seed);
        return;
    }
    // trunk as a 2-segment leaning prism
    int sides = lod == 0 ? 4 : 3;
    for (int i = 0; i < sides; i++) {
        float a0 = 2 * PI * i / sides, a1 = 2 * PI * (i + 1) / sides, r0 = h * 0.03f, r1 = h * 0.018f;
        Vector3 b0 = { p.x + cosf(a0) * r0, p.y - 0.3f, p.z + sinf(a0) * r0 }, b1 = { p.x + cosf(a1) * r0, p.y - 0.3f, p.z + sinf(a1) * r0 };
        Vector3 t0 = { top.x + cosf(a0) * r1, top.y, top.z + sinf(a0) * r1 }, t1 = { top.x + cosf(a1) * r1, top.y, top.z + sinf(a1) * r1 };
        MB_Quad(b, b0, t0, t1, b1, bark);
    }
    int puffs = lod == 0 ? 2 : 1;
    for (int k = 0; k < puffs; k++) {
        float ox = (Rand01(&s) - 0.5f) * h * 0.35f, oz = (Rand01(&s) - 0.5f) * h * 0.35f, oy = (Rand01(&s) - 0.3f) * h * 0.08f;
        float rr = h * (puffs == 1 ? 0.34f : 0.24f + Rand01(&s) * 0.08f);
        MB_Blob(b, (Vector3){ top.x + ox, top.y + oy + h * 0.04f, top.z + oz }, (Vector3){ rr, rr * 0.5f, rr }, lod == 0 ? 3 : 2,
                ColorJitter(leaf, seed + k * 7, 10), 0.35f, seed + k);
    }
}

static void treeOak(MeshBuf *b, Vector3 p, float h, unsigned seed, int lod)
{
    Color leaf = ColorJitter((Color){ 62, 80, 44, 255 }, seed, 12);
    if (lod < 2) treeTrunk(b, (Vector3){ p.x, p.y - 0.2f, p.z }, h * 0.05f, h * 0.45f, (Color){ 80, 70, 58, 255 }, lod);
    MB_Blob(b, (Vector3){ p.x, p.y + h * 0.62f, p.z }, (Vector3){ h * 0.42f, h * 0.38f, h * 0.42f }, lod == 0 ? 3 : 2, leaf, 0.3f, seed);
}

static void treeOlive(MeshBuf *b, Vector3 p, float h, unsigned seed, int lod)
{
    Color leaf = ColorJitter((Color){ 124, 138, 100, 255 }, seed, 10);
    if (lod < 2) treeTrunk(b, (Vector3){ p.x, p.y - 0.2f, p.z }, 0.18f, h * 0.45f, (Color){ 98, 90, 78, 255 }, lod);
    MB_Blob(b, (Vector3){ p.x, p.y + h * 0.6f, p.z }, (Vector3){ h * 0.5f, h * 0.33f, h * 0.5f }, lod == 0 ? 3 : 2, leaf, 0.35f, seed);
}

static void treeCypress(MeshBuf *b, Vector3 p, float h, unsigned seed, int lod)
{
    Color leaf = ColorJitter((Color){ 40, 64, 40, 255 }, seed, 8);
    MB_Blob(b, (Vector3){ p.x, p.y + h * 0.5f, p.z }, (Vector3){ h * 0.11f, h * 0.52f, h * 0.11f }, lod == 0 ? 3 : 2, leaf, 0.12f, seed);
}

static void treePlane(MeshBuf *b, Vector3 p, float h, unsigned seed, int lod)
{
    // platane: thick mottled pale trunk splitting into limbs, wide crown yellowing in autumn
    Color leaf = ColorLerp((Color){ 118, 128, 58, 255 }, (Color){ 182, 158, 72, 255 }, (seed & 255) / 255.0f);
    Color bark = { 176, 168, 134, 255 };
    float th = h * 0.36f;
    MB_Cylinder(b, (Vector3){ p.x, p.y - 0.2f, p.z }, 0.42f, 0.34f, th + 0.2f, lod == 0 ? 7 : 4, bark, false);
    unsigned s = seed;
    if (lod == 0) {
        for (int k = 0; k < 3; k++) {
            float a = k * 2.1f + Rand01(&s);
            Vector3 b0 = { p.x, p.y + th, p.z };
            Vector3 b1 = { p.x + cosf(a) * h * 0.18f, p.y + th + h * 0.26f, p.z + sinf(a) * h * 0.18f };
            Vector3 dv = Vector3Subtract(b1, b0);
            for (int q = 0; q < 4; q++) {
                float a0 = q * PI * 0.5f, a1 = (q + 1) * PI * 0.5f, r0 = 0.24f, r1 = 0.14f;
                Vector3 o0 = { cosf(a0), 0, sinf(a0) }, o1 = { cosf(a1), 0, sinf(a1) };
                MB_Quad(b, Vector3Add(b0, Vector3Scale(o0, r0)), Vector3Add(b1, Vector3Scale(o0, r1)),
                        Vector3Add(b1, Vector3Scale(o1, r1)), Vector3Add(b0, Vector3Scale(o1, r0)), bark);
                (void)dv;
            }
        }
    }
    int puffs = lod == 0 ? 3 : 1;
    for (int k = 0; k < puffs; k++) {
        float a = k * 2.1f + Rand01(&s) * 0.8f, rr = puffs == 1 ? h * 0.42f : h * (0.27f + Rand01(&s) * 0.06f);
        float off = puffs == 1 ? 0 : h * 0.17f;
        MB_Blob(b, (Vector3){ p.x + cosf(a) * off, p.y + h * 0.66f + Rand01(&s) * h * 0.08f, p.z + sinf(a) * off },
                (Vector3){ rr, rr * 0.62f, rr }, lod == 0 ? 3 : 2, ColorJitter(leaf, seed + k, 14), 0.35f, seed + k * 13);
    }
}

static void bush(MeshBuf *b, Vector3 p, float r, unsigned seed, Color base)
{
    MB_Blob(b, (Vector3){ p.x, p.y + r * 0.45f, p.z }, (Vector3){ r, r * 0.65f, r }, 2, ColorJitter(base, seed, 14), 0.4f, seed);
}

static float treeObstacleRadius(int type) { return type == 3 ? 0.5f : type == 1 ? 0.55f : 0.45f; }

static void placeTree(Chunk *c, MeshBuf *d, int type, Vector3 p, unsigned seed, int lod, float routeDist)
{
    unsigned s = seed;
    float h;
    switch (type) {
    case 1: h = 14 + Rand01(&s) * 6; treePlane(d, p, h, seed, lod); break;
    case 2: h = 9 + Rand01(&s) * 7; treePine(d, p, h, seed, lod); break;
    case 3: h = 9 + Rand01(&s) * 6; treeCypress(d, p, h, seed, lod); break;
    case 4: h = 3.5f + Rand01(&s) * 2; treeOlive(d, p, h, seed, lod); break;
    default: h = 6 + Rand01(&s) * 4; treeOak(d, p, h, seed, lod); break;
    }
    if (routeDist < 60) addObs(c, p.x, p.z, treeObstacleRadius(type), 0);
}

static void vineRows(MeshBuf *d, const Chunk *c, float cx, float cz, float cd, unsigned seed)
{
    unsigned blk = Hash2((int)floorf(cx / 45.0f), (int)floorf(cz / 45.0f));
    float ang = (blk & 1) ? 0.0f : PI * 0.5f;
    ang += ((blk >> 3) & 7) * 0.03f;
    static const Color autumn[4] = { { 168, 150, 58, 255 }, { 176, 112, 52, 255 }, { 138, 136, 62, 255 }, { 150, 82, 50, 255 } };
    Color base = autumn[(blk >> 6) & 3];
    float ca = cosf(ang), sa = sinf(ang);
    for (int k = 0; k < 4; k++) {
        float off = -3.75f + k * 2.5f;
        for (int half = 0; half < 2; half++) {
            float along = -2.5f + half * 5.0f;
            float x = cx + ca * along - sa * off, z = cz + sa * along + ca * off;
            float y = groundC(c, x, z, cd);
            MB_Box(d, (Vector3){ x, y + 0.5f, z }, (Vector3){ 2.45f, 0.5f, 0.32f }, -ang, ColorJitter(base, seed + k * 3 + half, 16));
        }
    }
}

// ------------------------------------------------------------------ terrain + vegetation
static void vertexNormalRaw(int i, int j, Vector3 *n)
{
    const Grid *g = &W.dem;
    int i0 = i > 0 ? i - 1 : i, i1 = i < g->nx - 1 ? i + 1 : i;
    int j0 = j > 0 ? j - 1 : j, j1 = j < g->nz - 1 ? j + 1 : j;
    float hx = (g->h[j * g->nx + i1] - g->h[j * g->nx + i0]) * 0.1f / ((i1 - i0) * g->cell);
    float hz = (g->h[j1 * g->nx + i] - g->h[j0 * g->nx + i]) * 0.1f / ((j1 - j0) * g->cell);
    *n = Vector3Normalize((Vector3){ -hx, 1, -hz });
}

static void buildTerrain(Chunk *c, MeshBuf *t, MeshBuf *d)
{
    const Grid *g = &W.dem;
    for (int j = c->cz0; j <= c->cz1; j++) {
        for (int i = c->cx0; i <= c->cx1; i++) {
            int k = j * gnx + i;
            if (owner[k] != c->id) continue;
            float cd = cdist[k];
            float x0 = gx0 + i * gcell, z0 = gz0 + j * gcell;
            int sub = cd < NEAR_SUBDIV ? 2 : 1;
            float step = gcell / sub;
            bool conform = cd < CONFORM_ZONE + gcell;
            Vector3 P[3][3], N[3][3];
            Color C[3][3];
            for (int b = 0; b <= sub; b++) {
                for (int a = 0; a <= sub; a++) {
                    float x = x0 + a * step, z = z0 + b * step;
                    float rd = -1, hw = 0;
                    if (conform) {
                        float dd;
                        float y = conformed(x, z, c->mid, c->win, &dd);
                        float e = 1.5f;
                        float yx = conformed(x + e, z, c->mid, c->win, NULL) - conformed(x - e, z, c->mid, c->win, NULL);
                        float yz = conformed(x, z + e, c->mid, c->win, NULL) - conformed(x, z - e, c->mid, c->win, NULL);
                        P[b][a] = (Vector3){ x, y, z };
                        N[b][a] = Vector3Normalize((Vector3){ -yx / (2 * e), 1, -yz / (2 * e) });
                        rd = fabsf(dd);
                        int seg;
                        float s = RouteProject(x, z, c->mid, c->win, NULL, &seg);
                        float w;
                        RouteSample(s, NULL, NULL, NULL, &w, NULL);
                        hw = w * 0.5f;
                    } else if (sub == 1) {
                        int gi = i + a, gj = j + b;
                        P[b][a] = (Vector3){ x, g->h[gj * g->nx + gi] * 0.1f, z };
                        vertexNormalRaw(gi, gj, &N[b][a]);
                    } else {
                        P[b][a] = (Vector3){ x, DemHeight(x, z), z };
                        float e = 2.0f;
                        N[b][a] = Vector3Normalize((Vector3){ -(DemHeight(x + e, z) - DemHeight(x - e, z)) / (2 * e), 1,
                                                              -(DemHeight(x, z + e) - DemHeight(x, z - e)) / (2 * e) });
                    }
                    if (GridClass(&W.lcov, x, z) == LC_WATER && (rd < 0 || rd > hw + SHOULDER + BLEND)) P[b][a].y -= 2.4f;
                    C[b][a] = terrainColor(x, z, N[b][a].y, rd, hw);
                }
            }
            for (int b = 0; b < sub; b++) {
                for (int a = 0; a < sub; a++) {
                    MB_TriN(t, P[b][a], P[b + 1][a], P[b + 1][a + 1], N[b][a], N[b + 1][a], N[b + 1][a + 1], C[b][a], C[b + 1][a], C[b + 1][a + 1]);
                    MB_TriN(t, P[b][a], P[b + 1][a + 1], P[b][a + 1], N[b][a], N[b + 1][a + 1], N[b][a + 1], C[b][a], C[b + 1][a + 1], C[b][a + 1]);
                }
            }

            // ---- vegetation for this cell
            if (mask[k]) continue;
            float cx = x0 + gcell * 0.5f, cz = z0 + gcell * 0.5f;
            int cls = GridClass(&W.lcov, cx, cz);
            unsigned seed = Hash2(i * 7 + 3, j * 13 + 1);
            unsigned s = seed;
            int lod = cd < TREE_NEAR ? 0 : cd < TREE_MID ? 1 : 2;
            if (lod == 2 && (cd > TREE_FAR || gQuality == 0)) continue;
            float clear = 0;
            if (cd < 40) {
                // keep the road and its verges clear
                float dd;
                RouteProject(cx, cz, c->mid, c->win, &dd, NULL);
                float w;
                RouteSample(RouteProject(cx, cz, c->mid, c->win, NULL, NULL), NULL, NULL, NULL, &w, NULL);
                clear = fabsf(dd) - (w * 0.5f + SHOULDER + 3.5f);
                if (clear < 2.0f) continue;
            }
            float r1 = Rand01(&s), r2 = Rand01(&s), r3 = Rand01(&s);
            float px = x0 + gcell * (0.15f + 0.7f * r1), pz = z0 + gcell * (0.15f + 0.7f * r2);
            Vector3 p = { px, groundC(c, px, pz, cd), pz };
            switch (cls) {
            case LC_FOREST:
                if (lod == 2) { if (r3 < 0.3f) placeTree(c, d, r3 < 0.2f ? 2 : 0, p, seed, 2, cd); break; }
                placeTree(c, d, r3 < 0.65f ? 2 : 0, p, seed, lod, cd);
                if (lod == 0 && Rand01(&s) < 0.25f) {
                    float qx = x0 + gcell * Rand01(&s), qz = z0 + gcell * Rand01(&s);
                    bush(d, (Vector3){ qx, groundC(c, qx, qz, cd), qz }, 0.7f + Rand01(&s) * 0.6f, seed + 5, (Color){ 84, 98, 58, 255 });
                }
                break;
            case LC_SHRUB:
                if (lod == 2) break;
                bush(d, p, 0.6f + r3 * 0.7f, seed, (Color){ 104, 112, 72, 255 });
                if (lod == 0) {
                    float qx = x0 + gcell * Rand01(&s), qz = z0 + gcell * Rand01(&s);
                    bush(d, (Vector3){ qx, groundC(c, qx, qz, cd), qz }, 0.5f + Rand01(&s) * 0.5f, seed + 9, (Color){ 122, 120, 82, 255 });
                }
                if (Rand01(&s) < 0.12f) placeTree(c, d, Rand01(&s) < 0.6f ? 2 : 0, (Vector3){ px + 2, p.y, pz - 2 }, seed + 3, lod, cd);
                break;
            case LC_GRASS:
            case LC_UNKNOWN:
                if (lod < 2 && r3 < 0.08f) placeTree(c, d, r3 < 0.03f ? 2 : 0, p, seed, lod, cd);
                else if (lod == 0 && r3 < 0.2f) bush(d, p, 0.6f, seed, (Color){ 100, 112, 66, 255 });
                break;
            case LC_CROP:
                if (lod < 2 && r3 < 0.025f) placeTree(c, d, r3 < 0.012f ? 3 : 0, p, seed, lod, cd);
                break;
            case LC_VINEYARD:
                if (cd < 320) vineRows(d, c, cx, cz, cd, seed);
                break;
            case LC_ORCHARD:
                if (lod < 2) {
                    for (int q = 0; q < 2; q++) {
                        float qx = x0 + gcell * (0.25f + 0.5f * q) + (Rand01(&s) - 0.5f), qz = z0 + gcell * (0.3f + 0.4f * q) + (Rand01(&s) - 0.5f);
                        placeTree(c, d, 4, (Vector3){ qx, groundC(c, qx, qz, cd), qz }, seed + q * 31, lod, cd);
                    }
                }
                break;
            case LC_RESIDENTIAL:
                if (lod < 2 && r3 < 0.32f) {
                    float r4 = Rand01(&s);
                    placeTree(c, d, r4 < 0.3f ? 3 : r4 < 0.55f ? 2 : r4 < 0.8f ? 1 : 0, p, seed, lod, cd);
                } else if (lod == 0 && r3 < 0.5f) bush(d, p, 0.8f, seed, (Color){ 70, 96, 52, 255 });
                break;
            case LC_BARE:
                if (lod == 0 && r3 < 0.1f) MB_Blob(d, p, (Vector3){ 1.0f, 0.6f, 0.8f }, 2, (Color){ 196, 188, 168, 255 }, 0.4f, seed);
                break;
            default: break;
            }
        }
    }
}

// ------------------------------------------------------------------ road
static void stripe(MeshBuf *r, int i0, int i1, float dOff, float width, float dash, float gap, Color col, bool (*want)(const RoutePt *))
{
    for (int k = i0; k < i1; k++) {
        const RoutePt *a = &W.route[k], *b = &W.route[k + 1];
        if (want && !want(a)) continue;
        if (dash > 0) {
            float sm = (a->s + b->s) * 0.5f;
            if (fmodf(sm, dash + gap) > dash) continue;
        }
        float ra = dOff, rb = dOff;
        Vector3 ra0 = { a->x - a->hz * (ra - width * 0.5f), a->y + 0.045f, a->z + a->hx * (ra - width * 0.5f) };
        Vector3 ra1 = { a->x - a->hz * (ra + width * 0.5f), a->y + 0.045f, a->z + a->hx * (ra + width * 0.5f) };
        Vector3 rb0 = { b->x - b->hz * (rb - width * 0.5f), b->y + 0.045f, b->z + b->hx * (rb - width * 0.5f) };
        Vector3 rb1 = { b->x - b->hz * (rb + width * 0.5f), b->y + 0.045f, b->z + b->hx * (rb + width * 0.5f) };
        MB_Quad(r, ra0, ra1, rb1, rb0, col);
    }
}

static bool isTwoWay(const RoutePt *p) { return !(p->flags & RF_ONEWAY) && p->lanesForward < p->lanes && !(p->flags & RF_ROUNDABOUT); }
static bool twoWayStraight(const RoutePt *p) { return isTwoWay(p) && fabsf(p->curv) < 1.0f / 280.0f; }
static bool twoWayCurve(const RoutePt *p) { return isTwoWay(p) && fabsf(p->curv) >= 1.0f / 280.0f; }
static bool ruralEdge(const RoutePt *p) { return p->roadClass < RC_MOTORWAY && !(p->flags & RF_BUILTUP); }
static bool isMotorway(const RoutePt *p) { return p->roadClass >= RC_MOTORWAY; }

// lateral extents of the paved surface (left is negative)
static void pavedExtents(const RoutePt *p, float *l, float *rr)
{
    float hw = p->width * 0.5f;
    *l = -hw; *rr = hw;
    if (p->roadClass >= RC_MOTORWAY) { *l -= 0.9f; *rr += 2.6f; }
}

static Vector3 lat(const RoutePt *p, float d, float dy)
{
    return (Vector3){ p->x - p->hz * d, p->y + dy, p->z + p->hx * d };
}

static void buildRoad(Chunk *c, MeshBuf *r, MeshBuf *d)
{
    int i0 = c->i0, i1 = c->i1;
    for (int k = i0; k < i1; k++) {
        const RoutePt *a = &W.route[k], *b = &W.route[k + 1];
        float la, ra, lb, rb;
        pavedExtents(a, &la, &ra);
        pavedExtents(b, &lb, &rb);
        unsigned h = Hash2((int)(a->s / 24.0f), 77);
        Color asphalt = a->roadClass >= RC_MOTORWAY ? (Color){ 74, 76, 80, 255 } : (Color){ 86, 87, 90, 255 };
        asphalt = ColorJitter(asphalt, h, 8);
        MB_Quad(r, lat(a, la, 0.03f), lat(a, ra, 0.03f), lat(b, rb, 0.03f), lat(b, lb, 0.03f), asphalt);
        Color gravel = { 146, 138, 118, 255 };
        MB_Quad(r, lat(a, la - SHOULDER, -0.14f), lat(a, la, 0.03f), lat(b, lb, 0.03f), lat(b, lb - SHOULDER, -0.14f), gravel);
        MB_Quad(r, lat(a, ra, 0.03f), lat(a, ra + SHOULDER, -0.14f), lat(b, rb + SHOULDER, -0.14f), lat(b, rb, 0.03f), gravel);
    }
    Color white = { 232, 232, 226, 255 };
    // centre line of two-way roads: dashed on straights, continuous in bends
    stripe(r, i0, i1, 0, 0.13f, 4.0f, 10.0f, white, twoWayStraight);
    stripe(r, i0, i1, 0, 0.13f, 0, 0, white, twoWayCurve);
    // edge lines on rural roads
    for (int k = i0; k < i1; k++) {
        const RoutePt *a = &W.route[k];
        if (a->roadClass >= RC_MOTORWAY) {
            float hw = a->width * 0.5f;
            stripe(r, k, k + 1, -hw + 0.12f, 0.2f, 0, 0, white, NULL);
            stripe(r, k, k + 1, hw, 0.25f, 0, 0, white, NULL);
            int lanes = a->lanes;
            for (int ln = 1; ln < lanes; ln++) stripe(r, k, k + 1, -hw + a->width * ln / lanes, 0.15f, 12.0f, 14.0f, white, NULL);
        } else if (ruralEdge(a)) {
            float hw = a->width * 0.5f;
            stripe(r, k, k + 1, -hw + 0.3f, 0.12f, (a->flags & RF_ONEWAY) ? 0 : 0, 0, white, NULL);
            stripe(r, k, k + 1, hw - 0.3f, 0.12f, 0, 0, white, NULL);
        }
    }

    // start line and chequered finish line
    for (int k = i0; k < i1; k++) {
        const RoutePt *a = &W.route[k], *b = &W.route[k + 1];
        bool start = a->s >= 3.0f && a->s < 5.0f, fin = a->s >= W.length - 6.0f && a->s < W.length - 2.0f;
        if (!start && !fin) continue;
        float la, ra;
        pavedExtents(a, &la, &ra);
        int n = 12;
        for (int q = 0; q < n; q++) {
            if (start && q % 2) continue;
            bool blackSq = ((q + (int)(a->s / 1.0f)) % 2) == 0;
            Color col = start ? (Color){ 236, 236, 230, 255 } : (blackSq ? (Color){ 25, 25, 28, 255 } : (Color){ 240, 240, 236, 255 });
            float d0 = la + (ra - la) * q / n, d1 = la + (ra - la) * (q + 1) / n;
            MB_Quad(r, lat(a, d0, 0.05f), lat(a, d1, 0.05f), lat(b, d1, 0.05f), lat(b, d0, 0.05f), col);
        }
    }
    // ---- roadside furniture
    for (int k = i0; k < i1; k++) {
        if (W.route[k].flags & RF_BRIDGE) {
            const RoutePt *a = &W.route[k], *b = &W.route[k + 1];
            float la, ra, lb, rb;
            pavedExtents(a, &la, &ra);
            pavedExtents(b, &lb, &rb);
            Color conc = { 196, 190, 178, 255 };
            Vector3 rightA = { -a->hz, 0, a->hx }, leftA = { a->hz, 0, -a->hx };
            for (int side = -1; side <= 1; side += 2) {
                float da = side < 0 ? la - 0.5f : ra + 0.5f, db = side < 0 ? lb - 0.5f : rb + 0.5f;
                Vector3 in = side < 0 ? rightA : leftA;
                quadFacing(d, lat(a, da, -0.2f), lat(b, db, -0.2f), lat(b, db, 0.95f), lat(a, da, 0.95f), in, conc);
                quadFacing(d, lat(a, da + side * 0.3f, -1.5f), lat(b, db + side * 0.3f, -1.5f), lat(b, db + side * 0.3f, 0.95f), lat(a, da + side * 0.3f, 0.95f), Vector3Negate(in), ColorBrightness(conc, -0.1f));
                quadFacing(d, lat(a, da, 0.95f), lat(b, db, 0.95f), lat(b, db + side * 0.3f, 0.95f), lat(a, da + side * 0.3f, 0.95f), (Vector3){ 0, 1, 0 }, ColorBrightness(conc, 0.08f));
            }
        }
    }
    for (int k = i0; k < i1; k++) {
        const RoutePt *a = &W.route[k], *b = &W.route[k + 1];
        float la, ra;
        pavedExtents(a, &la, &ra);
        int seg50a = (int)(a->s / 50.0f), seg50b = (int)(b->s / 50.0f);
        if (a->roadClass >= RC_MOTORWAY) {
            // guard rail on the right, concrete barrier on the left
            float lb, rb;
            pavedExtents(b, &lb, &rb);
            Color steel = { 176, 180, 184, 255 };
            Vector3 rightA = { -a->hz, 0, a->hx };
            Vector3 leftA = { a->hz, 0, -a->hx };
            float dr = ra + 0.8f, dr2 = rb + 0.8f;
            quadFacing(d, lat(a, dr, 0.45f), lat(b, dr2, 0.45f), lat(b, dr2, 0.78f), lat(a, dr, 0.78f), leftA, steel);
            quadFacing(d, lat(a, dr + 0.05f, 0.45f), lat(b, dr2 + 0.05f, 0.45f), lat(b, dr2 + 0.05f, 0.78f), lat(a, dr + 0.05f, 0.78f), rightA, ColorBrightness(steel, -0.15f));
            if ((int)(a->s / 4.0f) != (int)(b->s / 4.0f)) {
                Vector3 pp = lat(a, dr + 0.1f, 0.38f);
                MB_Box(d, pp, (Vector3){ 0.06f, 0.4f, 0.06f }, atan2f(a->hx, a->hz), (Color){ 120, 122, 126, 255 });
            }
            Color conc = { 188, 184, 174, 255 };
            float dl = la - 0.55f, dl2 = lb - 0.55f;
            quadFacing(d, lat(a, dl + 0.3f, -0.1f), lat(b, dl2 + 0.3f, -0.1f), lat(b, dl2 + 0.1f, 0.82f), lat(a, dl + 0.1f, 0.82f), rightA, conc);
            quadFacing(d, lat(a, dl - 0.3f, -0.1f), lat(b, dl2 - 0.3f, -0.1f), lat(b, dl2 - 0.1f, 0.82f), lat(a, dl - 0.1f, 0.82f), leftA, ColorBrightness(conc, -0.1f));
            quadFacing(d, lat(a, dl + 0.1f, 0.82f), lat(a, dl - 0.1f, 0.82f), lat(b, dl2 - 0.1f, 0.82f), lat(b, dl2 + 0.1f, 0.82f), (Vector3){ 0, 1, 0 }, ColorBrightness(conc, 0.08f));
        } else if (!(a->flags & RF_BUILTUP)) {
            // French delineator posts (balises J6) every 50 m on both sides
            if (seg50a != seg50b) {
                for (int side = -1; side <= 1; side += 2) {
                    float dd = side < 0 ? la - SHOULDER - 0.2f : ra + SHOULDER + 0.2f;
                    Vector3 pp = lat(a, dd, 0.0f);
                    pp.y = a->y - 0.1f;
                    float yaw = atan2f(a->hx, a->hz);
                    MB_Box(d, (Vector3){ pp.x, pp.y + 0.5f, pp.z }, (Vector3){ 0.07f, 0.5f, 0.05f }, yaw, (Color){ 236, 236, 230, 255 });
                    MB_Box(d, (Vector3){ pp.x, pp.y + 0.86f, pp.z }, (Vector3){ 0.075f, 0.1f, 0.055f }, yaw, (Color){ 30, 30, 32, 255 });
                    Color refl = side > 0 ? (Color){ 255, 150, 30, 255 } : (Color){ 245, 245, 245, 255 };
                    MB_Box(d, (Vector3){ pp.x - a->hx * 0.06f, pp.y + 0.86f, pp.z - a->hz * 0.06f }, (Vector3){ 0.04f, 0.05f, 0.01f }, yaw, refl);
                    addObs(c, pp.x, pp.z, 0.12f, 2);
                }
            }
        } else {
            // street lamps in villages
            int s35a = (int)(a->s / 35.0f), s35b = (int)(b->s / 35.0f);
            if (s35a != s35b) {
                float dd = ra + SHOULDER + 0.6f;
                Vector3 pp = lat(a, dd, 0.0f);
                pp.y = a->y - 0.2f;
                MB_Cylinder(d, pp, 0.09f, 0.06f, 6.2f, 5, (Color){ 70, 74, 70, 255 }, false);
                Vector3 arm = lat(a, dd - 1.2f, 0.0f);
                arm.y = pp.y + 6.0f;
                MB_Box(d, (Vector3){ (pp.x + arm.x) * 0.5f, arm.y + 0.05f, (pp.z + arm.z) * 0.5f }, (Vector3){ 0.6f, 0.05f, 0.05f }, atan2f(a->hz, -a->hx) + PI * 0.5f, (Color){ 70, 74, 70, 255 });
                MB_Box(d, (Vector3){ arm.x, arm.y - 0.1f, arm.z }, (Vector3){ 0.25f, 0.08f, 0.15f }, atan2f(a->hx, a->hz), (Color){ 220, 220, 200, 255 });
                addObs(c, pp.x, pp.z, 0.18f, 2);
            }
        }
    }
}

// ------------------------------------------------------------------ side roads
static void buildSideRoad(Chunk *c, MeshBuf *r, const RoadPiece *pc)
{
    const SideRoad *sr = &W.roads[pc->road];
    int n = pc->b - pc->a + 1;
    if (n < 2) return;
    // densify
    int cap = 0;
    for (int k = pc->a; k < pc->b; k++) {
        float dx = sr->xz[2 * (k + 1)] - sr->xz[2 * k], dz = sr->xz[2 * (k + 1) + 1] - sr->xz[2 * k + 1];
        cap += 1 + (int)(sqrtf(dx * dx + dz * dz) / 5.0f);
    }
    cap += 2;
    float *px = malloc(sizeof(float) * cap * 2);
    int m = 0;
    for (int k = pc->a; k < pc->b; k++) {
        float ax = sr->xz[2 * k], az = sr->xz[2 * k + 1], bx = sr->xz[2 * (k + 1)], bz = sr->xz[2 * (k + 1) + 1];
        float L = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
        int steps = 1 + (int)(L / 5.0f);
        for (int q = 0; q < steps; q++) { px[2 * m] = ax + (bx - ax) * q / steps; px[2 * m + 1] = az + (bz - az) * q / steps; m++; }
    }
    px[2 * m] = sr->xz[2 * pc->b]; px[2 * m + 1] = sr->xz[2 * pc->b + 1]; m++;

    bool bridge = sr->flags & 4;
    float yA = 0, yB = 0;
    if (bridge) { yA = SceneGround(px[0], px[1], c->mid); yB = SceneGround(px[2 * (m - 1)], px[2 * (m - 1) + 1], c->mid); }
    bool dirt = sr->roadClass == 0 && sr->width < 4.0f;
    Color col = dirt ? (Color){ 156, 138, 104, 255 } : (sr->roadClass >= RC_MOTORWAY ? (Color){ 74, 76, 80, 255 } : (Color){ 92, 93, 96, 255 });
    float hw = sr->width * 0.5f;
    Vector3 prevL = { 0 }, prevR = { 0 };
    for (int q = 0; q < m; q++) {
        int qa = q > 0 ? q - 1 : q, qb = q < m - 1 ? q + 1 : q;
        float dx = px[2 * qb] - px[2 * qa], dz = px[2 * qb + 1] - px[2 * qa + 1];
        float L = sqrtf(dx * dx + dz * dz);
        if (L < 1e-4f) { dx = 1; dz = 0; L = 1; }
        dx /= L; dz /= L;
        float x = px[2 * q], z = px[2 * q + 1];
        float y;
        if (bridge) y = yA + (yB - yA) * q / (float)(m - 1) + 0.1f;
        else y = SceneGround(x, z, c->mid) + 0.07f;
        // keep side roads just under our own road where they meet it
        float cd = SceneRouteDist(x, z);
        if (cd < 25) {
            float dd;
            int seg;
            float s = RouteProject(x, z, c->mid, c->win, &dd, &seg);
            Vector3 rp;
            float w;
            RouteSample(s, &rp, NULL, NULL, &w, NULL);
            if (fabsf(dd) < w * 0.5f + 1.0f && !bridge) y = fminf(y, rp.y - 0.05f);
        }
        Vector3 Lp = { x + dz * hw, y, z - dx * hw }, Rp = { x - dz * hw, y, z + dx * hw };
        if (q > 0) MB_Quad(r, prevR, Rp, Lp, prevL, col);
        prevL = Lp; prevR = Rp;
    }
    free(px);
}

// ------------------------------------------------------------------ buildings
static float polyArea(const float *xz, int n)
{
    float a = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        a += xz[2 * i] * xz[2 * j + 1] - xz[2 * j] * xz[2 * i + 1];
    }
    return a * 0.5f;   // in (x,z) coords: positive = clockwise seen from above
}

static bool pointInTri(float px, float pz, float ax, float az, float bx, float bz, float cx, float cz)
{
    float d1 = (px - bx) * (az - bz) - (ax - bx) * (pz - bz);
    float d2 = (px - cx) * (bz - cz) - (bx - cx) * (pz - cz);
    float d3 = (px - ax) * (cz - az) - (cx - ax) * (pz - az);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0), pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

// ear clipping; poly given with positive polyArea orientation; emits triangles facing +Y
static void flatRoof(MeshBuf *b, const float *xz, int n, float y, Color col)
{
    int *idx = malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) idx[i] = i;
    int cnt = n, guard = 0;
    while (cnt > 2 && guard++ < n * n) {
        bool clipped = false;
        for (int i = 0; i < cnt; i++) {
            int ia = idx[(i + cnt - 1) % cnt], ib = idx[i], ic = idx[(i + 1) % cnt];
            float ax = xz[2 * ia], az = xz[2 * ia + 1], bx = xz[2 * ib], bz = xz[2 * ib + 1], cx = xz[2 * ic], cz = xz[2 * ic + 1];
            float cr = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
            if (cr <= 0) continue;     // reflex for this orientation
            bool ear = true;
            for (int q = 0; q < cnt && ear; q++) {
                int iq = idx[q];
                if (iq == ia || iq == ib || iq == ic) continue;
                if (pointInTri(xz[2 * iq], xz[2 * iq + 1], ax, az, bx, bz, cx, cz)) ear = false;
            }
            if (!ear) continue;
            // (a,b,c) is clockwise seen from above in (x,z) -> for +Y normal emit (a,c,b)
            MB_Tri(b, (Vector3){ ax, y, az }, (Vector3){ cx, y, cz }, (Vector3){ bx, y, bz }, col);
            for (int q = i; q < cnt - 1; q++) idx[q] = idx[q + 1];
            cnt--;
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    free(idx);
}

static void buildBuilding(MeshBuf *b, const Building *bd, float routeDist, unsigned seed)
{
    int n = bd->nv;
    if (n < 3) return;
    float *xz = malloc(sizeof(float) * 2 * n);
    memcpy(xz, bd->xz, sizeof(float) * 2 * n);
    if (polyArea(xz, n) < 0) {   // normalise to a positive shoelace area in (x,z)
        for (int i = 0; i < n / 2; i++) {
            float tx = xz[2 * i], tz = xz[2 * i + 1];
            xz[2 * i] = xz[2 * (n - 1 - i)]; xz[2 * i + 1] = xz[2 * (n - 1 - i) + 1];
            xz[2 * (n - 1 - i)] = tx; xz[2 * (n - 1 - i) + 1] = tz;
        }
    }
    float gmin = 1e9f, gsum = 0;
    for (int i = 0; i < n; i++) {
        float g = DemHeight(xz[2 * i], xz[2 * i + 1]);
        if (g < gmin) gmin = g;
        gsum += g;
    }
    float base = gmin - 0.4f, eave = gsum / n + bd->height;
    if (bd->kind == 4) eave = gsum / n + fmaxf(bd->height, 9.0f);
    unsigned s = seed;
    static const Color walls[] = { { 232, 214, 178, 255 }, { 226, 198, 152, 255 }, { 216, 182, 132, 255 }, { 238, 226, 202, 255 },
                                   { 210, 176, 128, 255 }, { 226, 192, 170, 255 }, { 220, 206, 176, 255 } };
    static const Color shutters[] = { { 84, 124, 96, 255 }, { 74, 112, 150, 255 }, { 138, 128, 168, 255 }, { 124, 86, 56, 255 }, { 226, 226, 216, 255 }, { 96, 140, 150, 255 } };
    bool industrial = bd->kind == 2;
    Color wall = industrial ? ColorJitter((Color){ 196, 196, 190, 255 }, seed, 14) : walls[(int)(Rand01(&s) * 7) % 7];
    Color roofC = industrial ? (Color){ 150, 152, 154, 255 } : ColorJitter((Color){ 178, 94, 58, 255 }, seed + 1, 14);
    Color shut = shutters[(int)(Rand01(&s) * 6) % 6];
    float area = fabsf(polyArea(xz, n));

    // walls
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        Vector3 p0 = { xz[2 * i], base, xz[2 * i + 1] }, p1 = { xz[2 * j], base, xz[2 * j + 1] };
        Vector3 q0 = { p0.x, eave, p0.z }, q1 = { p1.x, eave, p1.z };
        MB_Quad(b, p1, p0, q0, q1, wall);   // outward for a positive-area polygon
        // windows and shutters on houses close to the road
        float ex = p1.x - p0.x, ez = p1.z - p0.z, L = sqrtf(ex * ex + ez * ez);
        if (industrial || routeDist > (gQuality ? 170 : 90) || L < 3.2f) continue;
        float nx = ez / L, nz = -ex / L;            // outward normal
        float ux = ex / L, uz = ez / L;
        int floors = (int)fmaxf(1, roundf(bd->height / 3.0f));
        int win = (int)((L - 1.0f) / 3.2f);
        if (win < 1) win = 1;
        for (int f = 0; f < floors; f++) {
            float y0 = gsum / n + 0.9f + f * 3.0f;
            if (y0 + 1.3f > eave - 0.3f) break;
            for (int w = 0; w < win; w++) {
                float t = (w + 0.5f) * L / win;
                float cx = p0.x + ux * t + nx * 0.04f, cz = p0.z + uz * t + nz * 0.04f;
                float hwid = 0.45f, hh = f == 0 && w == win / 2 && (seed & 1) ? 1.05f : 0.65f;
                float yy = f == 0 && hh > 0.9f ? gsum / n : y0;
                Vector3 a = { cx - ux * hwid, yy, cz - uz * hwid }, bb = { cx + ux * hwid, yy, cz + uz * hwid };
                Vector3 c2 = { bb.x, yy + 2 * hh, bb.z }, d2 = { a.x, yy + 2 * hh, a.z };
                quadFacing(b, a, bb, c2, d2, (Vector3){ nx, 0, nz }, (Color){ 52, 58, 66, 255 });
                if (hh < 0.9f) {
                    for (int sd = -1; sd <= 1; sd += 2) {
                        float o0 = sd * hwid, o1 = sd * (hwid + 0.42f);
                        Vector3 s0 = { cx + ux * fminf(o0, o1) + nx * 0.02f, y0 - 0.05f, cz + uz * fminf(o0, o1) + nz * 0.02f };
                        Vector3 s1 = { cx + ux * fmaxf(o0, o1) + nx * 0.02f, y0 - 0.05f, cz + uz * fmaxf(o0, o1) + nz * 0.02f };
                        quadFacing(b, s0, s1, (Vector3){ s1.x, y0 + 2 * hh + 0.05f, s1.z }, (Vector3){ s0.x, y0 + 2 * hh + 0.05f, s0.z }, (Vector3){ nx, 0, nz }, shut);
                    }
                }
            }
        }
    }
    // roof
    bool hip = !industrial && bd->roofShape != 0 && n == 4 && area < 900;
    bool convex = true;
    for (int i = 0; i < n && convex; i++) {
        int j = (i + 1) % n, k = (i + 2) % n;
        float cr = (xz[2 * j] - xz[2 * i]) * (xz[2 * k + 1] - xz[2 * i + 1]) - (xz[2 * j + 1] - xz[2 * i + 1]) * (xz[2 * k] - xz[2 * i]);
        if (cr < 0) convex = false;
    }
    if (hip) {
        // rectangle-ish: ridge along the longer axis
        float e0 = hypotf(xz[2] - xz[0], xz[3] - xz[1]), e1 = hypotf(xz[4] - xz[2], xz[5] - xz[3]);
        int a0 = e0 >= e1 ? 0 : 1;          // edge index of a long side
        float cxm = (xz[0] + xz[2] + xz[4] + xz[6]) * 0.25f, czm = (xz[1] + xz[3] + xz[5] + xz[7]) * 0.25f;
        float lx = xz[2 * (a0 + 1)] - xz[2 * a0], lz = xz[2 * (a0 + 1) + 1] - xz[2 * a0 + 1];
        float ll = sqrtf(lx * lx + lz * lz);
        lx /= ll; lz /= ll;
        float shortLen = a0 == 0 ? e1 : e0, longLen = a0 == 0 ? e0 : e1;
        float rh = shortLen * 0.27f, half = fmaxf(0, (longLen - shortLen) * 0.5f);
        Vector3 r0 = { cxm - lx * half, eave + rh, czm - lz * half }, r1 = { cxm + lx * half, eave + rh, czm + lz * half };
        // eave corners pushed out slightly (overhang)
        Vector3 E[4];
        for (int i = 0; i < 4; i++) {
            float dx = xz[2 * i] - cxm, dz = xz[2 * i + 1] - czm, dl = sqrtf(dx * dx + dz * dz);
            E[i] = (Vector3){ xz[2 * i] + dx / dl * 0.35f, eave - 0.12f, xz[2 * i + 1] + dz / dl * 0.35f };
        }
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            // the ridge end nearest to this edge's midpoint
            float mx = (E[i].x + E[j].x) * 0.5f, mz = (E[i].z + E[j].z) * 0.5f;
            bool longSide = ((i - a0) % 2 + 2) % 2 == 0;
            if (longSide) {
                // trapezoid: E[i], E[j], ridge end near E[j], ridge end near E[i]
                Vector3 ri = (Vector3Distance(E[i], r0) < Vector3Distance(E[i], r1)) ? r0 : r1;
                Vector3 rj = (Vector3Distance(E[j], r0) < Vector3Distance(E[j], r1)) ? r0 : r1;
                quadFacing(b, E[i], E[j], rj, ri, (Vector3){ mx - cxm, 4.0f, mz - czm }, roofC);
            } else {
                Vector3 rr = (Vector3DistanceSqr((Vector3){ mx, eave, mz }, r0) < Vector3DistanceSqr((Vector3){ mx, eave, mz }, r1)) ? r0 : r1;
                triFacing(b, E[i], E[j], rr, (Vector3){ mx - cxm, 4.0f, mz - czm }, ColorBrightness(roofC, -0.05f));
            }
        }
    } else if (!industrial && convex && n <= 10 && area < 500) {
        float cxm = 0, czm = 0;
        for (int i = 0; i < n; i++) { cxm += xz[2 * i]; czm += xz[2 * i + 1]; }
        cxm /= n; czm /= n;
        Vector3 apex = { cxm, eave + sqrtf(area) * 0.16f, czm };
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            triFacing(b, (Vector3){ xz[2 * i], eave, xz[2 * i + 1] }, (Vector3){ xz[2 * j], eave, xz[2 * j + 1] }, apex,
                      (Vector3){ (xz[2 * i] + xz[2 * j]) * 0.5f - cxm, 4.0f, (xz[2 * i + 1] + xz[2 * j + 1]) * 0.5f - czm }, roofC);
        }
    } else {
        flatRoof(b, xz, n, eave, industrial ? roofC : (Color){ 196, 190, 178, 255 });
    }
    free(xz);
}

// ------------------------------------------------------------------ chunk build
// CPU part (thread-safe: reads world data, writes only this chunk's buffers/obstacles)
static void buildChunkCPU(Chunk *c, MeshBuf *t, MeshBuf *r, MeshBuf *d)
{
    MB_Init(t, 16384);
    MB_Init(r, 4096);
    MB_Init(d, 16384);
    c->nObs = 0;
    buildTerrain(c, t, d);
    buildRoad(c, r, d);
    for (int k = 0; k < c->nPcs; k++) buildSideRoad(c, r, &pieces[c->pcs[k]]);
    for (int k = 0; k < c->nBld; k++) {
        const Building *bd = &W.buildings[c->bld[k]];
        float rd = SceneRouteDist(bd->cx, bd->cz);
        buildBuilding(d, bd, rd, Hash2(c->bld[k], 99));
        if (rd < 80) addObs(c, bd->cx, bd->cz, bd->radius * 0.85f, 1);
    }
    for (int k = 0; k < c->nTrees; k++) {
        const MappedTree *tr = &W.trees[c->trees[k]];
        float rd = SceneRouteDist(tr->x, tr->z);
        Vector3 p = { tr->x, SceneGround(tr->x, tr->z, c->mid), tr->z };
        placeTree(c, d, tr->type == 0 ? 0 : tr->type, p, Hash2(c->trees[k], 5), rd < TREE_NEAR ? 0 : 1, rd);
    }
}

// GPU part (main thread only)
static void uploadChunk(Chunk *c, MeshBuf *t, MeshBuf *r, MeshBuf *d)
{
    c->terrain = t->count ? MB_Upload(t) : (Mesh){ 0 };
    c->road = r->count ? MB_Upload(r) : (Mesh){ 0 };
    c->detail = d->count ? MB_Upload(d) : (Mesh){ 0 };
    MB_Free(t); MB_Free(r); MB_Free(d);
    c->built = true;
}

static void buildChunk(Chunk *c)
{
    MeshBuf t, r, d;
    buildChunkCPU(c, &t, &r, &d);
    uploadChunk(c, &t, &r, &d);
}

// ---- background builder thread
static pthread_t worker;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cvar = PTHREAD_COND_INITIALIZER;
static volatile int jobChunk = -1;
static volatile bool jobDone, workerQuit, workerOn;
static MeshBuf jobT, jobR, jobD;

static void *workerMain(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mtx);
        while (!workerQuit && (jobChunk < 0 || jobDone)) pthread_cond_wait(&cvar, &mtx);
        if (workerQuit) { pthread_mutex_unlock(&mtx); break; }
        int id = jobChunk;
        pthread_mutex_unlock(&mtx);
        buildChunkCPU(&chunks[id], &jobT, &jobR, &jobD);
        pthread_mutex_lock(&mtx);
        jobDone = true;
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static void unloadChunk(Chunk *c)
{
    if (c->terrain.vertexCount) UnloadMesh(c->terrain);
    if (c->road.vertexCount) UnloadMesh(c->road);
    if (c->detail.vertexCount) UnloadMesh(c->detail);
    c->terrain = c->road = c->detail = (Mesh){ 0 };
    c->built = false;
    c->nObs = 0;
}

// ------------------------------------------------------------------ far terrain (horizon)
static void buildFar(void)
{
    const Grid *g = &W.far;
    if (!g->h) return;
    // split into tiles of at most 120x120 cells (non indexed)
    int tile = 120, st = 2;   // every other sample (~300 m)
    for (int tj = 0; tj < g->nz - 1; tj += tile) {
        for (int ti = 0; ti < g->nx - 1; ti += tile) {
            if (nFar >= 16) break;
            MeshBuf b;
            MB_Init(&b, 6 * tile * tile);
            for (int j = tj; j < tj + tile && j < g->nz - st; j += st) {
                for (int i = ti; i < ti + tile && i < g->nx - st; i += st) {
                    Vector3 P[4];
                    Color C[4];
                    Vector3 N[4];
                    for (int q = 0; q < 4; q++) {
                        int gi = i + (q & 1) * st, gj = j + (q >> 1) * st;
                        float x = g->x0 + gi * g->cell, z = g->z0 + gj * g->cell;
                        float y = g->h[gj * g->nx + gi] * 0.1f;
                        float cd = SceneRouteDist(x, z);
                        if (cd < CORRIDOR + 60) y -= 45.0f * Clamp((CORRIDOR + 60 - cd) / 120.0f, 0, 1);
                        P[q] = (Vector3){ x, y, z };
                        int i0 = gi > 0 ? gi - 1 : gi, i1 = gi < g->nx - 1 ? gi + 1 : gi, j0 = gj > 0 ? gj - 1 : gj, j1 = gj < g->nz - 1 ? gj + 1 : gj;
                        float hx = (g->h[gj * g->nx + i1] - g->h[gj * g->nx + i0]) * 0.1f / ((i1 - i0) * g->cell);
                        float hz = (g->h[j1 * g->nx + gi] - g->h[j0 * g->nx + gi]) * 0.1f / ((j1 - j0) * g->cell);
                        N[q] = Vector3Normalize((Vector3){ -hx, 1, -hz });
                        int cls = GridClass(&W.farc, x, z);
                        if (y < 0.5f && cls == 0) cls = LC_WATER;
                        C[q] = classColor(cls, x, z);
                    }
                    MB_TriN(&b, P[0], P[2], P[3], N[0], N[2], N[3], C[0], C[2], C[3]);
                    MB_TriN(&b, P[0], P[3], P[1], N[0], N[3], N[1], C[0], C[3], C[1]);
                }
            }
            if (b.count) farMesh[nFar++] = MB_Upload(&b);
            MB_Free(&b);
        }
    }
}

// ------------------------------------------------------------------ init
void SceneInit(void)
{
    litMat = LoadMaterialDefault();
    litMat.shader = gLit;
    gx0 = W.dem.x0; gz0 = W.dem.z0; gcell = W.dem.cell;
    gnx = W.dem.nx - 1; gnz = W.dem.nz - 1;
    size_t ncell = (size_t)gnx * gnz;
    owner = malloc(sizeof(int16_t) * ncell);
    cdist = malloc(sizeof(float) * ncell);
    mask = calloc(ncell, 1);

    nChunks = (int)ceilf(W.length / CHUNK_LEN);
    if (nChunks < 1) nChunks = 1;
    chunks = calloc(nChunks, sizeof(Chunk));
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        c->id = i;
        c->s0 = i * CHUNK_LEN;
        c->s1 = fminf(W.length, (i + 1) * CHUNK_LEN);
        c->i0 = RouteIndexAtS(c->s0);
        c->i1 = RouteIndexAtS(c->s1) + 1;
        if (c->i1 > W.nRoute - 1) c->i1 = W.nRoute - 1;
        c->mid = (c->i0 + c->i1) / 2;
        c->win = (c->i1 - c->i0) / 2 + 60;
        c->cx0 = gnx; c->cz0 = gnz; c->cx1 = -1; c->cz1 = -1;
        c->minY = 1e9f; c->maxY = -1e9f;
    }

    // --- distance transform (chamfer) from the route, carrying the nearest chunk id
    for (size_t k = 0; k < ncell; k++) { cdist[k] = 1e9f; owner[k] = -1; }
    for (int i = 0; i < W.nRoute; i++) {
        int k = cellIndex(W.route[i].x, W.route[i].z, NULL, NULL);
        if (k < 0) continue;
        int ch = (int)(W.route[i].s / CHUNK_LEN);
        if (ch >= nChunks) ch = nChunks - 1;
        cdist[k] = 0;
        owner[k] = (int16_t)ch;
    }
    const float D1 = gcell, D2 = gcell * 1.41421f;
    for (int j = 0; j < gnz; j++) {
        for (int i = 0; i < gnx; i++) {
            int k = j * gnx + i;
            float best = cdist[k];
            int bo = owner[k];
            if (i > 0 && cdist[k - 1] + D1 < best) { best = cdist[k - 1] + D1; bo = owner[k - 1]; }
            if (j > 0 && cdist[k - gnx] + D1 < best) { best = cdist[k - gnx] + D1; bo = owner[k - gnx]; }
            if (i > 0 && j > 0 && cdist[k - gnx - 1] + D2 < best) { best = cdist[k - gnx - 1] + D2; bo = owner[k - gnx - 1]; }
            if (i < gnx - 1 && j > 0 && cdist[k - gnx + 1] + D2 < best) { best = cdist[k - gnx + 1] + D2; bo = owner[k - gnx + 1]; }
            cdist[k] = best; owner[k] = (int16_t)bo;
        }
    }
    for (int j = gnz - 1; j >= 0; j--) {
        for (int i = gnx - 1; i >= 0; i--) {
            int k = j * gnx + i;
            float best = cdist[k];
            int bo = owner[k];
            if (i < gnx - 1 && cdist[k + 1] + D1 < best) { best = cdist[k + 1] + D1; bo = owner[k + 1]; }
            if (j < gnz - 1 && cdist[k + gnx] + D1 < best) { best = cdist[k + gnx] + D1; bo = owner[k + gnx]; }
            if (i < gnx - 1 && j < gnz - 1 && cdist[k + gnx + 1] + D2 < best) { best = cdist[k + gnx + 1] + D2; bo = owner[k + gnx + 1]; }
            if (i > 0 && j < gnz - 1 && cdist[k + gnx - 1] + D2 < best) { best = cdist[k + gnx - 1] + D2; bo = owner[k + gnx - 1]; }
            cdist[k] = best; owner[k] = (int16_t)bo;
        }
    }
    for (int j = 0; j < gnz; j++) {
        for (int i = 0; i < gnx; i++) {
            int k = j * gnx + i;
            if (cdist[k] > CORRIDOR || owner[k] < 0) { owner[k] = -1; continue; }
            Chunk *c = &chunks[owner[k]];
            if (i < c->cx0) c->cx0 = i;
            if (i > c->cx1) c->cx1 = i;
            if (j < c->cz0) c->cz0 = j;
            if (j > c->cz1) c->cz1 = j;
            float h = W.dem.h[j * W.dem.nx + i] * 0.1f;
            if (h < c->minY) c->minY = h;
            if (h > c->maxY) c->maxY = h;
        }
    }
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (c->cx1 < 0) { c->cx0 = c->cz0 = 0; c->cx1 = c->cz1 = -1; c->minY = 0; c->maxY = 1; }
        c->box.min = (Vector3){ gx0 + c->cx0 * gcell, c->minY - 5, gz0 + c->cz0 * gcell };
        c->box.max = (Vector3){ gx0 + (c->cx1 + 1) * gcell, c->maxY + 40, gz0 + (c->cz1 + 1) * gcell };
    }

    // --- masks: buildings and side roads (no trees there)
    for (int b = 0; b < W.nBuildings; b++) {
        const Building *bd = &W.buildings[b];
        int k = cellIndex(bd->cx, bd->cz, NULL, NULL);
        if (k < 0 || owner[k] < 0 || cdist[k] > 650) continue;
        pushInt(&chunks[owner[k]].bld, &chunks[owner[k]].nBld, &chunks[owner[k]].capBld, b);
        int ci0, cj0, ci1, cj1;
        cellIndex(bd->cx - bd->radius, bd->cz - bd->radius, &ci0, &cj0);
        cellIndex(bd->cx + bd->radius, bd->cz + bd->radius, &ci1, &cj1);
        for (int j = cj0; j <= cj1; j++)
            for (int i = ci0; i <= ci1; i++)
                if (i >= 0 && j >= 0 && i < gnx && j < gnz) mask[j * gnx + i] |= 2;
    }
    for (int r = 0; r < W.nRoads; r++) {
        const SideRoad *sr = &W.roads[r];
        int start = -1, cur = -1;
        for (int p = 0; p < sr->np; p++) {
            float x = sr->xz[2 * p], z = sr->xz[2 * p + 1];
            int k = cellIndex(x, z, NULL, NULL);
            int o = k >= 0 ? owner[k] : -1;
            if (p + 1 < sr->np) {
                float bx = sr->xz[2 * (p + 1)], bz = sr->xz[2 * (p + 1) + 1];
                float L = hypotf(bx - x, bz - z);
                int steps = 1 + (int)(L / 3.0f);
                for (int q = 0; q <= steps; q++) {
                    float mx = x + (bx - x) * q / steps, mz = z + (bz - z) * q / steps;
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int kk = cellIndex(mx + dx * sr->width * 0.6f, mz + dz * sr->width * 0.6f, NULL, NULL);
                            if (kk >= 0) mask[kk] |= 1;
                        }
                }
            }
            if (o != cur || p == sr->np - 1) {
                int endp = (o != cur) ? p : p;
                if (cur >= 0 && start >= 0 && endp - start >= 1) {
                    pieces = realloc(pieces, sizeof(RoadPiece) * (nPieces + 1));
                    pieces[nPieces] = (RoadPiece){ r, start, endp };
                    pushInt(&chunks[cur].pcs, &chunks[cur].nPcs, &chunks[cur].capPcs, nPieces);
                    nPieces++;
                }
                start = p;
                cur = o;
            }
        }
    }
    for (int t = 0; t < W.nTrees; t++) {
        int k = cellIndex(W.trees[t].x, W.trees[t].z, NULL, NULL);
        if (k < 0 || owner[k] < 0 || cdist[k] > TREE_MID) continue;
        pushInt(&chunks[owner[k]].trees, &chunks[owner[k]].nTrees, &chunks[owner[k]].capTrees, t);
    }
    buildFar();
    jobChunk = -1; jobDone = false; workerQuit = false;
    workerOn = pthread_create(&worker, NULL, workerMain, NULL) == 0;
    TraceLog(LOG_INFO, "SCENE: %d chunks, %d side-road pieces, %d far tiles, async builder %s", nChunks, nPieces, nFar, workerOn ? "on" : "off");
}

void SceneShutdown(void)
{
    if (workerOn) {
        pthread_mutex_lock(&mtx);
        workerQuit = true;
        pthread_cond_broadcast(&cvar);
        pthread_mutex_unlock(&mtx);
        pthread_join(worker, NULL);
        workerOn = false;
        if (jobChunk >= 0 && jobDone) { MB_Free(&jobT); MB_Free(&jobR); MB_Free(&jobD); }
        jobChunk = -1; jobDone = false;
    }
    for (int i = 0; i < nChunks; i++) {
        unloadChunk(&chunks[i]);
        free(chunks[i].obs); free(chunks[i].bld); free(chunks[i].pcs); free(chunks[i].trees);
    }
    for (int i = 0; i < nFar; i++) UnloadMesh(farMesh[i]);
    nFar = 0;
    free(chunks); free(owner); free(cdist); free(mask); free(pieces);
    chunks = NULL; pieces = NULL; nPieces = 0;
}

static float boxDist2D(const BoundingBox *b, Vector3 p)
{
    float dx = fmaxf(fmaxf(b->min.x - p.x, 0), p.x - b->max.x);
    float dz = fmaxf(fmaxf(b->min.z - p.z, 0), p.z - b->max.z);
    return sqrtf(dx * dx + dz * dz);
}

static int pickNext(Vector3 focus)
{
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (c->built || c->cx1 < 0 || i == jobChunk) continue;
        float dd = boxDist2D(&c->box, focus);
        if (dd < LOAD_RADIUS && dd < bd) { bd = dd; best = i; }
    }
    return best;
}

void SceneUpdate(Vector3 focus, int maxBuild)
{
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (c->cx1 < 0 || i == jobChunk) continue;
        float dd = boxDist2D(&c->box, focus);
        if (c->built && dd > LOAD_RADIUS + 350) unloadChunk(c);
    }
    // finished background job -> upload
    if (workerOn && jobChunk >= 0) {
        pthread_mutex_lock(&mtx);
        bool done = jobDone;
        pthread_mutex_unlock(&mtx);
        if (done) {
            uploadChunk(&chunks[jobChunk], &jobT, &jobR, &jobD);
            pthread_mutex_lock(&mtx);
            jobChunk = -1; jobDone = false;
            pthread_mutex_unlock(&mtx);
        }
    }
    if (maxBuild < 0 && workerOn) {
        if (jobChunk < 0) {
            int next = pickNext(focus);
            if (next >= 0) {
                pthread_mutex_lock(&mtx);
                jobChunk = next; jobDone = false;
                pthread_cond_signal(&cvar);
                pthread_mutex_unlock(&mtx);
            }
        }
        return;
    }
    if (maxBuild < 0) maxBuild = 1;
    for (int n = 0; n < maxBuild; n++) {
        int best = pickNext(focus);
        if (best < 0) break;
        buildChunk(&chunks[best]);
    }
}

void SceneSetQuality(int q)
{
    gQuality = q;
    if (q == 0) { LOAD_RADIUS = 850; DRAW_RADIUS = 1000; TREE_NEAR = 80; TREE_MID = 200; TREE_FAR = 0; }
    else { LOAD_RADIUS = 1200; DRAW_RADIUS = 1450; TREE_NEAR = 110; TREE_MID = 300; TREE_FAR = 560; }
    // rebuild everything with the new settings
    for (int i = 0; i < nChunks; i++) if (chunks[i].built && i != jobChunk) unloadChunk(&chunks[i]);
}

int SceneQuality(void) { return gQuality; }

float SceneProgress(Vector3 focus)
{
    int want = 0, have = 0;
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (c->cx1 < 0) continue;
        if (boxDist2D(&c->box, focus) < LOAD_RADIUS) { want++; if (c->built) have++; }
    }
    return want ? (float)have / want : 1.0f;
}

void SceneDraw(Camera3D cam)
{
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    RenderSetMat(MAT_MATTE);
    // far terrain with lighter fog so the hills show on the horizon
    RenderSetFog(1.0f / 5200.0f);
    for (int i = 0; i < nFar; i++) DrawMesh(farMesh[i], litMat, MatrixIdentity());
    RenderSetFog(gQuality ? 1.0f / 1150.0f : 1.0f / 850.0f);
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!c->built) continue;
        if (boxDist2D(&c->box, cam.position) > DRAW_RADIUS) continue;
        // behind the camera?
        bool anyFront = false;
        for (int q = 0; q < 8 && !anyFront; q++) {
            Vector3 p = { (q & 1) ? c->box.max.x : c->box.min.x, (q & 2) ? c->box.max.y : c->box.min.y, (q & 4) ? c->box.max.z : c->box.min.z };
            if (Vector3DotProduct(Vector3Subtract(p, cam.position), fwd) > -30.0f) anyFront = true;
        }
        if (!anyFront) continue;
        if (c->terrain.vertexCount) DrawMesh(c->terrain, litMat, MatrixIdentity());
        if (c->detail.vertexCount) DrawMesh(c->detail, litMat, MatrixIdentity());
    }
    RenderSetMat(MAT_ROAD);
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!c->built || !c->road.vertexCount) continue;
        if (boxDist2D(&c->box, cam.position) > DRAW_RADIUS) continue;
        DrawMesh(c->road, litMat, MatrixIdentity());
    }
    RenderSetMat(MAT_MATTE);
}

void SceneStats(int *loaded, long *verts)
{
    int n = 0;
    long v = 0;
    for (int i = 0; i < nChunks; i++) {
        if (!chunks[i].built) continue;
        n++;
        v += chunks[i].terrain.vertexCount + chunks[i].road.vertexCount + chunks[i].detail.vertexCount;
    }
    long vt = 0, vr = 0, vd = 0;
    for (int i = 0; i < nChunks; i++) if (chunks[i].built) { vt += chunks[i].terrain.vertexCount; vr += chunks[i].road.vertexCount; vd += chunks[i].detail.vertexCount; }
    long vf = 0;
    for (int i = 0; i < nFar; i++) { v += farMesh[i].vertexCount; vf += farMesh[i].vertexCount; }
    (void)vt; (void)vr; (void)vd; (void)vf;
    *loaded = n;
    *verts = v;
}

bool SceneCollide(float x, float z, float radius, Obstacle *hit)
{
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!c->built || !c->nObs) continue;
        if (x < c->box.min.x - 50 || x > c->box.max.x + 50 || z < c->box.min.z - 50 || z > c->box.max.z + 50) continue;
        for (int k = 0; k < c->nObs; k++) {
            const Obstacle *o = &c->obs[k];
            float dx = o->x - x, dz = o->z - z, rr = o->r + radius;
            if (dx * dx + dz * dz < rr * rr) { if (hit) *hit = *o; return true; }
        }
    }
    return false;
}
