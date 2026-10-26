// Procedural textures: a few periodic noise fields are computed once, every texture is then a cheap
// per-texel recipe on top of them. Everything tiles. The foliage atlas is painted leaf by leaf and
// gets hand-made mipmaps that keep the alpha-test coverage constant with distance.
#include "textures.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static Texture2D tex[TEX_COUNT];
static int N = 512;                     // ground texture size (power of two)
static float *fS, *fS2, *fR, *fW, *fWi, *fP, *fPi;   // base fields, N*N each

typedef struct { float r, g, b; } C3;
static C3 c3(float r, float g, float b) { return (C3){ r, g, b }; }
static C3 mixc(C3 a, C3 b, float t) { return (C3){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
static C3 mulc(C3 a, float k) { return (C3){ a.r * k, a.g * k, a.b * k }; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float sstep(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }

// ------------------------------------------------------------------ periodic noise
static unsigned hashi(int x, int y, unsigned seed)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float rnd(int x, int y, unsigned seed) { return (hashi(x, y, seed) & 0xFFFF) / 65535.0f; }

// value noise with lattice period P (power of two): x, y in lattice units
static float vnoise(float x, float y, int P, unsigned seed)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float tx = x - ix, ty = y - iy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
    int m = P - 1;
    float a = rnd(ix & m, iy & m, seed), b = rnd((ix + 1) & m, iy & m, seed);
    float c = rnd(ix & m, (iy + 1) & m, seed), d = rnd((ix + 1) & m, (iy + 1) & m, seed);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

static void fieldFbm(float *f, unsigned seed, int basePeriod, int octaves, bool ridged)
{
    float lo = 1e9f, hi = -1e9f;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float sum = 0, amp = 1, norm = 0;
            int P = basePeriod;
            for (int o = 0; o < octaves && P <= N; o++) {
                float n = vnoise(x * (float)P / N, y * (float)P / N, P, seed + o * 17);
                if (ridged) { n = 1 - fabsf(2 * n - 1); n *= n; }
                sum += n * amp; norm += amp;
                amp *= ridged ? 0.55f : 0.5f;
                P *= 2;
            }
            float v = sum / norm;
            f[y * N + x] = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    float k = hi > lo ? 1.0f / (hi - lo) : 1;
    for (int i = 0; i < N * N; i++) f[i] = (f[i] - lo) * k;
}

// Worley: F1 distance (normalised 0..1) and the id of the nearest feature point, cells x cells
static void fieldWorley(float *f1, float *id, int cells, unsigned seed)
{
    float cs = (float)N / cells;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int cx = (int)(x / cs), cy = (int)(y / cs);
            float best = 1e9f, bid = 0;
            for (int j = -1; j <= 1; j++) {
                for (int i = -1; i <= 1; i++) {
                    int gx = cx + i, gy = cy + j;
                    int wx = (gx + cells) % cells, wy = (gy + cells) % cells;
                    float px = (gx + rnd(wx, wy, seed)) * cs, py = (gy + rnd(wx, wy, seed + 1)) * cs;
                    float dx = x - px, dy = y - py, d = dx * dx + dy * dy;
                    if (d < best) { best = d; bid = rnd(wx, wy, seed + 2); }
                }
            }
            f1[y * N + x] = sqrtf(best) / cs;
            id[y * N + x] = bid;
        }
    }
}

static inline float S(const float *f, int x, int y) { return f[(y & (N - 1)) * N + (x & (N - 1))]; }
static float Sb(const float *f, float x, float y)   // bilinear, wrapping
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float tx = x - ix, ty = y - iy;
    float a = S(f, ix, iy), b = S(f, ix + 1, iy), c = S(f, ix, iy + 1), d = S(f, ix + 1, iy + 1);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// ------------------------------------------------------------------ image helpers
typedef struct { unsigned char *px; int w, h; } Img;

static Img imgNew(int w, int h) { Img im = { calloc((size_t)w * h * 4, 1), w, h }; return im; }
static void putpx(Img *im, int x, int y, C3 c)
{
    unsigned char *p = im->px + ((size_t)y * im->w + x) * 4;
    p[0] = (unsigned char)(clamp01(c.r) * 255 + 0.5f); p[1] = (unsigned char)(clamp01(c.g) * 255 + 0.5f);
    p[2] = (unsigned char)(clamp01(c.b) * 255 + 0.5f); p[3] = 255;
}
// paint with coverage a over the existing pixel (alpha = union)
static void blendpx(Img *im, int x, int y, C3 c, float a)
{
    if (x < 0 || y < 0 || x >= im->w || y >= im->h || a <= 0.002f) return;
    unsigned char *p = im->px + ((size_t)y * im->w + x) * 4;
    float da = p[3] / 255.0f;
    float na = a + da * (1 - a);
    float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
    if (na > 0) {
        r = (clamp01(c.r) * a + r * da * (1 - a)) / na;
        g = (clamp01(c.g) * a + g * da * (1 - a)) / na;
        b = (clamp01(c.b) * a + b * da * (1 - a)) / na;
    }
    p[0] = (unsigned char)(r * 255 + 0.5f); p[1] = (unsigned char)(g * 255 + 0.5f); p[2] = (unsigned char)(b * 255 + 0.5f);
    p[3] = (unsigned char)(clamp01(na) * 255 + 0.5f);
}

static Texture2D upload(Img im, bool mips)
{
    Image ri = { im.px, im.w, im.h, mips ? 0 : 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    if (mips) {
        int w = im.w, h = im.h, n = 1;
        while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
        ri.mipmaps = n;
    }
    Texture2D t = LoadTextureFromImage(ri);
    if (!mips) GenTextureMipmaps(&t);
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
    SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_8X);
#endif
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

// ------------------------------------------------------------------ ground recipes
typedef C3 (*Recipe)(int x, int y);

static C3 recGrass(int x, int y)
{
    float big = Sb(fS, x * 0.5f, y * 0.5f), mid = S(fS2, x, y);
    C3 c = mixc(c3(0.33f, 0.41f, 0.15f), c3(0.58f, 0.54f, 0.27f), sstep(0.25f, 0.8f, big * 0.6f + mid * 0.4f));
    // blades: ridged noise stretched vertically
    float bl = Sb(fR, x * 2.0f + 31, y * 1.1f), fine = S(fS2, x * 3 + 7, y * 3 + 11);
    c = mulc(c, 0.78f + 0.3f * bl + 0.25f * (fine - 0.5f));
    // small debris: dead leaves and pale stones
    float w = S(fP, x, y), id = S(fPi, x, y);
    if (w < 0.18f) c = mixc(c, id > 0.55f ? c3(0.50f, 0.40f, 0.22f) : c3(0.66f, 0.63f, 0.52f), sstep(0.18f, 0.06f, w) * 0.8f);
    return mulc(c, 0.92f + 0.16f * S(fS2, x + 200, y + 77));
}

static C3 recScrub(int x, int y)
{
    float big = Sb(fS, x * 0.5f, y * 0.5f);
    C3 ground = mulc(mixc(c3(0.60f, 0.54f, 0.42f), c3(0.70f, 0.66f, 0.54f), big), 0.85f + 0.3f * S(fS2, x, y));
    // dark green clumps of kermes oak / rosemary / thyme
    float cl = Sb(fS2, x * 2.0f + 100, y * 2.0f + 40) * 0.6f + (1 - S(fW, x, y)) * 0.4f;
    float k = sstep(0.52f, 0.66f, cl) * sstep(0.3f, 0.55f, Sb(fS, x * 0.7f + 300, y * 0.7f + 120));
    C3 clump = mixc(c3(0.30f, 0.36f, 0.20f), c3(0.46f, 0.48f, 0.28f), S(fR, x, y));
    C3 c = mixc(ground, clump, k);
    // limestone bits
    float w = S(fP, x, y), id = S(fPi, x, y);
    if (w < 0.22f && id > 0.5f) c = mixc(c, c3(0.78f, 0.76f, 0.68f), sstep(0.22f, 0.08f, w) * (1 - k));
    return c;
}

static C3 recRock(int x, int y)
{
    float big = Sb(fS, x * 0.5f, y * 0.5f);
    C3 c = mixc(c3(0.70f, 0.66f, 0.56f), c3(0.80f, 0.76f, 0.66f), big);
    c = mulc(c, 0.82f + 0.3f * S(fS2, x, y));
    // cracks and fissures
    float cr = Sb(fR, x * 2.0f + 90, y * 2.0f + 10);
    if (cr > 0.80f) c = mulc(c, 1 - 0.45f * sstep(0.80f, 0.93f, cr));
    c = mulc(c, 0.92f + 0.16f * S(fS2, x * 3 + 5, y * 3 + 9));
    // ochre weathering
    c = mixc(c, c3(0.74f, 0.58f, 0.40f), 0.35f * sstep(0.6f, 0.85f, Sb(fS2, x * 0.4f + 300, y * 0.4f + 50)));
    float w = S(fP, x, y);
    if (w < 0.16f) c = mixc(c, c3(0.86f, 0.84f, 0.76f), sstep(0.16f, 0.04f, w) * 0.7f);
    return c;
}

static C3 recSoil(int x, int y)
{
    float big = Sb(fS, x * 0.5f, y * 0.5f);
    C3 c = mixc(c3(0.50f, 0.38f, 0.25f), c3(0.64f, 0.52f, 0.36f), big);
    // clods
    float w = S(fW, x, y);
    c = mulc(c, 0.86f + 0.22f * (1 - w));
    // faint furrows
    float fur = 0.5f + 0.5f * sinf(x * 2 * PI * 6.0f / N + Sb(fS2, x * 0.3f, y * 0.3f) * 2.5f);
    c = mulc(c, 0.93f + 0.1f * fur);
    c = mulc(c, 0.9f + 0.2f * S(fS2, x, y));
    float p = S(fP, x, y);
    if (p < 0.14f) c = mixc(c, c3(0.72f, 0.68f, 0.58f), sstep(0.14f, 0.05f, p) * 0.6f);
    return c;
}

static C3 recAsphalt(int x, int y)
{
    float big = Sb(fS, x * 0.4f, y * 0.4f);
    C3 c = mulc(c3(0.28f, 0.28f, 0.29f), 0.86f + 0.28f * big);
    float grain = S(fS2, x * 3 + 17, y * 3 + 5);
    c = mulc(c, 0.85f + 0.3f * grain);
    // aggregate stones poking through the bitumen
    float w = S(fP, x, y), id = S(fPi, x, y);
    if (w < 0.17f && id > 0.45f) c = mixc(c, c3(0.44f, 0.43f, 0.42f), sstep(0.17f, 0.07f, w) * 0.8f);
    // rare cracks
    float cr = S(fR, x, y);
    if (cr > 0.84f && Sb(fS2, x * 0.2f, y * 0.2f) > 0.58f) c = mulc(c, 1 - 0.5f * sstep(0.84f, 0.95f, cr));
    // oil / repair patches
    c = mulc(c, 1 - 0.12f * sstep(0.68f, 0.8f, Sb(fS2, x * 0.15f + 400, y * 0.15f + 90)));
    return c;
}

static C3 recGravel(int x, int y)
{
    float w = S(fP, x, y), id = S(fPi, x, y);
    static const C3 pal[6] = { { 0.62f, 0.58f, 0.50f }, { 0.72f, 0.70f, 0.64f }, { 0.55f, 0.52f, 0.46f },
                               { 0.68f, 0.62f, 0.50f }, { 0.76f, 0.74f, 0.70f }, { 0.60f, 0.57f, 0.55f } };
    C3 stone = pal[(int)(id * 5.99f)];
    stone = mulc(stone, 1.15f - 0.5f * w);                       // rounded shading
    C3 gap = mulc(c3(0.40f, 0.36f, 0.30f), 0.8f + 0.3f * S(fS2, x, y));
    float k = sstep(0.62f, 0.45f, w);
    return mixc(gap, stone, k);
}

static C3 recDirt(int x, int y)
{
    float big = Sb(fS, x * 0.5f, y * 0.5f);
    C3 c = mixc(c3(0.56f, 0.46f, 0.32f), c3(0.68f, 0.60f, 0.46f), big);
    c = mulc(c, 0.85f + 0.3f * S(fS2, x, y));
    float w = S(fP, x, y), id = S(fPi, x, y);
    if (w < 0.2f && id > 0.3f) c = mixc(c, mulc(c3(0.70f, 0.66f, 0.58f), 1.1f - 0.5f * w), sstep(0.2f, 0.08f, w) * 0.8f);
    return c;
}

static C3 recConcrete(int x, int y)
{
    C3 c = mulc(c3(0.70f, 0.70f, 0.68f), 0.9f + 0.2f * Sb(fS, x * 0.5f, y * 0.5f));
    c = mulc(c, 0.94f + 0.12f * S(fS2, x * 2, y * 2));
    float w = S(fP, x, y);
    if (w < 0.1f) c = mulc(c, 0.85f);
    // panel joints
    int jx = x % (N / 2), jy = y % (N / 2);
    if (jx < 2 || jy < 2) c = mulc(c, 0.86f);
    return c;
}

static C3 recStucco(int x, int y)
{
    C3 c = mulc(c3(0.96f, 0.94f, 0.90f), 0.9f + 0.1f * Sb(fS, x * 0.5f, y * 0.5f));
    c = mulc(c, 0.92f + 0.12f * S(fS2, x * 3, y * 3));
    c = mulc(c, 0.95f + 0.08f * S(fR, x * 2, y * 2));
    // faint weather stains
    c = mulc(c, 1 - 0.07f * sstep(0.6f, 0.9f, Sb(fS2, x * 0.25f + 123, y * 0.25f + 321)));
    return c;
}

static C3 recRoof(int x, int y)
{
    const int cols = 12, rows = 6;
    float cw = (float)N / cols, rh = (float)N / rows;
    int r = (int)(y / rh);
    float v = (y - r * rh) / rh;
    // each row is shifted a little, tiles are hand made
    float xs = x + rnd(r, 0, 9) * 6.0f;
    int c = (int)floorf(xs / cw);
    float u = (xs - c * cw) / cw;
    unsigned h = hashi(c, r, 31);
    bool cover = (c & 1) == 0;                     // convex cover tile / concave channel tile
    float prof = cover ? 0.55f + 0.5f * sinf(u * PI) : 0.95f - 0.45f * sinf(u * PI);
    C3 base = mixc(c3(0.70f, 0.40f, 0.26f), c3(0.82f, 0.56f, 0.38f), (h & 255) / 255.0f);
    if (((h >> 8) & 7) == 0) base = mixc(base, c3(0.62f, 0.50f, 0.40f), 0.5f);   // older darker tile
    // overlap shadow at the top of each tile, lighter towards the bottom edge
    float rowS = 0.62f + 0.45f * sstep(0.0f, 0.3f, v);
    if (v < 0.09f) rowS *= 0.5f;
    if (v > 0.93f) rowS *= 0.8f;                     // rounded lower edge of the tile
    base = mulc(base, 0.85f + 0.3f * ((h >> 16) & 255) / 255.0f);
    C3 col = mulc(base, prof * rowS);
    // lichen / dust
    col = mixc(col, c3(0.66f, 0.64f, 0.52f), 0.35f * sstep(0.6f, 0.85f, Sb(fS, x * 0.5f, y * 0.5f)));
    col = mulc(col, 0.92f + 0.14f * S(fS2, x, y));
    return col;
}

static C3 recBark(int x, int y)
{
    float ridge = Sb(fR, x * 1.0f, y * 0.22f);
    C3 c = mixc(c3(0.34f, 0.27f, 0.20f), c3(0.56f, 0.47f, 0.38f), ridge);
    float fis = Sb(fS2, x * 2.5f, y * 0.3f);
    if (fis < 0.38f) c = mulc(c, 0.55f + 0.45f * sstep(0.2f, 0.38f, fis));
    c = mulc(c, 0.9f + 0.2f * S(fS, x, y));
    c = mixc(c, c3(0.56f, 0.58f, 0.50f), 0.5f * sstep(0.7f, 0.9f, Sb(fS, x * 0.6f + 50, y * 0.6f + 200)));   // lichen
    return c;
}

static Img genGround(Recipe rec)
{
    Img im = imgNew(N, N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) putpx(&im, x, y, rec(x, y));
    return im;
}

// ------------------------------------------------------------------ foliage atlas
static unsigned rs = 12345;
static float R01(void) { rs = rs * 1664525u + 1013904223u; return ((rs >> 8) & 0xFFFFFF) / 16777216.0f; }

static void drawLeaf(Img *im, float cx, float cy, float len, float wid, float ang, C3 col, float tipShade)
{
    float ca = cosf(ang), sa = sinf(ang);
    float rad = len * 0.5f + 2;
    int x0 = (int)(cx - rad), x1 = (int)(cx + rad), y0 = (int)(cy - rad), y1 = (int)(cy + rad);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float lx = dx * ca + dy * sa, ly = -dx * sa + dy * ca;      // lx along the leaf
            float t = lx / (len * 0.5f);
            if (t < -1 || t > 1) continue;
            float hw = wid * 0.5f * powf(fmaxf(0, 1 - t * t), 0.55f) * (t < 0 ? 1 : 1 - 0.35f * t);
            float a = clamp01(hw - fabsf(ly) + 0.5f);
            if (a <= 0) continue;
            float shade = 0.85f + 0.3f * (t * 0.5f + 0.5f) * tipShade;
            if (fabsf(ly) < 0.7f && t > -0.8f) shade *= 0.82f;              // midrib
            blendpx(im, x, y, mulc(col, shade), a);
        }
    }
}

static void drawLine(Img *im, float x0, float y0, float x1, float y1, float w0, float w1, C3 c0, C3 c1)
{
    float minx = fminf(x0, x1) - w0 - 1, maxx = fmaxf(x0, x1) + w0 + 1, miny = fminf(y0, y1) - w0 - 1, maxy = fmaxf(y0, y1) + w0 + 1;
    float dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++) {
        for (int x = (int)minx; x <= (int)maxx; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = L2 > 0 ? clamp01((px * dx + py * dy) / L2) : 0;
            float ex = px - dx * t, ey = py - dy * t;
            float d = sqrtf(ex * ex + ey * ey);
            float hw = (w0 + (w1 - w0) * t) * 0.5f;
            float a = clamp01(hw - d + 0.5f);
            if (a > 0) blendpx(im, x, y, mixc(c0, c1, t), a);
        }
    }
}

typedef struct { Img *im; int ox, oy, R; } Region;

static void paintBroad(Region g, int count, float lenK, float widK, const C3 *pal, int npal, float discRx, float discRy, float dark)
{
    for (int i = 0; i < count; i++) {
        float u = R01(), a = R01() * 2 * PI, rr = sqrtf(u);
        float cx = g.ox + g.R * 0.5f + cosf(a) * rr * discRx * g.R, cy = g.oy + g.R * 0.5f + sinf(a) * rr * discRy * g.R;
        float len = g.R * lenK * (0.8f + 0.4f * R01()), wid = len * widK * (0.85f + 0.3f * R01());
        float ang = a + (R01() - 0.5f) * 1.6f;
        C3 col = pal[(int)(R01() * npal) % npal];
        col = mulc(col, (0.9f + 0.2f * R01()) * (1 - dark * (1 - rr) * (1 - (float)i / count)));
        drawLeaf(g.im, cx, cy, len, wid, ang, col, 1.0f);
    }
}

static void paintPine(Region g)
{
    C3 twig = c3(0.36f, 0.28f, 0.20f);
    for (int b = 0; b < 11; b++) {
        float a = b * 2 * PI / 11 + R01() * 0.5f, rr = 0.18f + 0.2f * R01();
        float bx = g.ox + g.R * 0.5f + cosf(a) * rr * g.R, by = g.oy + g.R * 0.5f + sinf(a) * rr * g.R;
        drawLine(g.im, g.ox + g.R * 0.5f, g.oy + g.R * 0.5f, bx, by, 4.0f * g.R / 512, 2.5f * g.R / 512, twig, twig);
        for (int n = 0; n < 46; n++) {
            float na = a + (R01() - 0.5f) * 1.9f, L = g.R * (0.09f + 0.09f * R01());
            C3 c0 = mixc(c3(0.14f, 0.28f, 0.13f), c3(0.22f, 0.36f, 0.16f), R01());
            C3 c1 = mixc(c0, c3(0.38f, 0.50f, 0.22f), 0.5f);
            drawLine(g.im, bx, by, bx + cosf(na) * L, by + sinf(na) * L, 2.6f * g.R / 512, 1.6f * g.R / 512, c0, c1);
        }
    }
}

static void paintCypress(Region g)
{
    for (int i = 0; i < 2600; i++) {
        float x = g.ox + g.R * (0.08f + 0.84f * R01()), y = g.oy + g.R * R01();
        float ex = (x - g.ox - g.R * 0.5f) / (g.R * 0.42f), ey = (y - g.oy - g.R * 0.5f) / (g.R * 0.52f);
        if (ex * ex + ey * ey > 1) continue;
        float ang = -PI * 0.5f + (R01() - 0.5f) * 1.3f, L = g.R * (0.025f + 0.025f * R01());
        C3 c = mixc(c3(0.10f, 0.22f, 0.11f), c3(0.24f, 0.38f, 0.18f), R01());
        c = mulc(c, 0.75f + 0.45f * (1 - sqrtf(ex * ex + ey * ey)));
        drawLine(g.im, x, y, x + cosf(ang) * L, y + sinf(ang) * L, 5.0f * g.R / 512, 2.0f * g.R / 512, c, c);
    }
}

static void paintGrass(Region g)
{
    for (int i = 0; i < 120; i++) {
        float bx = g.ox + g.R * (0.2f + 0.6f * R01()), by = g.oy + g.R * 0.97f;
        float L = g.R * (0.45f + 0.45f * R01()), bend = (R01() - 0.5f) * 0.9f, lean = (R01() - 0.5f) * 0.5f;
        C3 c = mixc(c3(0.30f, 0.45f, 0.14f), c3(0.62f, 0.58f, 0.26f), R01());
        float px = bx, py = by;
        for (int s = 1; s <= 7; s++) {
            float t = s / 7.0f;
            float nx = bx + (lean * t + bend * t * t) * L, ny = by - t * L * (1 - 0.3f * t * t);
            drawLine(g.im, px, py, nx, ny, (4.2f - 3.0f * t) * g.R / 512, (4.2f - 3.0f * (t + 0.14f)) * g.R / 512,
                     mulc(c, 0.7f + 0.4f * t), mulc(c, 0.7f + 0.45f * t));
            px = nx; py = ny;
        }
    }
}

static void paintTwigs(Region g, int n, C3 col)
{
    for (int i = 0; i < n; i++) {
        float a = R01() * 2 * PI, L = g.R * (0.2f + 0.25f * R01());
        float cx = g.ox + g.R * 0.5f, cy = g.oy + g.R * 0.5f;
        drawLine(g.im, cx, cy, cx + cosf(a) * L, cy + sinf(a) * L, 3.5f * g.R / 512, 1.5f * g.R / 512, col, col);
    }
}

static Rectangle leafRect[LEAF_COUNT];

static Img genLeaves(int W)
{
    Img im = imgNew(W, W / 2);
    int R = W / 4;
    rs = 777;
    for (int k = 0; k < LEAF_COUNT; k++) {
        Region g = { &im, (k % 4) * R, (k / 4) * R, R };
        leafRect[k] = (Rectangle){ (float)g.ox / W + 0.01f, (float)g.oy / (W / 2) + 0.02f, (float)R / W - 0.02f, (float)R / (W / 2) - 0.04f };
        switch (k) {
        case LEAF_BROAD: {
            static const C3 pal[4] = { { 0.26f, 0.42f, 0.14f }, { 0.36f, 0.50f, 0.18f }, { 0.44f, 0.54f, 0.20f }, { 0.30f, 0.44f, 0.17f } };
            paintTwigs(g, 5, c3(0.40f, 0.32f, 0.24f));
            paintBroad(g, 210, 0.13f, 0.52f, pal, 4, 0.42f, 0.40f, 0.5f);
            break; }
        case LEAF_PINE: paintPine(g); break;
        case LEAF_OLIVE: {
            static const C3 pal[4] = { { 0.40f, 0.46f, 0.30f }, { 0.58f, 0.62f, 0.50f }, { 0.48f, 0.54f, 0.38f }, { 0.66f, 0.68f, 0.58f } };
            paintTwigs(g, 6, c3(0.46f, 0.42f, 0.36f));
            paintBroad(g, 360, 0.075f, 0.24f, pal, 4, 0.45f, 0.42f, 0.4f);
            break; }
        case LEAF_CYPRESS: paintCypress(g); break;
        case LEAF_GRASS: paintGrass(g); break;
        case LEAF_SHRUB: {
            static const C3 pal[4] = { { 0.34f, 0.40f, 0.24f }, { 0.48f, 0.50f, 0.32f }, { 0.40f, 0.46f, 0.26f }, { 0.56f, 0.56f, 0.38f } };
            paintTwigs(g, 8, c3(0.42f, 0.36f, 0.28f));
            paintBroad(g, 420, 0.065f, 0.45f, pal, 4, 0.46f, 0.38f, 0.5f);
            break; }
        case LEAF_VINE: {
            static const C3 pal[5] = { { 0.66f, 0.60f, 0.20f }, { 0.72f, 0.42f, 0.16f }, { 0.56f, 0.26f, 0.15f }, { 0.42f, 0.50f, 0.18f }, { 0.78f, 0.62f, 0.26f } };
            paintTwigs(g, 4, c3(0.40f, 0.30f, 0.22f));
            paintBroad(g, 260, 0.11f, 0.85f, pal, 5, 0.42f, 0.42f, 0.45f);
            break; }
        case LEAF_PLANE: {
            static const C3 pal[4] = { { 0.58f, 0.60f, 0.22f }, { 0.72f, 0.60f, 0.24f }, { 0.44f, 0.52f, 0.20f }, { 0.66f, 0.50f, 0.22f } };
            paintTwigs(g, 4, c3(0.56f, 0.52f, 0.42f));
            paintBroad(g, 240, 0.12f, 0.9f, pal, 4, 0.42f, 0.40f, 0.5f);
            break; }
        default: break;
        }
    }
    return im;
}

// Box-filtered mipmaps whose alpha is rescaled so the alpha-test coverage stays constant.
static unsigned char *buildAlphaMips(const Img *im, int *mipCount, size_t *totalBytes)
{
    int w = im->w, h = im->h, levels = 1;
    size_t total = (size_t)w * h * 4;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; total += (size_t)w * h * 4; levels++; }
    unsigned char *out = malloc(total);
    memcpy(out, im->px, (size_t)im->w * im->h * 4);
    // reference coverage
    long cov0 = 0;
    for (long i = 0; i < (long)im->w * im->h; i++) if (im->px[i * 4 + 3] >= 128) cov0++;
    float covFrac = (float)cov0 / ((float)im->w * im->h);
    const unsigned char *src = out;
    int sw = im->w, sh = im->h;
    size_t off = (size_t)sw * sh * 4;
    for (int l = 1; l < levels; l++) {
        int dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
        unsigned char *dst = out + off;
        float *alpha = malloc(sizeof(float) * dw * dh);
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                float r = 0, g = 0, b = 0, a = 0;
                for (int j = 0; j < 2; j++) {
                    for (int i = 0; i < 2; i++) {
                        int sx = (2 * x + i) < sw ? 2 * x + i : sw - 1, sy = (2 * y + j) < sh ? 2 * y + j : sh - 1;
                        const unsigned char *p = src + ((size_t)sy * sw + sx) * 4;
                        float pa = p[3] / 255.0f;
                        r += p[0] * pa; g += p[1] * pa; b += p[2] * pa; a += pa;
                    }
                }
                unsigned char *d = dst + ((size_t)y * dw + x) * 4;
                if (a > 0) { d[0] = (unsigned char)(r / a); d[1] = (unsigned char)(g / a); d[2] = (unsigned char)(b / a); }
                else d[0] = d[1] = d[2] = 0;
                alpha[y * dw + x] = a / 4;
            }
        }
        // find the alpha scale that restores the coverage
        float lo = 1, hi = 12;
        for (int it = 0; it < 12; it++) {
            float s = (lo + hi) * 0.5f;
            long cov = 0;
            for (int i = 0; i < dw * dh; i++) if (fminf(1, alpha[i] * s) >= 0.5f) cov++;
            if ((float)cov / (dw * dh) < covFrac) lo = s; else hi = s;
        }
        float s = (lo + hi) * 0.5f;
        for (int i = 0; i < dw * dh; i++) dst[i * 4 + 3] = (unsigned char)(fminf(1, alpha[i] * s) * 255 + 0.5f);
        free(alpha);
        src = dst; sw = dw; sh = dh; off += (size_t)dw * dh * 4;
    }
    *mipCount = levels;
    *totalBytes = total;
    return out;
}

// ------------------------------------------------------------------ public
static Img images[TEX_COUNT];
static bool keepImages;

void TexturesInit(int quality)
{
    N = quality == 0 ? 256 : 512;
    int leafW = quality == 0 ? 1024 : 2048;
    double t0 = GetTime();
    fS = malloc(sizeof(float) * N * N); fS2 = malloc(sizeof(float) * N * N); fR = malloc(sizeof(float) * N * N);
    fW = malloc(sizeof(float) * N * N); fWi = malloc(sizeof(float) * N * N); fP = malloc(sizeof(float) * N * N); fPi = malloc(sizeof(float) * N * N);
    fieldFbm(fS, 11, 4, 8, false);
    fieldFbm(fS2, 29, 8, 7, false);
    fieldFbm(fR, 47, 4, 7, true);
    fieldWorley(fW, fWi, 22, 5);
    fieldWorley(fP, fPi, N / 7, 9);

    static const Recipe recipes[TEX_LEAVES] = { recGrass, recScrub, recRock, recSoil, recAsphalt, recGravel, recDirt, recConcrete,
                                                recStucco, recRoof, recBark, recConcrete };
    for (int i = 0; i < TEX_LEAVES; i++) {
        Img im = genGround(recipes[i]);
        tex[i] = upload(im, false);
        if (keepImages) images[i] = im; else free(im.px);
    }
    Img lv = genLeaves(leafW);
    int mips; size_t bytes;
    unsigned char *data = buildAlphaMips(&lv, &mips, &bytes);
    Image ri = { data, lv.w, lv.h, mips, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    tex[TEX_LEAVES] = LoadTextureFromImage(ri);
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
    SetTextureFilter(tex[TEX_LEAVES], TEXTURE_FILTER_ANISOTROPIC_8X);
#endif
    SetTextureWrap(tex[TEX_LEAVES], TEXTURE_WRAP_CLAMP);
    free(data);
    if (keepImages) images[TEX_LEAVES] = lv; else free(lv.px);
    free(fS); free(fS2); free(fR); free(fW); free(fWi); free(fP); free(fPi);
    fS = fS2 = fR = fW = fWi = fP = fPi = NULL;
    TraceLog(LOG_INFO, "TEXTURES: %d x %d ground textures, %d x %d foliage atlas, generated in %.0f ms", N, N, leafW, leafW / 2, (GetTime() - t0) * 1000);
}

void TexturesShutdown(void)
{
    for (int i = 0; i < TEX_COUNT; i++) { if (tex[i].id) UnloadTexture(tex[i]); tex[i] = (Texture2D){ 0 }; }
    for (int i = 0; i < TEX_COUNT; i++) { free(images[i].px); images[i].px = NULL; }
}

Texture2D Tex(int id) { return (id >= 0 && id < TEX_COUNT) ? tex[id] : (Texture2D){ 0 }; }
Rectangle LeafUV(int kind) { return leafRect[kind >= 0 && kind < LEAF_COUNT ? kind : 0]; }

void TexturesExport(const char *dir)
{
    static const char *names[TEX_COUNT] = { "grass", "scrub", "rock", "soil", "asphalt", "gravel", "dirt", "concrete",
                                            "stucco", "roof", "bark", "plaster2", "leaves" };
    if (!images[0].px) {        // regenerate with the CPU copies kept
        keepImages = true;
        int q = N == 256 ? 0 : 2;
        TexturesShutdown();
        TexturesInit(q);
    }
    for (int i = 0; i < TEX_COUNT; i++) {
        if (!images[i].px) continue;
        Image im = { images[i].px, images[i].w, images[i].h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%s.png", dir, names[i]);
        ExportImage(im, path);
    }
}
