// Streaming world geometry built from world.bin: terrain conformed to the road, road surface and
// markings, roadside furniture, vegetation from land cover, real buildings, side roads, far terrain.
// Every chunk is 4 meshes: terrain (indexed, 4 blended ground textures), road, detail (opaque,
// textured walls/roofs/trunks) and foliage (alpha-tested leaf cards).
#include "scene.h"
#include "world.h"
#include "render.h"
#include "textures.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "thread.h"

#define CORRIDOR      720.0f   // near terrain extends this far from the route
#define NEAR_SUBDIV   50.0f    // cells closer than this are subdivided 2x2
#define CONFORM_ZONE  45.0f    // terrain within this distance is conformed to the road
#define SHOULDER      1.4f
#define BLEND         14.0f
#define GROUND_TILE   6.0f     // metres per ground texture repeat
#define ROAD_TILE     4.0f
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
    double shownAt;
    Mesh terrain, road, detail, foliage;
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
static Mesh midMesh[32];
static int nMid;
static Material matFlat, matTerrain, matRoad, matDetail, matFoliage, matDepth, matDepthCut;

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
static void quadFacingUV(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Vector3 hint, Color col, Vector2 ta, Vector2 tc, Vector2 td, Vector2 te)
{
    Vector3 n = Vector3CrossProduct(Vector3Subtract(c, a), Vector3Subtract(d, a));
    if (Vector3DotProduct(n, hint) < 0) MB_QuadUV(b, a, e, d, c, col, ta, te, td, tc);
    else MB_QuadUV(b, a, c, d, e, col, ta, tc, td, te);
}
static void triFacingUV(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 hint, Color col, Vector2 ta, Vector2 tc, Vector2 td)
{
    Vector3 n = Vector3CrossProduct(Vector3Subtract(c, a), Vector3Subtract(d, a));
    if (Vector3DotProduct(n, hint) < 0) { n = Vector3Normalize(Vector3Negate(n)); MB_TriUV(b, a, d, c, n, n, n, col, col, col, ta, td, tc); }
    else { n = Vector3Normalize(n); MB_TriUV(b, a, c, d, n, n, n, col, col, col, ta, tc, td); }
}

static Vector3 vec(float x, float y, float z) { return (Vector3){ x, y, z }; }
static float sstep(float a, float b, float x) { float t = Clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static Vector2 uv2(float u, float v) { return (Vector2){ u, v }; }
static Color tint(float r, float g, float b) { return (Color){ (unsigned char)Clamp(r * 255, 0, 255), (unsigned char)Clamp(g * 255, 0, 255), (unsigned char)Clamp(b * 255, 0, 255), 255 }; }

// ------------------------------------------------------------------ land cover
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

// flat colour of a land cover class (far and mid meshes, roughly the average of the textured ground)
static Color classColor(int cls, float x, float z)
{
    float n = vnoise(x, z, 37.0f), n2 = vnoise(x + 500, z - 300, 11.0f);
    Color c;
    switch (cls) {
    case LC_FOREST:      c = ColorLerp((Color){ 58, 74, 42, 255 }, (Color){ 76, 88, 52, 255 }, n); break;
    case LC_SHRUB:       c = ColorLerp((Color){ 118, 116, 84, 255 }, (Color){ 140, 134, 100, 255 }, n); break;
    case LC_GRASS:       c = ColorLerp((Color){ 118, 124, 66, 255 }, (Color){ 148, 140, 82, 255 }, n); break;
    case LC_CROP:        c = ColorLerp((Color){ 154, 132, 92, 255 }, (Color){ 178, 162, 108, 255 }, vnoise(x, z, 90.0f)); break;
    case LC_VINEYARD:    c = ColorLerp((Color){ 142, 114, 80, 255 }, (Color){ 158, 134, 94, 255 }, n); break;
    case LC_ORCHARD:     c = ColorLerp((Color){ 134, 126, 86, 255 }, (Color){ 148, 140, 100, 255 }, n); break;
    case LC_RESIDENTIAL: c = ColorLerp((Color){ 124, 128, 84, 255 }, (Color){ 150, 146, 112, 255 }, n2); break;
    case LC_INDUSTRIAL:  c = ColorLerp((Color){ 150, 146, 136, 255 }, (Color){ 170, 166, 154, 255 }, n2); break;
    case LC_BARE:        c = ColorLerp((Color){ 186, 176, 152, 255 }, (Color){ 206, 196, 172, 255 }, n2); break;
    case LC_WATER:       c = ColorLerp((Color){ 46, 92, 104, 255 }, (Color){ 70, 118, 126, 255 }, n2); break;
    case LC_WETLAND:     c = (Color){ 96, 112, 78, 255 }; break;
    case LC_PAVED:       c = (Color){ 120, 120, 116, 255 }; break;
    default:             c = ColorLerp((Color){ 126, 132, 76, 255 }, (Color){ 154, 148, 94, 255 }, n); break;
    }
    float k = 0.92f + 0.16f * n2;
    return (Color){ (unsigned char)Clamp(c.r * k, 0, 255), (unsigned char)Clamp(c.g * k, 0, 255), (unsigned char)Clamp(c.b * k, 0, 255), 255 };
}

// Textured ground: material id (0 grass, 1 garrigue, 2 ploughed soil, 3 limestone/bare; fractions blend),
// tint multiplied with the texture, flat = 1 for surfaces drawn with the vertex colour only (water, paving).
static void terrainMat(float x, float z, float ny, float roadD, float roadHalf, float *id, Color *col, float *flat)
{
    int cls = GridClass(&W.lcov, x, z);
    float n = vnoise(x, z, 70.0f), n2 = vnoise(x + 500, z - 300, 45.0f), n3 = vnoise(x - 900, z + 700, 60.0f);
    float m, r = 1, g = 1, b = 1;
    *flat = 0;
    switch (cls) {
    case LC_FOREST:      m = 1.0f + 0.3f * n3; r = 0.70f; g = 0.74f; b = 0.58f; break;           // litter under the trees
    case LC_SHRUB:       m = 1.0f + 0.4f * (n3 - 0.5f); r = 0.98f; g = 0.97f; b = 0.92f; break;
    case LC_GRASS:       m = 0.0f + 0.9f * n3 * n3; r = 0.97f; g = 0.98f; b = 0.90f; break;
    case LC_CROP:        m = 2.0f - 1.3f * sstep(0.45f, 0.7f, vnoise(x, z, 90.0f)); r = 1.0f; g = 0.98f; b = 0.90f; break;
    case LC_VINEYARD:    m = 2.0f; r = 0.95f; g = 0.90f; b = 0.84f; break;
    case LC_ORCHARD:     m = 0.5f + 0.5f * n3; r = 0.95f; g = 0.95f; b = 0.86f; break;
    case LC_RESIDENTIAL: m = 0.35f + 0.4f * n2; r = 0.95f; g = 0.97f; b = 0.90f; break;
    case LC_INDUSTRIAL:  m = 3.0f; r = 0.86f; g = 0.86f; b = 0.86f; break;
    case LC_BARE:        m = 3.0f; r = 1.0f; g = 1.0f; b = 0.98f; break;
    case LC_WETLAND:     m = 0.8f; r = 0.74f; g = 0.82f; b = 0.66f; break;
    case LC_WATER:       *flat = 1; m = 3; *col = classColor(cls, x, z); *id = m; return;
    case LC_PAVED:       *flat = 1; m = 3; *col = (Color){ 122, 122, 118, 255 }; *id = m; return;
    default:             m = 0.3f + 0.7f * n3; r = 0.96f; g = 0.96f; b = 0.88f; break;
    }
    // steep slopes: limestone shows through
    if (ny < 0.86f) { float k = Clamp((0.86f - ny) * 4.0f, 0, 0.85f); m = m + (3.0f - m) * k; r = r + (1 - r) * k; g = g + (1 - g) * k; b = b + (1 - b) * k; }
    // dry, trampled verge along the road
    if (roadD >= 0) {
        float e = roadD - roadHalf;
        if (e < 8.0f) {
            float k = 1 - Clamp((e - 1.0f) / 7.0f, 0, 1);
            m = m + (1.0f - m) * k;
            r = r + (0.96f - r) * k; g = g + (0.92f - g) * k; b = b + (0.80f - b) * k;
        }
    }
    float k = 0.92f + 0.14f * n2 + 0.08f * (n - 0.5f);
    *id = Clamp(m, 0, 3);
    *col = tint(r * k, g * k, b * k);
}

// ------------------------------------------------------------------ vegetation
// Leaf cards: quads with the atlas texture, normals from the crown ellipsoid so the whole crown is
// lit as one soft volume. The foliage shader does not cull, so the winding is free.
static void leafCard(MeshBuf *f, Vector3 p, Vector3 a1, Vector3 a2, Vector3 n, int kind, Color col)
{
    Rectangle r = LeafUV(kind);
    Vector3 v0 = Vector3Subtract(Vector3Subtract(p, a1), a2), v1 = Vector3Subtract(Vector3Add(p, a1), a2);
    Vector3 v2 = Vector3Add(Vector3Add(p, a1), a2), v3 = Vector3Add(Vector3Subtract(p, a1), a2);
    Vector2 t0 = uv2(r.x, r.y + r.height), t1 = uv2(r.x + r.width, r.y + r.height), t2 = uv2(r.x + r.width, r.y), t3 = uv2(r.x, r.y);
    MB_TriUV(f, v0, v1, v2, n, n, n, col, col, col, t0, t1, t2);
    MB_TriUV(f, v0, v2, v3, n, n, n, col, col, col, t0, t2, t3);
}

static Vector3 randDir(unsigned *s)
{
    float z = Rand01(s) * 2 - 1, a = Rand01(s) * 2 * PI, r = sqrtf(fmaxf(0, 1 - z * z));
    return vec(cosf(a) * r, z, sinf(a) * r);
}

// n cards inside the ellipsoid (centre c, radii r); size relative to the mean radius
static void crownCards(MeshBuf *f, Vector3 c, Vector3 r, int n, float sizeK, int kind, Color col, unsigned seed)
{
    unsigned s = seed;
    float rm = (r.x + r.y + r.z) / 3;
    for (int k = 0; k < n; k++) {
        Vector3 d = randDir(&s);
        float depth = 0.35f + 0.5f * Rand01(&s);
        Vector3 p = vec(c.x + d.x * r.x * depth, c.y + d.y * r.y * depth, c.z + d.z * r.z * depth);
        float sz = rm * sizeK * (0.75f + 0.5f * Rand01(&s));
        // card plane: faces roughly outwards, random roll
        Vector3 m = Vector3Normalize(Vector3Add(d, Vector3Scale(randDir(&s), 0.9f)));
        Vector3 ref = fabsf(m.y) < 0.8f ? vec(0, 1, 0) : vec(1, 0, 0);
        Vector3 a1 = Vector3Normalize(Vector3CrossProduct(m, ref));
        Vector3 a2 = Vector3CrossProduct(m, a1);
        float roll = Rand01(&s) * 2 * PI;
        Vector3 b1 = Vector3Add(Vector3Scale(a1, cosf(roll) * sz * 0.5f), Vector3Scale(a2, sinf(roll) * sz * 0.5f));
        Vector3 b2 = Vector3Add(Vector3Scale(a1, -sinf(roll) * sz * 0.5f), Vector3Scale(a2, cosf(roll) * sz * 0.5f));
        Vector3 nrm = Vector3Normalize(vec(d.x / r.x, d.y / r.y + 0.25f, d.z / r.z));
        leafCard(f, p, b1, b2, nrm, kind, ColorJitter(col, seed + k * 11, 12));
    }
}

// two crossed vertical cards (distant trees, bushes)
static void crossCards(MeshBuf *f, Vector3 c, float halfW, float halfH, int kind, Color col, unsigned seed, float tiltUp)
{
    float a = (seed & 1023) / 1023.0f * PI;
    for (int k = 0; k < 2; k++) {
        float ang = a + k * PI * 0.5f;
        Vector3 a1 = vec(cosf(ang) * halfW, 0, sinf(ang) * halfW), a2 = vec(0, halfH, 0);
        Vector3 n = Vector3Normalize(vec(-sinf(ang) * 0.35f, tiltUp, cosf(ang) * 0.35f));
        leafCard(f, c, a1, a2, n, kind, col);
    }
}

static void crownCore(MeshBuf *d, Vector3 c, Vector3 r, Color col, unsigned seed)
{
    MB_Mat(d, 0, 1, 0);
    MB_Blob(d, c, vec(r.x * 0.62f, r.y * 0.62f, r.z * 0.62f), 2, col, 0.3f, seed);
}

static void treeTrunk(MeshBuf *d, Vector3 p, float r, float h, Color col, int lod)
{
    MB_Mat(d, 2, 0, 1.2f);
    MB_Cylinder(d, vec(p.x, p.y - 0.3f, p.z), r, r * 0.7f, h + 0.3f, lod == 0 ? 7 : 4, col, false);
}

static void treePine(MeshBuf *d, MeshBuf *f, Vector3 p, float h, unsigned seed, int lod)
{
    unsigned s = seed;
    // Aleppo pine: slender leaning trunk, irregular flat crown
    float lean = (Rand01(&s) - 0.5f) * 0.25f, lean2 = (Rand01(&s) - 0.5f) * 0.25f;
    Vector3 top = { p.x + lean * h, p.y + h * 0.74f, p.z + lean2 * h };
    Color bark = { 196, 172, 150, 255 };
    Color leaf = ColorJitter((Color){ 220, 228, 212, 255 }, seed, 14);
    Vector3 crown = vec(top.x, top.y + h * 0.1f, top.z), rad = vec(h * 0.34f, h * 0.17f, h * 0.34f);
    if (lod == 2) { crossCards(f, vec(crown.x, crown.y - h * 0.02f, crown.z), h * 0.36f, h * 0.22f, LEAF_PINE, leaf, seed, 0.9f);
                    MB_Mat(d, 2, 0, 1.2f);
                    MB_Cylinder(d, vec(p.x, p.y - 0.3f, p.z), h * 0.025f, h * 0.018f, h * 0.7f, 3, bark, false);
                    return; }
    // trunk as a leaning prism with bark texture
    int sides = lod == 0 ? 5 : 4;
    MB_Mat(d, 2, 0, 1.2f);
    float r0 = h * 0.03f, r1 = h * 0.018f, circ = 2 * PI * r0;
    for (int i = 0; i < sides; i++) {
        float a0 = 2 * PI * i / sides, a1 = 2 * PI * (i + 1) / sides;
        Vector3 b0 = { p.x + cosf(a0) * r0, p.y - 0.3f, p.z + sinf(a0) * r0 }, b1 = { p.x + cosf(a1) * r0, p.y - 0.3f, p.z + sinf(a1) * r0 };
        Vector3 t0 = { top.x + cosf(a0) * r1, top.y, top.z + sinf(a0) * r1 }, t1 = { top.x + cosf(a1) * r1, top.y, top.z + sinf(a1) * r1 };
        float u0 = circ * i / sides / 1.2f, u1 = circ * (i + 1) / sides / 1.2f, v1 = (top.y - p.y) / 1.2f;
        MB_QuadUV(d, b0, t0, t1, b1, bark, uv2(u0, 0), uv2(u0, v1), uv2(u1, v1), uv2(u1, 0));
    }
    if (lod == 0) crownCore(d, crown, rad, (Color){ 44, 66, 40, 255 }, seed);
    crownCards(f, crown, rad, lod == 0 ? 12 : 5, 1.25f, LEAF_PINE, leaf, seed);
    // a lower branch tuft
    if (lod == 0) crownCards(f, vec(top.x - lean * h * 0.3f, top.y - h * 0.16f, top.z + (Rand01(&s) - 0.5f) * h * 0.2f), vec(h * 0.18f, h * 0.08f, h * 0.18f), 4, 1.3f, LEAF_PINE, leaf, seed + 3);
}

static void treeOak(MeshBuf *d, MeshBuf *f, Vector3 p, float h, unsigned seed, int lod)
{
    Color leaf = ColorJitter((Color){ 222, 232, 210, 255 }, seed, 14);
    Vector3 crown = vec(p.x, p.y + h * 0.62f, p.z), rad = vec(h * 0.42f, h * 0.36f, h * 0.42f);
    if (lod == 2) { crossCards(f, vec(p.x, p.y + h * 0.55f, p.z), h * 0.42f, h * 0.45f, LEAF_BROAD, leaf, seed, 0.8f); return; }
    treeTrunk(d, p, h * 0.05f, h * 0.45f, (Color){ 190, 176, 160, 255 }, lod);
    if (lod == 0) crownCore(d, crown, rad, (Color){ 46, 62, 34, 255 }, seed);
    crownCards(f, crown, rad, lod == 0 ? 12 : 5, 1.15f, LEAF_BROAD, leaf, seed);
}

static void treeOlive(MeshBuf *d, MeshBuf *f, Vector3 p, float h, unsigned seed, int lod)
{
    Color leaf = ColorJitter((Color){ 228, 232, 220, 255 }, seed, 10);
    Vector3 crown = vec(p.x, p.y + h * 0.62f, p.z), rad = vec(h * 0.5f, h * 0.34f, h * 0.5f);
    if (lod == 2) { crossCards(f, vec(p.x, p.y + h * 0.5f, p.z), h * 0.5f, h * 0.5f, LEAF_OLIVE, leaf, seed, 0.8f); return; }
    treeTrunk(d, p, 0.22f, h * 0.42f, (Color){ 210, 200, 186, 255 }, lod);
    if (lod == 0) crownCore(d, crown, rad, (Color){ 80, 92, 64, 255 }, seed);
    crownCards(f, crown, rad, lod == 0 ? 10 : 5, 1.2f, LEAF_OLIVE, leaf, seed);
}

static void treeCypress(MeshBuf *d, MeshBuf *f, Vector3 p, float h, unsigned seed, int lod)
{
    Color leaf = ColorJitter((Color){ 214, 226, 208, 255 }, seed, 8);
    Vector3 c = vec(p.x, p.y + h * 0.5f, p.z);
    if (lod == 2) { crossCards(f, c, h * 0.13f, h * 0.5f, LEAF_CYPRESS, leaf, seed, 0.3f); return; }
    if (lod == 0) { MB_Mat(d, 0, 1, 0); MB_Blob(d, c, vec(h * 0.08f, h * 0.48f, h * 0.08f), 2, (Color){ 30, 50, 30, 255 }, 0.1f, seed); }
    // vertical cards around the column
    int n = lod == 0 ? 5 : 3;
    unsigned s = seed;
    for (int k = 0; k < n; k++) {
        float a = 2 * PI * k / n + Rand01(&s) * 0.5f;
        Vector3 out = vec(cosf(a), 0, sinf(a));
        Vector3 pp = Vector3Add(c, Vector3Scale(out, h * 0.05f));
        Vector3 a1 = vec(-sinf(a) * h * 0.13f, 0, cosf(a) * h * 0.13f), a2 = vec(0, h * 0.5f, 0);
        leafCard(f, pp, a1, a2, Vector3Normalize(vec(out.x, 0.35f, out.z)), LEAF_CYPRESS, leaf);
    }
}

static void treePlane(MeshBuf *d, MeshBuf *f, Vector3 p, float h, unsigned seed, int lod)
{
    // platane: thick mottled pale trunk splitting into limbs, wide crown yellowing in autumn
    Color leaf = ColorLerp((Color){ 206, 226, 196, 255 }, (Color){ 236, 222, 184, 255 }, (seed & 255) / 255.0f);
    Color bark = { 230, 222, 196, 255 };
    Vector3 crown = vec(p.x, p.y + h * 0.66f, p.z), rad = vec(h * 0.42f, h * 0.3f, h * 0.42f);
    if (lod == 2) { crossCards(f, vec(p.x, p.y + h * 0.6f, p.z), h * 0.42f, h * 0.4f, LEAF_PLANE, leaf, seed, 0.8f);
                    treeTrunk(d, p, 0.4f, h * 0.4f, bark, 2); return; }
    float th = h * 0.36f;
    MB_Mat(d, 2, 0, 1.2f);
    MB_Cylinder(d, vec(p.x, p.y - 0.2f, p.z), 0.42f, 0.34f, th + 0.2f, lod == 0 ? 7 : 5, bark, false);
    unsigned s = seed;
    if (lod == 0) {
        for (int k = 0; k < 3; k++) {
            float a = k * 2.1f + Rand01(&s);
            Vector3 b0 = { p.x, p.y + th, p.z };
            Vector3 b1 = { p.x + cosf(a) * h * 0.18f, p.y + th + h * 0.26f, p.z + sinf(a) * h * 0.18f };
            for (int q = 0; q < 4; q++) {
                float a0 = q * PI * 0.5f, a1 = (q + 1) * PI * 0.5f, r0 = 0.24f, r1 = 0.14f;
                Vector3 o0 = { cosf(a0), 0, sinf(a0) }, o1 = { cosf(a1), 0, sinf(a1) };
                MB_QuadUV(d, Vector3Add(b0, Vector3Scale(o0, r0)), Vector3Add(b1, Vector3Scale(o0, r1)),
                          Vector3Add(b1, Vector3Scale(o1, r1)), Vector3Add(b0, Vector3Scale(o1, r0)), bark,
                          uv2(q * 0.3f, 0), uv2(q * 0.3f, 2.5f), uv2(q * 0.3f + 0.3f, 2.5f), uv2(q * 0.3f + 0.3f, 0));
            }
        }
    }
    if (lod == 0) crownCore(d, crown, rad, (Color){ 72, 82, 40, 255 }, seed);
    crownCards(f, crown, rad, lod == 0 ? 13 : 6, 1.15f, LEAF_PLANE, leaf, seed);
}

static void bush(MeshBuf *f, Vector3 p, float r, unsigned seed, Color base, int kind)
{
    crossCards(f, vec(p.x, p.y + r * 0.62f, p.z), r * 1.05f, r * 0.68f, kind, ColorJitter(base, seed, 14), seed, 0.9f);
}

static void grassTuft(MeshBuf *f, Vector3 p, float w, unsigned seed, Color col)
{
    crossCards(f, vec(p.x, p.y + w * 0.3f, p.z), w * 0.55f, w * 0.32f, LEAF_GRASS, col, seed, 1.0f);
}

static float treeObstacleRadius(int type) { return type == 3 ? 0.5f : type == 1 ? 0.55f : 0.45f; }

static void placeTree(Chunk *c, MeshBuf *d, MeshBuf *f, int type, Vector3 p, unsigned seed, int lod, float routeDist)
{
    unsigned s = seed;
    float h;
    switch (type) {
    case 1: h = 14 + Rand01(&s) * 6; treePlane(d, f, p, h, seed, lod); break;
    case 2: h = 9 + Rand01(&s) * 7; treePine(d, f, p, h, seed, lod); break;
    case 3: h = 9 + Rand01(&s) * 6; treeCypress(d, f, p, h, seed, lod); break;
    case 4: h = 3.5f + Rand01(&s) * 2; treeOlive(d, f, p, h, seed, lod); break;
    default: h = 6 + Rand01(&s) * 4; treeOak(d, f, p, h, seed, lod); break;
    }
    if (routeDist < 60) addObs(c, p.x, p.z, treeObstacleRadius(type), 0);
}

static void vineRows(MeshBuf *f, const Chunk *c, float cx, float cz, float cd, unsigned seed)
{
    unsigned blk = Hash2((int)floorf(cx / 45.0f), (int)floorf(cz / 45.0f));
    float ang = (blk & 1) ? 0.0f : PI * 0.5f;
    ang += ((blk >> 3) & 7) * 0.03f;
    static const Color autumn[4] = { { 236, 226, 170, 255 }, { 232, 196, 150, 255 }, { 206, 214, 160, 255 }, { 226, 176, 140, 255 } };
    Color base = autumn[(blk >> 6) & 3];
    float ca = cosf(ang), sa = sinf(ang);
    Rectangle r = LeafUV(LEAF_VINE);
    for (int k = 0; k < 4; k++) {
        float off = -3.75f + k * 2.5f;
        for (int half = 0; half < 2; half++) {
            float along = -2.5f + half * 5.0f;
            float x = cx + ca * along - sa * off, z = cz + sa * along + ca * off;
            float y = groundC(c, x, z, cd);
            Color col = ColorJitter(base, seed + k * 3 + half, 16);
            // hedge: two long leafy cards along the row + one across
            Vector3 cc = vec(x, y + 0.55f, z);
            Vector3 a1 = vec(ca * 2.5f, 0, sa * 2.5f), a2 = vec(0, 0.55f, 0);
            Vector3 n = Vector3Normalize(vec(-sa, 0.6f, ca));
            // long faces (uv stretched along the row: 2 repeats of the region are not possible, use the full card)
            Vector3 side = vec(-sa * 0.3f, 0, ca * 0.3f);
            (void)r;
            leafCard(f, Vector3Add(cc, side), a1, a2, n, LEAF_VINE, col);
            leafCard(f, Vector3Subtract(cc, side), a1, a2, Vector3Negate(vec(n.x, -n.y, n.z)), LEAF_VINE, col);
            leafCard(f, vec(cc.x, cc.y + 0.25f, cc.z), a1, vec(-sa * 0.5f, 0.1f, ca * 0.5f), vec(0, 1, 0), LEAF_VINE, col);
        }
    }
}

// ------------------------------------------------------------------ terrain
static void vertexNormalRaw(int i, int j, Vector3 *n)
{
    const Grid *g = &W.dem;
    int i0 = i > 0 ? i - 1 : i, i1 = i < g->nx - 1 ? i + 1 : i;
    int j0 = j > 0 ? j - 1 : j, j1 = j < g->nz - 1 ? j + 1 : j;
    float hx = (g->h[j * g->nx + i1] - g->h[j * g->nx + i0]) * 0.1f / ((i1 - i0) * g->cell);
    float hz = (g->h[j1 * g->nx + i] - g->h[j0 * g->nx + i]) * 0.1f / ((j1 - j0) * g->cell);
    *n = Vector3Normalize((Vector3){ -hx, 1, -hz });
}

// shared terrain vertex at half-cell key (kx, ky)
static int terrainVertex(GridBuf *t, const Chunk *c, int kx, int ky, bool conform)
{
    int lx = kx - t->lx0, ly = ky - t->ly0;
    if (lx >= 0 && ly >= 0 && lx < t->lw && ly < t->lh && t->lookup[ly * t->lw + lx] >= 0) return t->lookup[ly * t->lw + lx];
    float x = gx0 + kx * gcell * 0.5f, z = gz0 + ky * gcell * 0.5f;
    Vector3 P, N;
    float rd = -1, hw = 0;
    if (conform) {
        float dd;
        float y = conformed(x, z, c->mid, c->win, &dd);
        float e = 1.5f;
        float yx = conformed(x + e, z, c->mid, c->win, NULL) - conformed(x - e, z, c->mid, c->win, NULL);
        float yz = conformed(x, z + e, c->mid, c->win, NULL) - conformed(x, z - e, c->mid, c->win, NULL);
        P = (Vector3){ x, y, z };
        N = Vector3Normalize((Vector3){ -yx / (2 * e), 1, -yz / (2 * e) });
        rd = fabsf(dd);
        float s = RouteProject(x, z, c->mid, c->win, NULL, NULL), w;
        RouteSample(s, NULL, NULL, NULL, &w, NULL);
        hw = w * 0.5f;
    } else if (!(kx & 1) && !(ky & 1)) {
        int gi = kx / 2, gj = ky / 2;
        P = (Vector3){ x, W.dem.h[gj * W.dem.nx + gi] * 0.1f, z };
        vertexNormalRaw(gi, gj, &N);
    } else {
        P = (Vector3){ x, DemHeight(x, z), z };
        float e = 2.0f;
        N = Vector3Normalize((Vector3){ -(DemHeight(x + e, z) - DemHeight(x - e, z)) / (2 * e), 1,
                                        -(DemHeight(x, z + e) - DemHeight(x, z - e)) / (2 * e) });
    }
    if (GridClass(&W.lcov, x, z) == LC_WATER && (rd < 0 || rd > hw + SHOULDER + BLEND)) P.y -= 2.4f;
    float id, flat;
    Color col;
    terrainMat(x, z, N.y, rd, hw, &id, &col, &flat);
    return GB_Vertex(t, kx, ky, P, N, col, uv2(x / GROUND_TILE, z / GROUND_TILE), id, flat);
}

static void buildTerrainRow(Chunk *c, GridBuf *t, MeshBuf *d, MeshBuf *f, int j)
{
    for (int i = c->cx0; i <= c->cx1; i++) {
        int k = j * gnx + i;
        if (owner[k] != c->id) continue;
        float cd = cdist[k];
        float x0 = gx0 + i * gcell, z0 = gz0 + j * gcell;
        int sub = cd < NEAR_SUBDIV ? 2 : 1;
        bool conform = cd < CONFORM_ZONE + gcell;
        int step = 2 / sub;
        for (int b = 0; b < sub; b++) {
            for (int a = 0; a < sub; a++) {
                int kx = 2 * i + a * step, ky = 2 * j + b * step;
                int v00 = terrainVertex(t, c, kx, ky, conform), v10 = terrainVertex(t, c, kx + step, ky, conform);
                int v01 = terrainVertex(t, c, kx, ky + step, conform), v11 = terrainVertex(t, c, kx + step, ky + step, conform);
                GB_Tri(t, v00, v01, v11);
                GB_Tri(t, v00, v11, v10);
            }
        }

        // ---- vegetation for this cell
        if (mask[k]) continue;
        float cx = x0 + gcell * 0.5f, cz = z0 + gcell * 0.5f;
        int cls = GridClass(&W.lcov, cx, cz);
        unsigned seed = Hash2(i * 7 + 3, j * 13 + 1);
        unsigned s = seed;
        int lod = cd < TREE_NEAR ? 0 : cd < TREE_MID ? 1 : 2;
        if (lod == 2 && cd > TREE_FAR) continue;
        float clear = 0;
        if (cd < 40) {
            // keep the road and its verges clear
            float dd;
            RouteProject(cx, cz, c->mid, c->win, &dd, NULL);
            float w;
            RouteSample(RouteProject(cx, cz, c->mid, c->win, NULL, NULL), NULL, NULL, NULL, &w, NULL);
            clear = fabsf(dd) - (w * 0.5f + SHOULDER + 3.5f);
            if (clear < 2.0f) {
                // grass tufts on the verge
                if (clear > -2.5f && lod == 0 && (cls == LC_GRASS || cls == LC_SHRUB || cls == LC_UNKNOWN || cls == LC_CROP || cls == LC_ORCHARD)) {
                    for (int q = 0; q < 2; q++) {
                        float qx = x0 + gcell * Rand01(&s), qz = z0 + gcell * Rand01(&s);
                        float qd;
                        RouteProject(qx, qz, c->mid, c->win, &qd, NULL);
                        if (fabsf(qd) < w * 0.5f + SHOULDER + 0.6f) continue;
                        grassTuft(f, vec(qx, groundC(c, qx, qz, cd) - 0.05f, qz), 1.1f + Rand01(&s) * 0.6f, seed + q * 7,
                                  ColorLerp((Color){ 214, 222, 176, 255 }, (Color){ 236, 222, 160, 255 }, Rand01(&s)));
                    }
                }
                continue;
            }
        }
        float r1 = Rand01(&s), r2 = Rand01(&s), r3 = Rand01(&s);
        float px = x0 + gcell * (0.15f + 0.7f * r1), pz = z0 + gcell * (0.15f + 0.7f * r2);
        Vector3 p = { px, groundC(c, px, pz, cd), pz };
        switch (cls) {
        case LC_FOREST:
            if (lod == 2) { if (r3 < 0.45f) placeTree(c, d, f, r3 < 0.3f ? 2 : 0, p, seed, 2, cd); break; }
            placeTree(c, d, f, r3 < 0.65f ? 2 : 0, p, seed, lod, cd);
            if (lod == 0 && Rand01(&s) < 0.3f) {
                float qx = x0 + gcell * Rand01(&s), qz = z0 + gcell * Rand01(&s);
                bush(f, (Vector3){ qx, groundC(c, qx, qz, cd), qz }, 0.7f + Rand01(&s) * 0.6f, seed + 5, (Color){ 190, 206, 176, 255 }, LEAF_SHRUB);
            }
            break;
        case LC_SHRUB:
            if (lod == 2) { if (r3 < 0.25f) bush(f, p, 0.8f + r3, seed, (Color){ 210, 214, 190, 255 }, LEAF_SHRUB); break; }
            bush(f, p, 0.6f + r3 * 0.7f, seed, (Color){ 214, 218, 194, 255 }, LEAF_SHRUB);
            if (lod == 0) {
                float qx = x0 + gcell * Rand01(&s), qz = z0 + gcell * Rand01(&s);
                bush(f, (Vector3){ qx, groundC(c, qx, qz, cd), qz }, 0.5f + Rand01(&s) * 0.5f, seed + 9, (Color){ 226, 222, 192, 255 }, LEAF_SHRUB);
            }
            if (Rand01(&s) < 0.12f) placeTree(c, d, f, Rand01(&s) < 0.6f ? 2 : 0, (Vector3){ px + 2, p.y, pz - 2 }, seed + 3, lod, cd);
            break;
        case LC_GRASS:
        case LC_UNKNOWN:
            if (r3 < 0.08f) placeTree(c, d, f, r3 < 0.03f ? 2 : 0, p, seed, lod, cd);
            else if (lod == 0 && r3 < 0.25f) bush(f, p, 0.6f, seed, (Color){ 206, 218, 180, 255 }, LEAF_SHRUB);
            else if (lod == 0 && r3 < 0.6f) grassTuft(f, p, 1.2f + Rand01(&s) * 0.6f, seed, (Color){ 220, 226, 180, 255 });
            break;
        case LC_CROP:
            if (r3 < 0.025f) placeTree(c, d, f, r3 < 0.012f ? 3 : 0, p, seed, lod, cd);
            break;
        case LC_VINEYARD:
            if (cd < 320) vineRows(f, c, cx, cz, cd, seed);
            break;
        case LC_ORCHARD:
            if (lod < 2) {
                for (int q = 0; q < 2; q++) {
                    float qx = x0 + gcell * (0.25f + 0.5f * q) + (Rand01(&s) - 0.5f), qz = z0 + gcell * (0.3f + 0.4f * q) + (Rand01(&s) - 0.5f);
                    placeTree(c, d, f, 4, (Vector3){ qx, groundC(c, qx, qz, cd), qz }, seed + q * 31, lod, cd);
                }
            } else if (r3 < 0.5f) placeTree(c, d, f, 4, p, seed, 2, cd);
            break;
        case LC_RESIDENTIAL:
            if (r3 < 0.32f) {
                float r4 = Rand01(&s);
                placeTree(c, d, f, r4 < 0.3f ? 3 : r4 < 0.55f ? 2 : r4 < 0.8f ? 1 : 0, p, seed, lod, cd);
            } else if (lod == 0 && r3 < 0.5f) bush(f, p, 0.8f, seed, (Color){ 186, 214, 170, 255 }, LEAF_BROAD);
            break;
        case LC_BARE:
            if (lod == 0 && r3 < 0.1f) { MB_Mat(d, 3, 0, 3.0f); MB_Blob(d, p, (Vector3){ 1.0f, 0.6f, 0.8f }, 2, (Color){ 230, 226, 214, 255 }, 0.4f, seed); }
            break;
        default: break;
        }
    }
}

// ------------------------------------------------------------------ road
static void stripe(MeshBuf *r, int i0, int i1, float dOff, float width, float dash, float gap, Color col, bool (*want)(const RoutePt *))
{
    MB_Mat(r, 0, 0.75f, 0);      // worn paint: mostly flat white, a little asphalt grain
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
        MB_QuadUV(r, ra0, ra1, rb1, rb0, col, uv2((ra - width * 0.5f) / ROAD_TILE, a->s / ROAD_TILE), uv2((ra + width * 0.5f) / ROAD_TILE, a->s / ROAD_TILE),
                  uv2((rb + width * 0.5f) / ROAD_TILE, b->s / ROAD_TILE), uv2((rb - width * 0.5f) / ROAD_TILE, b->s / ROAD_TILE));
    }
}

static bool isTwoWay(const RoutePt *p) { return !(p->flags & RF_ONEWAY) && p->lanesForward < p->lanes && !(p->flags & RF_ROUNDABOUT); }
static bool twoWayStraight(const RoutePt *p) { return isTwoWay(p) && fabsf(p->curv) < 1.0f / 280.0f; }
static bool twoWayCurve(const RoutePt *p) { return isTwoWay(p) && fabsf(p->curv) >= 1.0f / 280.0f; }
static bool ruralEdge(const RoutePt *p) { return p->roadClass < RC_MOTORWAY && !(p->flags & RF_BUILTUP); }

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

// road-space quad between route points a and b, lateral d0..d1 (uv = lateral / along in tiles)
static void roadQuad(MeshBuf *r, const RoutePt *a, const RoutePt *b, float da0, float da1, float db0, float db1, float y0, float y1, Color col)
{
    MB_QuadUV(r, lat(a, da0, y0), lat(a, da1, y1), lat(b, db1, y1), lat(b, db0, y0), col,
              uv2(da0 / ROAD_TILE, a->s / ROAD_TILE), uv2(da1 / ROAD_TILE, a->s / ROAD_TILE), uv2(db1 / ROAD_TILE, b->s / ROAD_TILE), uv2(db0 / ROAD_TILE, b->s / ROAD_TILE));
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
        Color asphalt = a->roadClass >= RC_MOTORWAY ? (Color){ 226, 228, 234, 255 } : (Color){ 240, 240, 244, 255 };
        asphalt = ColorJitter(asphalt, h, 14);
        MB_Mat(r, 0, 0, 0);
        roadQuad(r, a, b, la, ra, lb, rb, 0.03f, 0.03f, asphalt);
        MB_Mat(r, 1.55f, 0, 0);
        Color gravel = { 214, 208, 196, 255 };
        roadQuad(r, a, b, la - SHOULDER, la, lb - SHOULDER, lb, -0.14f, 0.03f, gravel);
        roadQuad(r, a, b, ra, ra + SHOULDER, rb, rb + SHOULDER, 0.03f, -0.14f, gravel);
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
            stripe(r, k, k + 1, -hw + 0.3f, 0.12f, 0, 0, white, NULL);
            stripe(r, k, k + 1, hw - 0.3f, 0.12f, 0, 0, white, NULL);
        }
    }

    // start line and chequered finish line
    MB_Mat(r, 0, 1, 0);
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
            Color conc = { 226, 222, 214, 255 };
            Vector3 rightA = { -a->hz, 0, a->hx }, leftA = { a->hz, 0, -a->hx };
            MB_Mat(d, 3, 0, 3.0f);
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
            MB_Mat(d, 0, 1, 0);
            quadFacing(d, lat(a, dr, 0.45f), lat(b, dr2, 0.45f), lat(b, dr2, 0.78f), lat(a, dr, 0.78f), leftA, steel);
            quadFacing(d, lat(a, dr + 0.05f, 0.45f), lat(b, dr2 + 0.05f, 0.45f), lat(b, dr2 + 0.05f, 0.78f), lat(a, dr + 0.05f, 0.78f), rightA, ColorBrightness(steel, -0.15f));
            if ((int)(a->s / 4.0f) != (int)(b->s / 4.0f)) {
                Vector3 pp = lat(a, dr + 0.1f, 0.38f);
                MB_Box(d, pp, (Vector3){ 0.06f, 0.4f, 0.06f }, atan2f(a->hx, a->hz), (Color){ 120, 122, 126, 255 });
            }
            Color conc = { 222, 218, 208, 255 };
            float dl = la - 0.55f, dl2 = lb - 0.55f;
            MB_Mat(d, 3, 0, 3.0f);
            quadFacing(d, lat(a, dl + 0.3f, -0.1f), lat(b, dl2 + 0.3f, -0.1f), lat(b, dl2 + 0.1f, 0.82f), lat(a, dl + 0.1f, 0.82f), rightA, conc);
            quadFacing(d, lat(a, dl - 0.3f, -0.1f), lat(b, dl2 - 0.3f, -0.1f), lat(b, dl2 - 0.1f, 0.82f), lat(a, dl - 0.1f, 0.82f), leftA, ColorBrightness(conc, -0.1f));
            quadFacing(d, lat(a, dl + 0.1f, 0.82f), lat(a, dl - 0.1f, 0.82f), lat(b, dl2 - 0.1f, 0.82f), lat(b, dl2 + 0.1f, 0.82f), (Vector3){ 0, 1, 0 }, ColorBrightness(conc, 0.08f));
        } else if (!(a->flags & RF_BUILTUP)) {
            // French delineator posts (balises J6) every 50 m on both sides
            if (seg50a != seg50b) {
                MB_Mat(d, 0, 1, 0);
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
                MB_Mat(d, 0, 1, 0);
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
    Color col = dirt ? (Color){ 236, 230, 218, 255 } : (sr->roadClass >= RC_MOTORWAY ? (Color){ 224, 226, 232, 255 } : (Color){ 236, 236, 240, 255 });
    MB_Mat(r, dirt ? 2 : 0, 0, 0);
    float hw = sr->width * 0.5f;
    Vector3 prevL = { 0 }, prevR = { 0 };
    float along = 0, prevAlong = 0;
    for (int q = 0; q < m; q++) {
        int qa = q > 0 ? q - 1 : q, qb = q < m - 1 ? q + 1 : q;
        float dx = px[2 * qb] - px[2 * qa], dz = px[2 * qb + 1] - px[2 * qa + 1];
        float L = sqrtf(dx * dx + dz * dz);
        if (L < 1e-4f) { dx = 1; dz = 0; L = 1; }
        dx /= L; dz /= L;
        float x = px[2 * q], z = px[2 * q + 1];
        if (q > 0) along += hypotf(x - px[2 * (q - 1)], z - px[2 * (q - 1) + 1]);
        float y;
        if (bridge) y = yA + (yB - yA) * q / (float)(m - 1) + 0.1f;
        else y = SceneGround(x, z, c->mid) + 0.12f;
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
        if (q > 0) MB_QuadUV(r, prevR, Rp, Lp, prevL, col, uv2(hw * 2 / ROAD_TILE, prevAlong / ROAD_TILE), uv2(hw * 2 / ROAD_TILE, along / ROAD_TILE),
                             uv2(0, along / ROAD_TILE), uv2(0, prevAlong / ROAD_TILE));
        prevL = Lp; prevR = Rp; prevAlong = along;
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

// roof uv: along the eave (metres / tile) and up the slope
static Vector2 roofUV(Vector3 p, Vector3 e0, Vector3 eaveDir)
{
    Vector3 rel = Vector3Subtract(p, e0);
    float along = Vector3DotProduct(rel, eaveDir);
    Vector3 perp = Vector3Subtract(rel, Vector3Scale(eaveDir, along));
    return uv2(along / 2.4f, Vector3Length(perp) / 2.4f);
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
    static const Color walls[] = { { 236, 218, 182, 255 }, { 230, 202, 156, 255 }, { 222, 188, 138, 255 }, { 242, 232, 208, 255 },
                                   { 216, 182, 134, 255 }, { 232, 198, 176, 255 }, { 226, 212, 182, 255 } };
    static const Color shutters[] = { { 84, 124, 96, 255 }, { 74, 112, 150, 255 }, { 138, 128, 168, 255 }, { 124, 86, 56, 255 }, { 226, 226, 216, 255 }, { 96, 140, 150, 255 } };
    bool industrial = bd->kind == 2;
    Color wall = industrial ? ColorJitter((Color){ 226, 226, 222, 255 }, seed, 14) : walls[(int)(Rand01(&s) * 7) % 7];
    Color roofC = industrial ? (Color){ 190, 192, 194, 255 } : ColorJitter((Color){ 234, 226, 214, 255 }, seed + 1, 22);
    Color shut = shutters[(int)(Rand01(&s) * 6) % 6];
    float area = fabsf(polyArea(xz, n));

    // walls
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        Vector3 p0 = { xz[2 * i], base, xz[2 * i + 1] }, p1 = { xz[2 * j], base, xz[2 * j + 1] };
        Vector3 q0 = { p0.x, eave, p0.z }, q1 = { p1.x, eave, p1.z };
        float ex = p1.x - p0.x, ez = p1.z - p0.z, L = sqrtf(ex * ex + ez * ez);
        MB_Mat(b, industrial ? 3 : 0, 0, 0);
        float uw = L / 3.0f, vh = (eave - base) / 3.0f, vo = (seed & 7) * 0.37f;
        MB_QuadUV(b, p1, p0, q0, q1, wall, uv2(uw, vo), uv2(0, vo), uv2(0, vo + vh), uv2(uw, vo + vh));   // outward for a positive-area polygon
        // windows and shutters on houses close to the road
        if (industrial || routeDist > (gQuality == 2 ? 260 : gQuality ? 170 : 90) || L < 3.2f) continue;
        float nx = ez / L, nz = -ex / L;            // outward normal
        float ux = ex / L, uz = ez / L;
        int floors = (int)fmaxf(1, roundf(bd->height / 3.0f));
        int win = (int)((L - 1.0f) / 3.2f);
        if (win < 1) win = 1;
        MB_Mat(b, 0, 1, 0);
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
        MB_Mat(b, 1, 0, 0);
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
            float mx = (E[i].x + E[j].x) * 0.5f, mz = (E[i].z + E[j].z) * 0.5f;
            bool longSide = ((i - a0) % 2 + 2) % 2 == 0;
            Vector3 eaveDir = Vector3Normalize(Vector3Subtract(E[j], E[i]));
            Vector3 hint = { mx - cxm, 4.0f, mz - czm };
            if (longSide) {
                Vector3 ri = (Vector3Distance(E[i], r0) < Vector3Distance(E[i], r1)) ? r0 : r1;
                Vector3 rj = (Vector3Distance(E[j], r0) < Vector3Distance(E[j], r1)) ? r0 : r1;
                quadFacingUV(b, E[i], E[j], rj, ri, hint, roofC, roofUV(E[i], E[i], eaveDir), roofUV(E[j], E[i], eaveDir), roofUV(rj, E[i], eaveDir), roofUV(ri, E[i], eaveDir));
            } else {
                Vector3 rr = (Vector3DistanceSqr((Vector3){ mx, eave, mz }, r0) < Vector3DistanceSqr((Vector3){ mx, eave, mz }, r1)) ? r0 : r1;
                triFacingUV(b, E[i], E[j], rr, hint, ColorBrightness(roofC, -0.05f), roofUV(E[i], E[i], eaveDir), roofUV(E[j], E[i], eaveDir), roofUV(rr, E[i], eaveDir));
            }
        }
        // under the overhang
        MB_Mat(b, 0, 1, 0);
    } else if (!industrial && convex && n <= 10 && area < 500) {
        MB_Mat(b, 1, 0, 0);
        float cxm = 0, czm = 0;
        for (int i = 0; i < n; i++) { cxm += xz[2 * i]; czm += xz[2 * i + 1]; }
        cxm /= n; czm /= n;
        Vector3 apex = { cxm, eave + sqrtf(area) * 0.16f, czm };
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            Vector3 ei = { xz[2 * i], eave, xz[2 * i + 1] }, ej = { xz[2 * j], eave, xz[2 * j + 1] };
            Vector3 eaveDir = Vector3Normalize(Vector3Subtract(ej, ei));
            triFacingUV(b, ei, ej, apex, (Vector3){ (xz[2 * i] + xz[2 * j]) * 0.5f - cxm, 4.0f, (xz[2 * i + 1] + xz[2 * j + 1]) * 0.5f - czm }, roofC,
                        roofUV(ei, ei, eaveDir), roofUV(ej, ei, eaveDir), roofUV(apex, ei, eaveDir));
        }
    } else {
        MB_Mat(b, 3, 0, 3.0f);
        flatRoof(b, xz, n, eave, industrial ? roofC : (Color){ 226, 222, 212, 255 });
    }
    free(xz);
}

// ------------------------------------------------------------------ chunk build (resumable job)
// The CPU part is thread-safe (reads world data, writes only this chunk's buffers/obstacles) and
// can be run in slices with a time budget (web: no background thread).
typedef struct {
    Chunk *c;
    GridBuf t; MeshBuf r, d, f;
    int phase, row, idx;
    bool active;
} BuildJob;

static void jobStart(BuildJob *j, Chunk *c)
{
    j->c = c;
    GB_Init(&j->t, 2 * c->cx0, 2 * c->cz0, 2 * (c->cx1 - c->cx0 + 1) + 1, 2 * (c->cz1 - c->cz0 + 1) + 1);
    MB_Init(&j->r, 4096);
    MB_Init(&j->d, 16384);
    MB_Init(&j->f, 16384);
    MB_Mat(&j->f, 0, 0, 0);          // foliage: always the leaf atlas
    c->nObs = 0;
    j->phase = 1; j->row = c->cz0; j->idx = 0;
    j->active = true;
}

// returns true when the job is complete; budgetSec <= 0 runs to completion
static bool jobStep(BuildJob *j, double budgetSec)
{
    double t0 = budgetSec > 0 ? GetTime() : 0;
    Chunk *c = j->c;
    for (;;) {
        switch (j->phase) {
        case 1:
            if (j->row > c->cz1) { j->phase = 2; break; }
            buildTerrainRow(c, &j->t, &j->d, &j->f, j->row++);
            break;
        case 2: buildRoad(c, &j->r, &j->d); j->phase = 3; j->idx = 0; break;
        case 3:
            if (j->idx >= c->nPcs) { j->phase = 4; j->idx = 0; break; }
            buildSideRoad(c, &j->r, &pieces[c->pcs[j->idx++]]);
            break;
        case 4:
            if (j->idx >= c->nBld) { j->phase = 5; j->idx = 0; break; }
            {
                const Building *bd = &W.buildings[c->bld[j->idx]];
                float rd = SceneRouteDist(bd->cx, bd->cz);
                buildBuilding(&j->d, bd, rd, Hash2(c->bld[j->idx], 99));
                if (rd < 80) addObs(c, bd->cx, bd->cz, bd->radius * 0.85f, 1);
                j->idx++;
            }
            break;
        case 5:
            for (int k = 0; k < c->nTrees; k++) {
                const MappedTree *tr = &W.trees[c->trees[k]];
                float rd = SceneRouteDist(tr->x, tr->z);
                Vector3 p = { tr->x, SceneGround(tr->x, tr->z, c->mid), tr->z };
                placeTree(c, &j->d, &j->f, tr->type == 0 ? 0 : tr->type, p, Hash2(c->trees[k], 5), rd < TREE_NEAR ? 0 : 1, rd);
            }
            j->phase = 6;
            return true;
        default: return true;
        }
        if (budgetSec > 0 && GetTime() - t0 > budgetSec) return false;
    }
}

// GPU part (main thread only)
static void jobUpload(BuildJob *j)
{
    Chunk *c = j->c;
    c->terrain = j->t.ni ? GB_Upload(&j->t) : (Mesh){ 0 };
    c->road = j->r.count ? MB_Upload(&j->r) : (Mesh){ 0 };
    c->detail = j->d.count ? MB_Upload(&j->d) : (Mesh){ 0 };
    c->foliage = j->f.count ? MB_Upload(&j->f) : (Mesh){ 0 };
    GB_Free(&j->t); MB_Free(&j->r); MB_Free(&j->d); MB_Free(&j->f);
    c->built = true;
    c->shownAt = GetTime();
    j->active = false;
    j->c = NULL;
}

// ---- background builder thread
static cp_thread *worker;
static cp_lock *lk;
static volatile int jobChunk = -1;
static volatile bool jobDone, workerQuit, workerOn;
static BuildJob bgJob, fgJob;
static int fgChunk = -1;

static void *workerMain(void *arg)
{
    (void)arg;
    for (;;) {
        cp_lock_acquire(lk);
        while (!workerQuit && (jobChunk < 0 || jobDone)) cp_lock_wait(lk);
        if (workerQuit) { cp_lock_release(lk); break; }
        int id = jobChunk;
        cp_lock_release(lk);
        jobStart(&bgJob, &chunks[id]);
        jobStep(&bgJob, 0);
        cp_lock_acquire(lk);
        jobDone = true;
        cp_lock_release(lk);
    }
    return NULL;
}

static void unloadChunk(Chunk *c)
{
    if (c->terrain.vertexCount) UnloadMesh(c->terrain);     // also frees the kept index copy
    if (c->road.vertexCount) UnloadMesh(c->road);
    if (c->detail.vertexCount) UnloadMesh(c->detail);
    if (c->foliage.vertexCount) UnloadMesh(c->foliage);
    c->terrain = c->road = c->detail = c->foliage = (Mesh){ 0 };
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

// Corridor at 40 m (always drawn, slightly below the real surface): fills the far-mesh trench and
// the ground under chunks that are not loaded yet, so new chunks don't pop in from nothing.
static void buildMid(void)
{
    const Grid *g = &W.dem;
    int st = 4;
    int tile = 160;      // cells per mesh tile
    for (int tj = 0; tj < g->nz - 1; tj += tile) {
        for (int ti = 0; ti < g->nx - 1; ti += tile) {
            if (nMid >= 32) return;
            MeshBuf b;
            MB_Init(&b, 4096);
            for (int j = tj; j < tj + tile && j + st < g->nz; j += st) {
                for (int i = ti; i < ti + tile && i + st < g->nx; i += st) {
                    float cx = g->x0 + (i + st * 0.5f) * g->cell, cz = g->z0 + (j + st * 0.5f) * g->cell;
                    float cd = SceneRouteDist(cx, cz);
                    if (cd > CORRIDOR + 90) continue;
                    Vector3 P[4], N[4];
                    Color C[4];
                    for (int q = 0; q < 4; q++) {
                        int gi = i + (q & 1) * st, gj = j + (q >> 1) * st;
                        float x = g->x0 + gi * g->cell, z = g->z0 + gj * g->cell;
                        float y = g->h[gj * g->nx + gi] * 0.1f;
                        // stay under the road corridor, which can be cut several metres into the hill
                        float vcd = SceneRouteDist(x, z);
                        if (vcd < CONFORM_ZONE + 40) y = fminf(y, SceneGround(x, z, -1));
                        y -= 3.5f;
                        P[q] = (Vector3){ x, y, z };
                        vertexNormalRaw(gi, gj, &N[q]);
                        int cls = GridClass(&W.lcov, x, z);
                        if (cls == LC_WATER) cls = LC_UNKNOWN;     // 40 m cells would smear the canal
                        C[q] = classColor(cls, x, z);
                    }
                    MB_TriN(&b, P[0], P[2], P[3], N[0], N[2], N[3], C[0], C[2], C[3]);
                    MB_TriN(&b, P[0], P[3], P[1], N[0], N[3], N[1], C[0], C[3], C[1]);
                }
            }
            if (b.count) midMesh[nMid++] = MB_Upload(&b);
            MB_Free(&b);
        }
    }
}

// ------------------------------------------------------------------ init
void SceneInit(void)
{
    TexturesInit(gQuality);
    matFlat = LoadMaterialDefault();
    matFlat.shader = gLit;
    matTerrain = LoadMaterialDefault();
    matTerrain.shader = gLit;
    matTerrain.maps[0].texture = Tex(TEX_GRASS); matTerrain.maps[1].texture = Tex(TEX_SCRUB);
    matTerrain.maps[2].texture = Tex(TEX_SOIL);  matTerrain.maps[3].texture = Tex(TEX_ROCK);
    matRoad = LoadMaterialDefault();
    matRoad.shader = gLit;
    matRoad.maps[0].texture = Tex(TEX_ASPHALT); matRoad.maps[1].texture = Tex(TEX_GRAVEL);
    matRoad.maps[2].texture = Tex(TEX_DIRT);    matRoad.maps[3].texture = Tex(TEX_CONCRETE);
    matDetail = LoadMaterialDefault();
    matDetail.shader = gLit;
    matDetail.maps[0].texture = Tex(TEX_STUCCO); matDetail.maps[1].texture = Tex(TEX_ROOF);
    matDetail.maps[2].texture = Tex(TEX_BARK);   matDetail.maps[3].texture = Tex(TEX_PLASTER2);
    matFoliage = LoadMaterialDefault();
    matFoliage.shader = gLitCut;
    matFoliage.maps[0].texture = Tex(TEX_LEAVES);
    matDepth = LoadMaterialDefault();
    matDepth.shader = gDepth;
    matDepthCut = LoadMaterialDefault();
    matDepthCut.shader = gDepthCut;
    matDepthCut.maps[0].texture = Tex(TEX_LEAVES);

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
                int endp = p;
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
    buildMid();
    jobChunk = -1; jobDone = false; workerQuit = false;
    fgChunk = -1; fgJob.active = false;
#if defined(PLATFORM_WEB)
    worker = NULL;            // no threads in the browser build: chunks are built in time slices
#else
    if (!lk) lk = cp_lock_create();
    worker = lk ? cp_thread_create(workerMain, NULL) : NULL;
#endif
    workerOn = worker != NULL;
    TraceLog(LOG_INFO, "SCENE: %d chunks, %d side-road pieces, %d far tiles, %d mid tiles, async builder %s", nChunks, nPieces, nFar, nMid, workerOn ? "on" : "off");
}

void SceneShutdown(void)
{
    if (workerOn) {
        cp_lock_acquire(lk);
        workerQuit = true;
        cp_lock_broadcast(lk);
        cp_lock_release(lk);
        cp_thread_join(worker);
        worker = NULL;
        workerOn = false;
        if (jobChunk >= 0 && jobDone) { GB_Free(&bgJob.t); MB_Free(&bgJob.r); MB_Free(&bgJob.d); MB_Free(&bgJob.f); }
        jobChunk = -1; jobDone = false;
    }
    if (fgJob.active) { GB_Free(&fgJob.t); MB_Free(&fgJob.r); MB_Free(&fgJob.d); MB_Free(&fgJob.f); fgJob.active = false; fgChunk = -1; }
    for (int i = 0; i < nChunks; i++) {
        unloadChunk(&chunks[i]);
        free(chunks[i].obs); free(chunks[i].bld); free(chunks[i].pcs); free(chunks[i].trees);
    }
    for (int i = 0; i < nFar; i++) UnloadMesh(farMesh[i]);
    for (int i = 0; i < nMid; i++) UnloadMesh(midMesh[i]);
    nFar = 0; nMid = 0;
    free(chunks); free(owner); free(cdist); free(mask); free(pieces);
    chunks = NULL; pieces = NULL; nPieces = 0;
    TexturesShutdown();
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
        if (c->built || c->cx1 < 0 || i == jobChunk || i == fgChunk) continue;
        float dd = boxDist2D(&c->box, focus);
        if (dd < LOAD_RADIUS && dd < bd) { bd = dd; best = i; }
    }
    return best;
}

void SceneUpdate(Vector3 focus, float budgetMs)
{
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (c->cx1 < 0 || i == jobChunk || i == fgChunk) continue;
        float dd = boxDist2D(&c->box, focus);
        if (c->built && dd > LOAD_RADIUS + 350) unloadChunk(c);
    }
    // finished background job -> upload, then hand out the next one
    if (workerOn) {
        if (jobChunk >= 0) {
            cp_lock_acquire(lk);
            bool done = jobDone;
            cp_lock_release(lk);
            if (done) {
                jobUpload(&bgJob);
                cp_lock_acquire(lk);
                jobChunk = -1; jobDone = false;
                cp_lock_release(lk);
            }
        }
        if (jobChunk < 0) {
            int next = pickNext(focus);
            if (next >= 0) {
                cp_lock_acquire(lk);
                jobChunk = next; jobDone = false;
                cp_lock_signal(lk);
                cp_lock_release(lk);
            }
        }
        if (budgetMs <= 0) return;
    }
    // main-thread slices (loading screen, or no worker thread)
    double budget = (budgetMs > 0 ? budgetMs : 5.0) / 1000.0;
    double t0 = GetTime();
    for (;;) {
        if (!fgJob.active) {
            int next = pickNext(focus);
            if (next < 0) break;
            fgChunk = next;
            jobStart(&fgJob, &chunks[next]);
        }
        double left = budget - (GetTime() - t0);
        if (left <= 0) break;
        if (jobStep(&fgJob, left)) { jobUpload(&fgJob); fgChunk = -1; }
        else break;
    }
}

void SceneSetQuality(int q)
{
    gQuality = q;
    if (q == 0) { LOAD_RADIUS = 900; DRAW_RADIUS = 1100; TREE_NEAR = 80; TREE_MID = 200; TREE_FAR = 560; }
    else if (q == 1) { LOAD_RADIUS = 1250; DRAW_RADIUS = 1500; TREE_NEAR = 110; TREE_MID = 300; TREE_FAR = 760; }
    else { LOAD_RADIUS = 1750; DRAW_RADIUS = 2100; TREE_NEAR = 150; TREE_MID = 420; TREE_FAR = 760; }
    // rebuild everything with the new settings
    for (int i = 0; i < nChunks; i++) if (chunks[i].built && i != jobChunk && i != fgChunk) unloadChunk(&chunks[i]);
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

static bool chunkVisible(const Chunk *c, Camera3D cam, Vector3 fwd)
{
    if (!c->built) return false;
    if (boxDist2D(&c->box, cam.position) > DRAW_RADIUS) return false;
    for (int q = 0; q < 8; q++) {
        Vector3 p = { (q & 1) ? c->box.max.x : c->box.min.x, (q & 2) ? c->box.max.y : c->box.min.y, (q & 4) ? c->box.max.z : c->box.min.z };
        if (Vector3DotProduct(Vector3Subtract(p, cam.position), fwd) > -30.0f) return true;
    }
    return false;
}

static float riseOf(const Chunk *c, double now)
{
    float k = (float)((now - c->shownAt) / 1.5);
    return k < 0 ? 0 : k > 1 ? 1 : k * k * (3 - 2 * k);
}

void SceneDraw(Camera3D cam)
{
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    double now = GetTime();
    RenderSetMat(MAT_MATTE);
    RenderSetRise(1.0f);
    for (int i = 0; i < nFar; i++) DrawMesh(farMesh[i], matFlat, MatrixIdentity());
    for (int i = 0; i < nMid; i++) DrawMesh(midMesh[i], matFlat, MatrixIdentity());
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!chunkVisible(c, cam, fwd)) continue;
        if (c->terrain.vertexCount) DrawMesh(c->terrain, matTerrain, MatrixIdentity());
    }
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!chunkVisible(c, cam, fwd) || !c->detail.vertexCount) continue;
        RenderSetRise(riseOf(c, now));
        DrawMesh(c->detail, matDetail, MatrixIdentity());
    }
    RenderSetRise(1.0f);
    RenderSetMat(MAT_ROAD);
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!c->built || !c->road.vertexCount) continue;
        if (boxDist2D(&c->box, cam.position) > DRAW_RADIUS) continue;
        DrawMesh(c->road, matRoad, MatrixIdentity());
    }
    RenderSetMat(MAT_MATTE);
    // foliage last (alpha tested, two-sided)
    rlDisableBackfaceCulling();
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!chunkVisible(c, cam, fwd) || !c->foliage.vertexCount) continue;
        RenderSetRise(riseOf(c, now));
        DrawMesh(c->foliage, matFoliage, MatrixIdentity());
    }
    rlEnableBackfaceCulling();
    RenderSetRise(1.0f);
}

// shadow casters around 'centre' (terrain, buildings/trunks, foliage) with the depth shaders
void SceneDrawShadow(Vector3 centre, float radius)
{
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!c->built || boxDist2D(&c->box, centre) > radius) continue;
        if (c->terrain.vertexCount) DrawMesh(c->terrain, matDepth, MatrixIdentity());
        if (c->detail.vertexCount) DrawMesh(c->detail, matDepth, MatrixIdentity());
    }
    rlDisableBackfaceCulling();
    for (int i = 0; i < nChunks; i++) {
        Chunk *c = &chunks[i];
        if (!c->built || !c->foliage.vertexCount || boxDist2D(&c->box, centre) > radius) continue;
        DrawMesh(c->foliage, matDepthCut, MatrixIdentity());
    }
    rlEnableBackfaceCulling();
}

void SceneStats(int *loaded, long *verts, long *bytes)
{
    int n = 0;
    long v = 0, by = 0;
    const int VB = 3 * 4 + 3 * 4 + 2 * 4 + 2 * 4 + 4;    // bytes per vertex on the GPU
    for (int i = 0; i < nChunks; i++) {
        const Chunk *c = &chunks[i];
        if (!c->built) continue;
        n++;
        long cv = c->terrain.vertexCount + c->road.vertexCount + c->detail.vertexCount + c->foliage.vertexCount;
        v += cv;
        by += cv * VB + (long)c->terrain.triangleCount * 3 * 2;
    }
    for (int i = 0; i < nFar; i++) { v += farMesh[i].vertexCount; by += (long)farMesh[i].vertexCount * VB; }
    for (int i = 0; i < nMid; i++) { v += midMesh[i].vertexCount; by += (long)midMesh[i].vertexCount * VB; }
    *loaded = n;
    *verts = v;
    if (bytes) *bytes = by;
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
