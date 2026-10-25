// CarPolo 3D — procedural vehicle models (see car_model.h for conventions).
//
// The body of every car is a loft: vertical cross-sections ("stations") along the length,
// each one a half-profile (bottom, wheel well, body side, greenhouse, roof) mirrored to make
// a closed ring. Plan-view rounding of the nose/tail is applied per point (so the front face
// can lean back at the top), the wheel arches are notches in the lower profile. Every quad of
// the loft is classified (paint, glass, black cladding, trims) and written to the mesh of its
// material kind; smooth normals come from the loft grid itself so material borders do not crease.
// Lights, plates, chevrons, door lines, Airbumps… are thin patches laid on the body surface
// (queried from the same section function), plus a few separate parts (mirrors, rails, spoiler).
#include "car_model.h"
#include "raymath.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PI
#define PI 3.14159265358979323846f
#endif

static float Sq(float a) { return a * a; }
static float Lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static Color Shade(Color c, float k)
{
    return (Color){ (unsigned char)Clampf(c.r * k, 0, 255), (unsigned char)Clampf(c.g * k, 0, 255),
                    (unsigned char)Clampf(c.b * k, 0, 255), c.a };
}

// ------------------------------------------------------------------------------------------
// Mesh builder: one growable, non-indexed triangle list per material kind
// ------------------------------------------------------------------------------------------
typedef struct { float *v, *n, *t; unsigned char *c; int count, cap; } MB;
typedef struct { MB mb[CARMAT_COUNT]; Matrix xf; int useXf; } Builder;

static void MbGrow(MB *b, int add)
{
    if (b->count + add <= b->cap) return;
    int nc = b->cap ? b->cap * 2 : 1024;
    while (nc < b->count + add) nc *= 2;
    b->v = (float *)realloc(b->v, sizeof(float) * 3 * nc);
    b->n = (float *)realloc(b->n, sizeof(float) * 3 * nc);
    b->t = (float *)realloc(b->t, sizeof(float) * 2 * nc);
    b->c = (unsigned char *)realloc(b->c, 4 * nc);
    b->cap = nc;
}

static void Emit(Builder *B, int k, Vector3 p, Vector3 n, Color c, Vector2 uv)
{
    if (B->useXf) {
        p = Vector3Transform(p, B->xf);
        Matrix r = B->xf; r.m12 = r.m13 = r.m14 = 0;
        n = Vector3Normalize(Vector3Transform(n, r));
    }
    MB *b = &B->mb[k];
    MbGrow(b, 1);
    int i = b->count++;
    b->v[i * 3 + 0] = p.x; b->v[i * 3 + 1] = p.y; b->v[i * 3 + 2] = p.z;
    b->n[i * 3 + 0] = n.x; b->n[i * 3 + 1] = n.y; b->n[i * 3 + 2] = n.z;
    b->t[i * 2 + 0] = uv.x; b->t[i * 2 + 1] = uv.y;
    b->c[i * 4 + 0] = c.r; b->c[i * 4 + 1] = c.g; b->c[i * 4 + 2] = c.b; b->c[i * 4 + 3] = 255;
}

static Vector3 TriNormalRaw(Vector3 a, Vector3 b, Vector3 c)
{
    return Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
}

// full control: per-vertex normals, colours and uvs
static void TriEx(Builder *B, int k, Vector3 a, Vector3 b, Vector3 c, Vector3 na, Vector3 nb, Vector3 nc,
                  Color ca, Color cb, Color cc, Vector2 ua, Vector2 ub, Vector2 uc)
{
    Vector3 fn = TriNormalRaw(a, b, c);
    if (Vector3LengthSqr(fn) < 1e-14f) return;
    Emit(B, k, a, na, ca, ua);
    Emit(B, k, b, nb, cb, ub);
    Emit(B, k, c, nc, cc, uc);
}

static void TriSmooth(Builder *B, int k, Vector3 a, Vector3 b, Vector3 c, Vector3 na, Vector3 nb, Vector3 nc, Color col)
{
    Vector2 z = { 0, 0 };
    TriEx(B, k, a, b, c, na, nb, nc, col, col, col, z, z, z);
}

// flat-shaded triangle whose winding is fixed so that its normal points towards `want`
static void TriFacing(Builder *B, int k, Vector3 a, Vector3 b, Vector3 c, Vector3 want, Color col)
{
    Vector3 fn = TriNormalRaw(a, b, c);
    if (Vector3LengthSqr(fn) < 1e-14f) return;
    if (Vector3DotProduct(fn, want) < 0) { Vector3 t = b; b = c; c = t; fn = Vector3Negate(fn); }
    fn = Vector3Normalize(fn);
    TriSmooth(B, k, a, b, c, fn, fn, fn, col);
}

static void QuadFacing(Builder *B, int k, Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 want, Color col)
{
    TriFacing(B, k, a, b, c, want, col);
    TriFacing(B, k, a, c, d, want, col);
}

static void BuilderFree(Builder *B)
{
    for (int k = 0; k < CARMAT_COUNT; k++) {
        free(B->mb[k].v); free(B->mb[k].n); free(B->mb[k].t); free(B->mb[k].c);
    }
    memset(B, 0, sizeof(*B));
}

// Turns the builder into a raylib Model (one mesh + one material per non-empty kind).
static Model BuilderToModel(Builder *B, int *kindsOut, int maxKinds, Texture2D decalTex)
{
    Model model = { 0 };
    model.transform = MatrixIdentity();
    int n = 0;
    for (int k = 0; k < CARMAT_COUNT; k++) if (B->mb[k].count > 0) n++;
    if (n > maxKinds) n = maxKinds;
    model.meshCount = n;
    model.materialCount = n;
    model.meshes = (Mesh *)MemAlloc(sizeof(Mesh) * (n > 0 ? n : 1));
    model.materials = (Material *)MemAlloc(sizeof(Material) * (n > 0 ? n : 1));
    model.meshMaterial = (int *)MemAlloc(sizeof(int) * (n > 0 ? n : 1));
    int i = 0;
    for (int k = 0; k < CARMAT_COUNT && i < n; k++) {
        MB *b = &B->mb[k];
        if (b->count == 0) continue;
        Mesh m = { 0 };
        m.vertexCount = b->count;
        m.triangleCount = b->count / 3;
        m.vertices = (float *)MemAlloc(sizeof(float) * 3 * b->count);
        m.normals = (float *)MemAlloc(sizeof(float) * 3 * b->count);
        m.colors = (unsigned char *)MemAlloc(4 * b->count);
        memcpy(m.vertices, b->v, sizeof(float) * 3 * b->count);
        memcpy(m.normals, b->n, sizeof(float) * 3 * b->count);
        memcpy(m.colors, b->c, 4 * b->count);
        if (k == CARMAT_DECAL) {
            m.texcoords = (float *)MemAlloc(sizeof(float) * 2 * b->count);
            memcpy(m.texcoords, b->t, sizeof(float) * 2 * b->count);
        }
        UploadMesh(&m, false);
        model.meshes[i] = m;
        model.materials[i] = LoadMaterialDefault();
        if (k == CARMAT_DECAL && decalTex.id > 0) model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = decalTex;
        model.meshMaterial[i] = i;
        if (kindsOut) kindsOut[i] = k;
        i++;
    }
    return model;
}

// ------------------------------------------------------------------------------------------
// Monotone cubic curves y(x) given as control points
// ------------------------------------------------------------------------------------------
typedef struct { float p[20][2]; int n; } Crv;

static Crv Cr(int n, ...)
{
    Crv c; memset(&c, 0, sizeof(c));
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 20; i++) { c.p[i][0] = (float)va_arg(ap, double); c.p[i][1] = (float)va_arg(ap, double); }
    va_end(ap);
    c.n = n < 20 ? n : 20;
    return c;
}

static float CvSec(const Crv *c, int i) { return (c->p[i + 1][1] - c->p[i][1]) / (c->p[i + 1][0] - c->p[i][0]); }
static float CvTan(const Crv *c, int i)
{
    if (i <= 0) return CvSec(c, 0);
    if (i >= c->n - 1) return CvSec(c, c->n - 2);
    float d0 = CvSec(c, i - 1), d1 = CvSec(c, i);
    if (d0 * d1 <= 0) return 0;
    return 2.0f / (1.0f / d0 + 1.0f / d1);
}
static float Cv(const Crv *c, float x)
{
    if (c->n <= 1) return c->n ? c->p[0][1] : 0;
    if (x <= c->p[0][0]) return c->p[0][1];
    if (x >= c->p[c->n - 1][0]) return c->p[c->n - 1][1];
    int i = 0;
    while (i < c->n - 2 && x > c->p[i + 1][0]) i++;
    float h = c->p[i + 1][0] - c->p[i][0], t = (x - c->p[i][0]) / h;
    float m0 = CvTan(c, i) * h, m1 = CvTan(c, i + 1) * h;
    float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * c->p[i][1] + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * c->p[i + 1][1] + (t3 - t2) * m1;
}

// ------------------------------------------------------------------------------------------
// Body description
// ------------------------------------------------------------------------------------------
typedef struct {
    float hl;                               // half length
    Crv top, belt, bot, wMax, wSh, wRf, crown;
    Crv endF, endR;                         // z of the nose / tail as a function of height y
    float roundF, roundR, expF, expR;       // plan-view rounding length and superellipse exponent
    float axleZ[2], axleY, archR, xWellIn;  // wheel arches (front, rear)
    float claddingTop, bumperBlackF, bumperBlackR;
    int   archCladding;
    float zWsBase, zWsTop, zB0, zB1, zRearGlass, zRoofEnd, zRwBottom;
    int   cStyle;                           // 0 quarter glass, 1 Cactus C-pillar, 2 panel van
    int   blackFrames, rearGlass;
    Color paint, glass, plastic, under, trim;
} BodySpec;

// A half cross-section: points from the bottom centre (x = 0) up the +X flank to the roof centre.
// zone[q] describes the quad between P[q] and P[q+1]; kc = index of the point lying exactly on the
// black-cladding boundary (quads q in [3, kc) are cladding), so cladding edges are clean curves.
#define MAXP 24
enum { ZN_BOTTOM, ZN_WELL, ZN_LIP, ZN_SIDE, ZN_GREEN, ZN_CORNER, ZN_ROOF };
typedef struct { Vector2 P[MAXP]; int zone[MAXP]; int n, kc; } Profile;

static float ArchY(const BodySpec *s, float z)
{
    float y = -1;
    for (int a = 0; a < 2; a++) {
        float dz = z - s->axleZ[a];
        if (fabsf(dz) < s->archR) {
            float yy = s->axleY - 0.03f + sqrtf(s->archR * s->archR - dz * dz);
            if (yy > y) y = yy;
        }
    }
    return y;
}

// height of the black cladding boundary on the flank at station z (-1 = none)
static float CladY(const BodySpec *s, float z)
{
    float y = -1;
    if (z < s->axleZ[0] && z > s->axleZ[1]) y = s->claddingTop;
    else if (z >= s->axleZ[0]) y = s->bumperBlackF;
    else y = s->bumperBlackR;
    if (s->archCladding) {
        for (int a = 0; a < 2; a++) {
            float dz = z - s->axleZ[a], rr = s->archR + 0.075f;
            if (fabsf(dz) < rr) {
                float yy = s->axleY - 0.03f + sqrtf(rr * rr - dz * dz);
                if (yy > y) y = yy;
            }
        }
    }
    return y;
}

static float EndFactor(const BodySpec *s, float z, float y)
{
    float f = 1.0f;
    float zf = Cv(&s->endF, y);
    if (z > zf - s->roundF) {
        float t = (z - (zf - s->roundF)) / s->roundF;
        if (t >= 1.0f) return 0.0f;
        f *= powf(1.0f - powf(t, s->expF), 1.0f / s->expF);
    }
    float zr = Cv(&s->endR, y);
    if (z < zr + s->roundR) {
        float t = ((zr + s->roundR) - z) / s->roundR;
        if (t >= 1.0f) return 0.0f;
        f *= powf(1.0f - powf(t, s->expR), 1.0f / s->expR);
    }
    return f;
}

static void PrPush(Profile *pr, float x, float y, int zoneNext)
{
    if (pr->n >= MAXP) return;
    pr->P[pr->n] = (Vector2){ x, y };
    pr->zone[pr->n] = zoneNext;
    pr->n++;
}

static void HalfSection(const BodySpec *s, float z, int lod, Profile *pr)
{
    float yb = Cv(&s->bot, z), ybl = Cv(&s->belt, z), yt = Cv(&s->top, z);
    float wm = Cv(&s->wMax, z), wsh = Cv(&s->wSh, z), wrf = Cv(&s->wRf, z), cr = Cv(&s->crown, z);
    if (ybl > yt - 0.01f) ybl = yt - 0.01f;
    float ay = ArchY(s, z);
    int arch = ay > yb + 0.005f;
    if (!arch) ay = yb;
    float wb = arch ? wm - 0.012f : wm - 0.06f;
    float xw = wm - s->xWellIn;
    pr->n = 0;
    PrPush(pr, 0, yb, ZN_BOTTOM);
    PrPush(pr, xw, yb, ZN_WELL);
    PrPush(pr, xw, ay, ZN_WELL);
    PrPush(pr, wb - 0.03f, ay, ZN_LIP);
    float y0 = ay + 0.03f;
    PrPush(pr, wb, y0, ZN_SIDE);
    // flank: x(t) bulges out to wMax at t≈0.42, then tucks in to the shoulder
    float yc = CladY(s, z);
    float tc = (yc - y0) / fmaxf(ybl - y0, 1e-3f);
    pr->kc = 3;
    if (yc > ay - 0.001f) pr->kc = 4;
    float ts[8]; int nt = 0, kIdx = -1;
    float tcc = Clampf(tc, 0.0f, 0.985f);
    if (lod == 0) {
        ts[nt++] = tcc * 0.5f; kIdx = nt; ts[nt++] = tcc;
        const float up[5] = { 0.18f, 0.40f, 0.62f, 0.82f, 1.0f };
        for (int i = 0; i < 5; i++) ts[nt++] = tcc + (1 - tcc) * up[i];
    } else {
        kIdx = nt; ts[nt++] = tcc;
        ts[nt++] = tcc + (1 - tcc) * 0.45f;
        ts[nt++] = 1.0f;
    }
    int base = pr->n;
    for (int i = 0; i < nt; i++) {
        float t = ts[i], tm = 0.42f;
        float y = Lerpf(y0, ybl, t);
        float x = t < tm ? wm - (wm - wb) * Sq(1 - t / tm) : wm - (wm - wsh) * Sq((t - tm) / (1 - tm));
        PrPush(pr, x, y, i == nt - 1 ? ZN_GREEN : ZN_SIDE);
    }
    if (tc > 0.0f) pr->kc = base + kIdx;
    // greenhouse with tumblehome, rounded roof edge, crowned roof
    float g = yt - ybl;
    float rr = fminf(0.085f, fmaxf(0.004f, g * 0.28f));
    float gtop = yt - rr;
    int ng = lod == 0 ? 4 : 2;
    for (int m = 1; m <= ng; m++) {
        float tg = m / (float)ng;
        float y = ybl + (gtop - ybl) * tg;
        float x = Lerpf(wsh - 0.018f, wrf, powf(tg, 0.85f)) + (g > 0.15f ? 0.016f * sinf(PI * tg) : 0.0f);
        PrPush(pr, x, y, m == ng ? ZN_CORNER : ZN_GREEN);
    }
    if (lod == 0) {
        // parabolic crown so the centre line has no crease
        float w0 = wrf - rr * 1.25f;
        PrPush(pr, wrf - rr * 0.62f, yt - rr * 0.24f, ZN_ROOF);
        PrPush(pr, w0, yt + cr * 0.30f, ZN_ROOF);
        PrPush(pr, w0 * 0.55f, yt + cr * (0.30f + 0.70f * (1.0f - 0.55f * 0.55f)), ZN_ROOF);
        PrPush(pr, w0 * 0.22f, yt + cr * (0.30f + 0.70f * (1.0f - 0.22f * 0.22f)), ZN_ROOF);
        PrPush(pr, 0, yt + cr, ZN_ROOF);
    } else {
        PrPush(pr, wrf - rr * 0.9f, yt - rr * 0.1f, ZN_ROOF);
        PrPush(pr, 0, yt + cr, ZN_ROOF);
    }
    for (int k = 0; k < pr->n; k++) pr->P[k].x *= EndFactor(s, z, pr->P[k].y);
}

// outermost half-width of the body at height y in station z (0 if none)
static float HalfWidthAtY(const BodySpec *s, float z, float y)
{
    Profile pr;
    HalfSection(s, z, 0, &pr);
    float best = 0;
    for (int k = 3; k < pr.n - 1; k++) {
        float y0 = pr.P[k].y, y1 = pr.P[k + 1].y;
        if ((y >= y0 && y <= y1) || (y >= y1 && y <= y0)) {
            float t = fabsf(y1 - y0) < 1e-6f ? 0.0f : (y - y0) / (y1 - y0);
            float x = Lerpf(pr.P[k].x, pr.P[k + 1].x, t);
            if (x > best) best = x;
        }
    }
    return best;
}

static int MakeStations(const BodySpec *s, int lod, float *zs, int max)
{
    int n = 0;
    float z = -s->hl;
    zs[n++] = z;
    float k = lod ? 3.0f : 1.4f;
    while (z < s->hl && n < max - 1) {
        float de = s->hl - fabsf(z);
        float d = 0.09f * k;
        if (de < 0.32f) d = (0.016f + de * 0.16f) * (lod ? 2.0f : 1.0f);
        for (int a = 0; a < 2; a++) if (fabsf(z - s->axleZ[a]) < s->archR + 0.05f) d = fminf(d, lod ? 0.10f : 0.052f);
        z += d;
        if (z > s->hl - 0.004f) z = s->hl;
        zs[n++] = z;
    }
    return n;
}

static int Classify(const BodySpec *s, int zone, int q, int kc, float z, float y, Color *col)
{
    switch (zone) {
    case ZN_BOTTOM:
    case ZN_WELL:
        *col = s->under; return CARMAT_PLASTIC;
    case ZN_LIP:
    case ZN_SIDE:
        if (q >= 3 && q < kc) { *col = s->plastic; return CARMAT_PLASTIC; }
        *col = s->paint; return CARMAT_PAINT;
    case ZN_GREEN: {
        if (s->cStyle == 3 || z > s->zWsBase - 0.01f) { *col = s->paint; return CARMAT_PAINT; }
        float gb = Cv(&s->belt, z), gt = Cv(&s->top, z);
        float rel = (y - gb) / fmaxf(gt - gb, 1e-3f);
        if (z <= s->zRoofEnd + 0.01f) {
            if (s->cStyle == 2) { *col = s->paint; return CARMAT_PAINT; }
            if (s->cStyle == 1) {
                if (rel < 0.21f || rel > 0.80f) { *col = s->trim; return CARMAT_PAINT; }
                *col = s->paint; return CARMAT_PAINT;
            }
            *col = s->trim; return CARMAT_PAINT;
        }
        if (z >= s->zB0 && z <= s->zB1) { *col = s->blackFrames ? s->trim : s->paint; return CARMAT_PAINT; }
        if (z > s->zB1) { *col = s->glass; return CARMAT_GLASS; }
        if (z > s->zRearGlass) {
            if (s->cStyle == 2) { *col = s->paint; return CARMAT_PAINT; }
            *col = s->glass; return CARMAT_GLASS;
        }
        if (s->cStyle == 0) {
            if (z > s->zRearGlass - 0.05f) { *col = s->trim; return CARMAT_PAINT; }
            *col = s->glass; return CARMAT_GLASS;
        }
        if (s->cStyle == 1) {
            if (rel < 0.21f || rel > 0.74f) { *col = s->trim; return CARMAT_PAINT; }
            *col = s->paint; return CARMAT_PAINT;
        }
        *col = s->paint; return CARMAT_PAINT;
    }
    case ZN_CORNER:
        if ((z > s->zWsTop - 0.03f && z < s->zWsBase) || (s->rearGlass && z > s->zRwBottom && z < s->zRoofEnd + 0.04f)) {
            *col = s->blackFrames ? s->trim : s->paint; return CARMAT_PAINT;
        }
        *col = s->paint; return CARMAT_PAINT;
    default: // ZN_ROOF
        if (z > s->zWsBase && z < s->zWsBase + 0.075f) { *col = s->plastic; return CARMAT_PLASTIC; }
        if (z >= s->zWsTop && z <= s->zWsBase) { *col = s->glass; return CARMAT_GLASS; }
        if (s->rearGlass && z >= s->zRwBottom && z <= s->zRoofEnd) { *col = s->glass; return CARMAT_GLASS; }
        *col = s->paint; return CARMAT_PAINT;
    }
}

static void BuildBodyLoft(Builder *B, const BodySpec *s, int lod)
{
    float zs[400];
    int S = MakeStations(s, lod, zs, 400);
    Profile pr0;
    HalfSection(s, zs[0], lod, &pr0);
    const int NPp = pr0.n;
    const int R = 2 * NPp - 2;
    Vector3 *G = (Vector3 *)calloc((size_t)S * R, sizeof(Vector3));
    Vector3 *N = (Vector3 *)calloc((size_t)S * R, sizeof(Vector3));
    int *KC = (int *)calloc((size_t)S, sizeof(int));
    int zoneOf[MAXP];
    memcpy(zoneOf, pr0.zone, sizeof(zoneOf));
    for (int i = 0; i < S; i++) {
        Profile pr;
        HalfSection(s, zs[i], lod, &pr);
        KC[i] = pr.kc;
        for (int k = 0; k < NPp; k++) G[i * R + k] = (Vector3){ pr.P[k].x, pr.P[k].y, zs[i] };
        for (int k = 1; k <= NPp - 2; k++) G[i * R + (R - k)] = (Vector3){ -pr.P[k].x, pr.P[k].y, zs[i] };
    }
    for (int i = 0; i < S - 1; i++) {
        for (int j = 0; j < R; j++) {
            int j1 = (j + 1) % R;
            Vector3 a = G[i * R + j], b = G[i * R + j1], c = G[(i + 1) * R + j], d = G[(i + 1) * R + j1];
            Vector3 f = Vector3Add(TriNormalRaw(a, b, c), TriNormalRaw(b, d, c));
            N[i * R + j] = Vector3Add(N[i * R + j], f);
            N[i * R + j1] = Vector3Add(N[i * R + j1], f);
            N[(i + 1) * R + j] = Vector3Add(N[(i + 1) * R + j], f);
            N[(i + 1) * R + j1] = Vector3Add(N[(i + 1) * R + j1], f);
        }
    }
    for (int i = 0; i < S * R; i++) {
        float l = Vector3Length(N[i]);
        N[i] = l > 1e-9f ? Vector3Scale(N[i], 1.0f / l) : (Vector3){ 0, 1, 0 };
    }
    for (int i = 0; i < S - 1; i++) {
        int kc = KC[i] < KC[i + 1] ? KC[i] : KC[i + 1];
        if (KC[i] != KC[i + 1] && (KC[i] <= 4 || KC[i + 1] <= 4)) kc = KC[i] > KC[i + 1] ? KC[i] : KC[i + 1];
        for (int j = 0; j < R; j++) {
            int j1 = (j + 1) % R;
            int half = j < NPp ? j : R - j;
            int half1 = j1 < NPp ? j1 : R - j1;
            int q = half < half1 ? half : half1;
            if (q > NPp - 2) q = NPp - 2;
            Vector3 a = G[i * R + j], b = G[i * R + j1], c = G[(i + 1) * R + j], d = G[(i + 1) * R + j1];
            Vector3 cen = Vector3Scale(Vector3Add(Vector3Add(a, b), Vector3Add(c, d)), 0.25f);
            Color col;
            int k = Classify(s, zoneOf[q], q, kc, cen.z, cen.y, &col);
            Vector3 na = N[i * R + j], nb = N[i * R + j1], nc = N[(i + 1) * R + j], nd = N[(i + 1) * R + j1];
            TriSmooth(B, k, a, b, c, na, nb, nc, col);
            TriSmooth(B, k, b, d, c, nb, nd, nc, col);
        }
    }
    free(G);
    free(N);
    free(KC);
}

// ------------------------------------------------------------------------------------------
// Surface patches: bands around the nose/tail, patches on the flanks
// ------------------------------------------------------------------------------------------
#define CONTOUR_N 220
typedef struct { float x[CONTOUR_N], z[CONTOUR_N], s[CONTOUR_N]; int n; } Contour;

// Plan-view contour of the body at height y, from the centre of the nose (front) or tail
// outwards along the +X flank. s = arc length.
static void EndContour(const BodySpec *s, int front, float y, float span, Contour *C)
{
    float zEnd = front ? Cv(&s->endF, y) : Cv(&s->endR, y);
    float zCut = front ? fminf(zEnd, s->hl) : fmaxf(zEnd, -s->hl);
    int n = 0;
    for (int i = 0; i < CONTOUR_N; i++) {
        float v = i / (float)(CONTOUR_N - 1);
        float d = span * powf(v, 2.6f);
        float z = front ? zCut - d : zCut + d;
        float x = HalfWidthAtY(s, z, y);
        if (n == 0) x = 0;
        C->x[n] = x; C->z[n] = z;
        C->s[n] = n ? C->s[n - 1] + sqrtf(Sq(x - C->x[n - 1]) + Sq(z - C->z[n - 1])) : 0;
        n++;
    }
    C->n = n;
}

// position + outward normal (xz) at arc length t on the contour (t >= 0, +X side)
static void ContourAt(const Contour *C, int front, float t, float *x, float *z, float *nx, float *nz)
{
    int i = 0;
    while (i < C->n - 2 && C->s[i + 1] < t) i++;
    float seg = C->s[i + 1] - C->s[i];
    float u = seg > 1e-6f ? Clampf((t - C->s[i]) / seg, 0, 1) : 0;
    *x = Lerpf(C->x[i], C->x[i + 1], u);
    *z = Lerpf(C->z[i], C->z[i + 1], u);
    float tx = C->x[i + 1] - C->x[i], tz = C->z[i + 1] - C->z[i];
    float l = sqrtf(tx * tx + tz * tz);
    if (l < 1e-6f) { tx = 1; tz = 0; l = 1; }
    tx /= l; tz /= l;
    if (front) { *nx = -tz; *nz = tx; } else { *nx = tz; *nz = -tx; }
}

typedef Color (*PatFn)(float u, float v, void *ud);

typedef struct {
    int kind; Color col; PatFn pat; void *ud;
    Rectangle uv;       // atlas rect (normalised) for decals
    int flipU;
} PatchStyle;

static Color PatchColor(const PatchStyle *st, float u, float v)
{
    return st->pat ? st->pat(u, v, st->ud) : st->col;
}

// Band on the nose (front=1) or tail: heights y0..y1, arc lengths s0..s1 (negative = right side).
static void AddEndBand(Builder *B, const BodySpec *s, int front, float y0, float y1, float s0, float s1,
                       float off, int rows, int cols, PatchStyle st)
{
    Vector3 *Pp = (Vector3 *)malloc(sizeof(Vector3) * (rows + 1) * (cols + 1));
    Vector3 *Nn = (Vector3 *)malloc(sizeof(Vector3) * (rows + 1) * (cols + 1));
    float maxS = fmaxf(fabsf(s0), fabsf(s1)) + 0.4f;
    for (int r = 0; r <= rows; r++) {
        float y = Lerpf(y0, y1, r / (float)rows);
        Contour C;
        EndContour(s, front, y, maxS, &C);
        for (int c = 0; c <= cols; c++) {
            float t = Lerpf(s0, s1, c / (float)cols);
            float x, z, nx, nz;
            ContourAt(&C, front, fabsf(t), &x, &z, &nx, &nz);
            if (t < 0) { x = -x; nx = -nx; }
            Pp[r * (cols + 1) + c] = (Vector3){ x + nx * off, y, z + nz * off };
            Nn[r * (cols + 1) + c] = (Vector3){ nx, 0, nz };
        }
    }
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i00 = r * (cols + 1) + c, i10 = i00 + 1, i01 = i00 + cols + 1, i11 = i01 + 1;
            float u0 = c / (float)cols, u1 = (c + 1) / (float)cols, v0 = r / (float)rows, v1 = (r + 1) / (float)rows;
            if (!front) { // seen from behind the car the band runs right-to-left
                u0 = 1 - u0; u1 = 1 - u1;
            }
            float uc = (u0 + u1) * 0.5f, vc = (v0 + v1) * 0.5f;
            Color col = PatchColor(&st, uc, vc);
            Vector2 t00 = { st.uv.x + u0 * st.uv.width, st.uv.y + (1 - v0) * st.uv.height };
            Vector2 t10 = { st.uv.x + u1 * st.uv.width, st.uv.y + (1 - v0) * st.uv.height };
            Vector2 t01 = { st.uv.x + u0 * st.uv.width, st.uv.y + (1 - v1) * st.uv.height };
            Vector2 t11 = { st.uv.x + u1 * st.uv.width, st.uv.y + (1 - v1) * st.uv.height };
            Vector3 a = Pp[i00], b = Pp[i10], c2 = Pp[i01], d = Pp[i11];
            if (front) {
                TriEx(B, st.kind, a, b, c2, Nn[i00], Nn[i10], Nn[i01], col, col, col, t00, t10, t01);
                TriEx(B, st.kind, b, d, c2, Nn[i10], Nn[i11], Nn[i01], col, col, col, t10, t11, t01);
            } else {
                TriEx(B, st.kind, a, c2, b, Nn[i00], Nn[i01], Nn[i10], col, col, col, t00, t01, t10);
                TriEx(B, st.kind, b, c2, d, Nn[i10], Nn[i01], Nn[i11], col, col, col, t10, t01, t11);
            }
        }
    }
    free(Pp);
    free(Nn);
}

// position on the flank (side = +1 left / -1 right) at (z, y), pushed out by `off`
static Vector3 FlankPoint(const BodySpec *s, int side, float z, float y, float off, Vector3 *nOut)
{
    float x = HalfWidthAtY(s, z, y);
    float e = 0.01f;
    float xz0 = HalfWidthAtY(s, z - e, y), xz1 = HalfWidthAtY(s, z + e, y);
    float xy0 = HalfWidthAtY(s, z, y - e), xy1 = HalfWidthAtY(s, z, y + e);
    // surface x = f(z, y): normal ~ (1, -df/dy, -df/dz)
    Vector3 n = Vector3Normalize((Vector3){ 1.0f, -(xy1 - xy0) / (2 * e), -(xz1 - xz0) / (2 * e) });
    Vector3 p = { x + n.x * off, y + n.y * off, z + n.z * off };
    if (side < 0) { p.x = -p.x; n.x = -n.x; }
    if (nOut) *nOut = n;
    return p;
}

// filled polygon (convex, given in (z, y)) laid on the flank, both sides
static void AddFlankPoly(Builder *B, const BodySpec *s, const Vector2 *pts, int n, float off, PatchStyle st, int bothSides)
{
    float zmin = 1e9f, zmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < n; i++) {
        zmin = fminf(zmin, pts[i].x); zmax = fmaxf(zmax, pts[i].x);
        ymin = fminf(ymin, pts[i].y); ymax = fmaxf(ymax, pts[i].y);
    }
    Vector2 c = { 0, 0 };
    for (int i = 0; i < n; i++) { c.x += pts[i].x / n; c.y += pts[i].y / n; }
    for (int side = 1; side >= -1; side -= 2) {
        if (side < 0 && !bothSides) break;
        Vector3 nc, n0, n1;
        Vector3 pc = FlankPoint(s, side, c.x, c.y, off, &nc);
        for (int i = 0; i < n; i++) {
            Vector2 a = pts[i], b = pts[(i + 1) % n];
            Vector3 pa = FlankPoint(s, side, a.x, a.y, off, &n0);
            Vector3 pb = FlankPoint(s, side, b.x, b.y, off, &n1);
            float ua = (a.x - zmin) / fmaxf(zmax - zmin, 1e-4f), va = (a.y - ymin) / fmaxf(ymax - ymin, 1e-4f);
            float ub = (b.x - zmin) / fmaxf(zmax - zmin, 1e-4f), vb = (b.y - ymin) / fmaxf(ymax - ymin, 1e-4f);
            float ucc = (c.x - zmin) / fmaxf(zmax - zmin, 1e-4f), vcc = (c.y - ymin) / fmaxf(ymax - ymin, 1e-4f);
            // left side (+X): text reads towards -Z; right side towards +Z
            if ((side > 0) != (st.flipU != 0)) { ua = 1 - ua; ub = 1 - ub; ucc = 1 - ucc; }
            Vector2 ta = { st.uv.x + ua * st.uv.width, st.uv.y + (1 - va) * st.uv.height };
            Vector2 tb = { st.uv.x + ub * st.uv.width, st.uv.y + (1 - vb) * st.uv.height };
            Vector2 tc = { st.uv.x + ucc * st.uv.width, st.uv.y + (1 - vcc) * st.uv.height };
            Color col = PatchColor(&st, ucc, vcc);
            Vector3 want = { (float)side, 0, 0 };
            Vector3 fn = TriNormalRaw(pc, pa, pb);
            Vector3 nnc = nc, nn0 = n0, nn1 = n1;
            if (Vector3DotProduct(fn, want) >= 0) TriEx(B, st.kind, pc, pa, pb, nnc, nn0, nn1, col, col, col, tc, ta, tb);
            else TriEx(B, st.kind, pc, pb, pa, nnc, nn1, nn0, col, col, col, tc, tb, ta);
        }
    }
}

// rounded rectangle outline points in (z, y)
static int RoundRectPts(float zc, float yc, float hw, float hh, float rad, Vector2 *out, int perCorner)
{
    int n = 0;
    float cz[4] = { zc + hw - rad, zc - hw + rad, zc - hw + rad, zc + hw - rad };
    float cy[4] = { yc + hh - rad, yc + hh - rad, yc - hh + rad, yc - hh + rad };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= perCorner; i++) {
            float a = (k * 90.0f + 90.0f * i / perCorner) * DEG2RAD;
            out[n++] = (Vector2){ cz[k] + cosf(a) * rad, cy[k] + sinf(a) * rad };
        }
    }
    return n;
}

// outline strip (closed loop) on the flank, width w
static void AddFlankLoop(Builder *B, const BodySpec *s, const Vector2 *pts, int n, float w, float off, int kind, Color col)
{
    for (int side = 1; side >= -1; side -= 2) {
        for (int i = 0; i < n; i++) {
            Vector2 a = pts[i], b = pts[(i + 1) % n];
            Vector2 d = Vector2Normalize(Vector2Subtract(b, a));
            Vector2 nn = { -d.y * w * 0.5f, d.x * w * 0.5f };
            Vector3 p0 = FlankPoint(s, side, a.x - nn.x, a.y - nn.y, off, NULL);
            Vector3 p1 = FlankPoint(s, side, a.x + nn.x, a.y + nn.y, off, NULL);
            Vector3 p2 = FlankPoint(s, side, b.x + nn.x, b.y + nn.y, off, NULL);
            Vector3 p3 = FlankPoint(s, side, b.x - nn.x, b.y - nn.y, off, NULL);
            QuadFacing(B, kind, p0, p1, p2, p3, (Vector3){ (float)side, 0, 0 }, col);
        }
    }
}

// open polyline strip on the flank (door gaps etc.)
static void AddFlankLine(Builder *B, const BodySpec *s, const Vector2 *pts, int n, float w, float off, int kind, Color col)
{
    for (int side = 1; side >= -1; side -= 2) {
        for (int i = 0; i < n - 1; i++) {
            Vector2 a = pts[i], b = pts[i + 1];
            int sub = 4;
            for (int j = 0; j < sub; j++) {
                Vector2 pa = Vector2Lerp(a, b, j / (float)sub), pb = Vector2Lerp(a, b, (j + 1) / (float)sub);
                Vector2 d = Vector2Normalize(Vector2Subtract(pb, pa));
                Vector2 nn = { -d.y * w * 0.5f, d.x * w * 0.5f };
                Vector3 p0 = FlankPoint(s, side, pa.x - nn.x, pa.y - nn.y, off, NULL);
                Vector3 p1 = FlankPoint(s, side, pa.x + nn.x, pa.y + nn.y, off, NULL);
                Vector3 p2 = FlankPoint(s, side, pb.x + nn.x, pb.y + nn.y, off, NULL);
                Vector3 p3 = FlankPoint(s, side, pb.x - nn.x, pb.y - nn.y, off, NULL);
                QuadFacing(B, kind, p0, p1, p2, p3, (Vector3){ (float)side, 0, 0 }, col);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------
// Primitive parts
// ------------------------------------------------------------------------------------------
// axis-aligned box (optionally with different colour on top)
static void AddBox(Builder *B, int kind, Vector3 c, Vector3 h, Color col)
{
    Vector3 v[8];
    for (int i = 0; i < 8; i++) v[i] = (Vector3){ c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z) };
    QuadFacing(B, kind, v[1], v[3], v[7], v[5], (Vector3){ 1, 0, 0 }, col);
    QuadFacing(B, kind, v[0], v[4], v[6], v[2], (Vector3){ -1, 0, 0 }, col);
    QuadFacing(B, kind, v[2], v[6], v[7], v[3], (Vector3){ 0, 1, 0 }, Shade(col, 1.0f));
    QuadFacing(B, kind, v[0], v[1], v[5], v[4], (Vector3){ 0, -1, 0 }, col);
    QuadFacing(B, kind, v[4], v[5], v[7], v[6], (Vector3){ 0, 0, 1 }, col);
    QuadFacing(B, kind, v[0], v[2], v[3], v[1], (Vector3){ 0, 0, -1 }, col);
}

// generic hexahedron from 8 corners (v[0..3] bottom/back ring, v[4..7] the opposite ring),
// each face fixed to point away from the centroid
static void AddHexa(Builder *B, int kind, const Vector3 *v, Color col, Color topCol, int topKind)
{
    Vector3 c = { 0 };
    for (int i = 0; i < 8; i++) c = Vector3Add(c, Vector3Scale(v[i], 0.125f));
    int f[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    for (int k = 0; k < 6; k++) {
        Vector3 a = v[f[k][0]], b = v[f[k][1]], cc = v[f[k][2]], d = v[f[k][3]];
        Vector3 fc = Vector3Scale(Vector3Add(Vector3Add(a, b), Vector3Add(cc, d)), 0.25f);
        Vector3 want = Vector3Subtract(fc, c);
        int up = want.y > 0.7f * Vector3Length(want);
        QuadFacing(B, up ? topKind : kind, a, b, cc, d, want, up ? topCol : col);
    }
}

// tube along a polyline
static void AddTube(Builder *B, int kind, const Vector3 *pts, int n, float rad, int sides, Color col, int caps)
{
    for (int i = 0; i < n - 1; i++) {
        Vector3 a = pts[i], b = pts[i + 1];
        Vector3 d = Vector3Normalize(Vector3Subtract(b, a));
        Vector3 up = fabsf(d.y) < 0.9f ? (Vector3){ 0, 1, 0 } : (Vector3){ 1, 0, 0 };
        Vector3 u = Vector3Normalize(Vector3CrossProduct(d, up));
        Vector3 w = Vector3CrossProduct(u, d);
        for (int k = 0; k < sides; k++) {
            float a0 = 2 * PI * k / sides, a1 = 2 * PI * (k + 1) / sides;
            Vector3 n0 = Vector3Add(Vector3Scale(u, cosf(a0)), Vector3Scale(w, sinf(a0)));
            Vector3 n1 = Vector3Add(Vector3Scale(u, cosf(a1)), Vector3Scale(w, sinf(a1)));
            Vector3 p0 = Vector3Add(a, Vector3Scale(n0, rad)), p1 = Vector3Add(a, Vector3Scale(n1, rad));
            Vector3 p2 = Vector3Add(b, Vector3Scale(n1, rad)), p3 = Vector3Add(b, Vector3Scale(n0, rad));
            Vector3 fn = TriNormalRaw(p0, p1, p2);
            if (Vector3DotProduct(fn, n0) + Vector3DotProduct(fn, n1) >= 0) {
                TriSmooth(B, kind, p0, p1, p2, n0, n1, n1, col);
                TriSmooth(B, kind, p0, p2, p3, n0, n1, n0, col);
            } else {
                TriSmooth(B, kind, p0, p2, p1, n0, n1, n1, col);
                TriSmooth(B, kind, p0, p3, p2, n0, n0, n1, col);
            }
        }
        if (caps && (i == 0 || i == n - 2)) {
            Vector3 cpt = i == 0 ? a : b;
            Vector3 want = i == 0 ? Vector3Negate(d) : d;
            for (int k = 0; k < sides; k++) {
                float a0 = 2 * PI * k / sides, a1 = 2 * PI * (k + 1) / sides;
                Vector3 q0 = Vector3Add(cpt, Vector3Scale(Vector3Add(Vector3Scale(u, cosf(a0)), Vector3Scale(w, sinf(a0))), rad));
                Vector3 q1 = Vector3Add(cpt, Vector3Scale(Vector3Add(Vector3Scale(u, cosf(a1)), Vector3Scale(w, sinf(a1))), rad));
                TriFacing(B, kind, cpt, q0, q1, want, col);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------
// Wheel (tyre + alloy rim), outer face towards +X, centred on the hub
// ------------------------------------------------------------------------------------------
typedef struct {
    float radius, width, rimR;
    int style;            // 0 Cactus two-tone 4 double spokes, 1 generic 5 spokes silver, 2 steel/black (vans/trucks)
    int lod;              // 1 = fewer segments (traffic)
} WheelSpec;

static void AddAnnulus(Builder *B, int kind, float x, float r0, float r1, float a0, float a1, int segs, int facePlusX, Color col)
{
    for (int i = 0; i < segs; i++) {
        float t0 = Lerpf(a0, a1, i / (float)segs), t1 = Lerpf(a0, a1, (i + 1) / (float)segs);
        Vector3 p0 = { x, r0 * cosf(t0), r0 * sinf(t0) }, p1 = { x, r1 * cosf(t0), r1 * sinf(t0) };
        Vector3 p2 = { x, r1 * cosf(t1), r1 * sinf(t1) }, p3 = { x, r0 * cosf(t1), r0 * sinf(t1) };
        QuadFacing(B, kind, p0, p1, p2, p3, (Vector3){ facePlusX ? 1.0f : -1.0f, 0, 0 }, col);
    }
}

// raised spoke arm: polygon in (r, angle) at height x0..x1 (top face silver, walls darker)
static void AddSpokeArm(Builder *B, float xTop, float xBase, float rIn, float rOut,
                        float aIn0, float aIn1, float aOut0, float aOut1, int steps, Color top, Color wall)
{
    for (int i = 0; i < steps; i++) {
        float v0 = i / (float)steps, v1 = (i + 1) / (float)steps;
        float r0 = Lerpf(rIn, rOut, v0), r1 = Lerpf(rIn, rOut, v1);
        float a00 = Lerpf(aIn0, aOut0, v0), a01 = Lerpf(aIn1, aOut1, v0);
        float a10 = Lerpf(aIn0, aOut0, v1), a11 = Lerpf(aIn1, aOut1, v1);
        // slightly domed spokes: lower towards the rim
        float xt0 = xTop - 0.006f * v0, xt1 = xTop - 0.006f * v1;
        Vector3 p00 = { xt0, r0 * cosf(a00), r0 * sinf(a00) }, p01 = { xt0, r0 * cosf(a01), r0 * sinf(a01) };
        Vector3 p10 = { xt1, r1 * cosf(a10), r1 * sinf(a10) }, p11 = { xt1, r1 * cosf(a11), r1 * sinf(a11) };
        QuadFacing(B, CARMAT_RIM, p00, p01, p11, p10, (Vector3){ 1, 0, 0 }, top);
        // side walls
        Vector3 b00 = { xBase, p00.y, p00.z }, b01 = { xBase, p01.y, p01.z }, b10 = { xBase, p10.y, p10.z }, b11 = { xBase, p11.y, p11.z };
        Vector3 c0 = Vector3Scale(Vector3Add(p00, p11), 0.5f);
        Vector3 w0 = Vector3Subtract(Vector3Scale(Vector3Add(p00, p10), 0.5f), c0); w0.x = 0;
        Vector3 w1 = Vector3Subtract(Vector3Scale(Vector3Add(p01, p11), 0.5f), c0); w1.x = 0;
        QuadFacing(B, CARMAT_RIM, p00, p10, b10, b00, w0, wall);
        QuadFacing(B, CARMAT_RIM, p01, p11, b11, b01, w1, wall);
    }
}

static void AddWheel(Builder *B, WheelSpec w)
{
    const int SEG = w.lod ? 14 : 28;
    float R = w.radius, hw = w.width * 0.5f, rr = w.rimR;
    // --- tyre: revolve a profile (x, r) around the X axis, smooth normals
    float prof[][2] = {
        { hw * 0.93f, rr + 0.004f }, { hw * 1.00f, rr + 0.035f }, { hw * 1.04f, Lerpf(rr, R, 0.55f) },
        { hw * 0.99f, R - 0.022f }, { hw * 0.86f, R - 0.004f }, { hw * 0.45f, R }, { 0, R },
        { -hw * 0.45f, R }, { -hw * 0.86f, R - 0.004f }, { -hw * 0.99f, R - 0.022f },
        { -hw * 1.04f, Lerpf(rr, R, 0.55f) }, { -hw * 1.00f, rr + 0.035f }, { -hw * 0.93f, rr + 0.004f } };
    int np = (int)(sizeof(prof) / sizeof(prof[0]));
    float profL[][2] = {
        { hw * 0.93f, rr + 0.004f }, { hw * 1.03f, Lerpf(rr, R, 0.5f) }, { hw * 0.92f, R - 0.01f }, { 0, R },
        { -hw * 0.92f, R - 0.01f }, { -hw * 1.03f, Lerpf(rr, R, 0.5f) }, { -hw * 0.93f, rr + 0.004f } };
    if (w.lod) { np = 7; memcpy(prof, profL, sizeof(profL)); }
    Color rub = { 26, 26, 28, 255 }, side = { 36, 36, 39, 255 };
    float coreR = (R + rr) * 0.5f;
    for (int i = 0; i < np - 1; i++) {
        for (int j = 0; j < SEG; j++) {
            float a0 = 2 * PI * j / SEG, a1 = 2 * PI * (j + 1) / SEG;
            Vector3 p[4] = {
                { prof[i][0], prof[i][1] * cosf(a0), prof[i][1] * sinf(a0) },
                { prof[i + 1][0], prof[i + 1][1] * cosf(a0), prof[i + 1][1] * sinf(a0) },
                { prof[i + 1][0], prof[i + 1][1] * cosf(a1), prof[i + 1][1] * sinf(a1) },
                { prof[i][0], prof[i][1] * cosf(a1), prof[i][1] * sinf(a1) } };
            Vector3 n[4];
            int pi_[4] = { i, i + 1, i + 1, i };
            float ang[4] = { a0, a0, a1, a1 };
            for (int k = 0; k < 4; k++) {
                // normal = direction from the tyre's toroidal core (r = coreR, x = 0) to the point
                float x = prof[pi_[k]][0], r = prof[pi_[k]][1];
                float dx = x, dr = r - coreR;
                float l = sqrtf(dx * dx + dr * dr);
                dx /= l; dr /= l;
                n[k] = (Vector3){ dx, dr * cosf(ang[k]), dr * sinf(ang[k]) };
            }
            int tread = fabsf(prof[i][0]) < hw * 0.9f && fabsf(prof[i + 1][0]) < hw * 0.9f;
            Color cc = tread ? rub : side;
            // grooves on the tread
            if (tread && (j % 2 == 0) && fabsf(prof[i][0]) > hw * 0.3f) cc = Shade(rub, 0.75f);
            Vector3 fn = TriNormalRaw(p[0], p[1], p[2]);
            if (Vector3DotProduct(fn, n[0]) < 0) {
                TriSmooth(B, CARMAT_RUBBER, p[0], p[2], p[1], n[0], n[2], n[1], cc);
                TriSmooth(B, CARMAT_RUBBER, p[0], p[3], p[2], n[0], n[3], n[2], cc);
            } else {
                TriSmooth(B, CARMAT_RUBBER, p[0], p[1], p[2], n[0], n[1], n[2], cc);
                TriSmooth(B, CARMAT_RUBBER, p[0], p[2], p[3], n[0], n[2], n[3], cc);
            }
        }
    }
    // --- rim
    Color silver = { 196, 198, 202, 255 }, silverDark = { 150, 152, 156, 255 };
    Color dark = { 58, 60, 65, 255 }, darker = { 34, 35, 38, 255 };
    float xFace = hw * 0.80f;
    if (w.style == 2) { silver = (Color){ 70, 72, 76, 255 }; silverDark = (Color){ 50, 52, 55, 255 }; dark = (Color){ 40, 41, 44, 255 }; }
    // outer lip
    AddAnnulus(B, CARMAT_RIM, xFace + 0.004f, rr - 0.02f, rr + 0.006f, 0, 2 * PI, SEG, 1, silver);
    // inner lip wall (cylinder from face down to the dish)
    for (int j = 0; j < SEG; j++) {
        float a0 = 2 * PI * j / SEG, a1 = 2 * PI * (j + 1) / SEG, r = rr - 0.02f;
        Vector3 p0 = { xFace + 0.004f, r * cosf(a0), r * sinf(a0) }, p1 = { xFace + 0.004f, r * cosf(a1), r * sinf(a1) };
        Vector3 p2 = { xFace - 0.035f, r * cosf(a1), r * sinf(a1) }, p3 = { xFace - 0.035f, r * cosf(a0), r * sinf(a0) };
        Vector3 inward = { 0, -cosf((a0 + a1) / 2), -sinf((a0 + a1) / 2) };
        QuadFacing(B, CARMAT_RIM, p0, p1, p2, p3, inward, silverDark);
    }
    // dark recessed dish (also closes the wheel so nothing is seen through it)
    float xDish = xFace - 0.035f;
    AddAnnulus(B, CARMAT_RIM, xDish, 0.0f, rr - 0.019f, 0, 2 * PI, SEG, 1, w.style == 2 ? darker : dark);
    AddAnnulus(B, CARMAT_RIM, -hw * 0.6f, 0.0f, rr + 0.004f, 0, 2 * PI, 16, 0, darker);
    // spokes
    float rHub = 0.062f, rOut = rr - 0.022f;
    if (w.style == 0) {
        for (int k = 0; k < 4; k++) {
            float c = k * PI / 2 + PI / 4;
            // each spoke: two arms diverging from the hub with a dark wedge between them
            AddSpokeArm(B, xFace, xDish, rHub, rOut, c + 0.03f, c + 0.48f, c + 0.045f, c + 0.36f, 5, silver, silverDark);
            AddSpokeArm(B, xFace, xDish, rHub, rOut, c - 0.48f, c - 0.03f, c - 0.36f, c - 0.045f, 5, silver, silverDark);
            // dark slot in the middle of each spoke, slightly recessed
            AddSpokeArm(B, xFace - 0.008f, xDish, rHub + 0.04f, rOut - 0.025f, c - 0.03f, c + 0.03f, c - 0.045f, c + 0.045f, 4, dark, darker);
        }
    } else {
        int ns = w.style == 2 ? 6 : 5;
        for (int k = 0; k < ns; k++) {
            float c = k * 2 * PI / ns;
            float wIn = w.style == 2 ? 0.20f : 0.16f, wOut = w.style == 2 ? 0.16f : 0.10f;
            AddSpokeArm(B, xFace, xDish, rHub, rOut, c - wIn, c + wIn, c - wOut, c + wOut, w.lod ? 1 : 4, silver, silverDark);
        }
    }
    // hub / centre cap
    AddAnnulus(B, CARMAT_RIM, xFace + 0.002f, 0.0f, rHub, 0, 2 * PI, 16, 1, w.style == 0 ? dark : silverDark);
    for (int j = 0; j < 16; j++) {
        float a0 = 2 * PI * j / 16, a1 = 2 * PI * (j + 1) / 16;
        Vector3 p0 = { xFace + 0.002f, rHub * cosf(a0), rHub * sinf(a0) }, p1 = { xFace + 0.002f, rHub * cosf(a1), rHub * sinf(a1) };
        Vector3 p2 = { xDish, rHub * cosf(a1), rHub * sinf(a1) }, p3 = { xDish, rHub * cosf(a0), rHub * sinf(a0) };
        QuadFacing(B, CARMAT_RIM, p0, p1, p2, p3, (Vector3){ 0, cosf((a0 + a1) / 2), sinf((a0 + a1) / 2) }, silverDark);
    }
    if (w.style == 0) {
        // tiny double chevron on the cap (in the YZ plane: Y up, Z forward)
        for (int c = 0; c < 2; c++) {
            float yc = -0.008f + c * 0.018f, wv = 0.032f, th = 0.007f, hgt = 0.012f;
            for (int s2 = -1; s2 <= 1; s2 += 2) {
                Vector3 a = { xFace + 0.0035f, yc + hgt, 0 }, b = { xFace + 0.0035f, yc, s2 * wv };
                Vector3 a2 = { xFace + 0.0035f, yc + hgt - th, 0 }, b2 = { xFace + 0.0035f, yc - th, s2 * wv };
                QuadFacing(B, CARMAT_RIM, a, b, b2, a2, (Vector3){ 1, 0, 0 }, silver);
            }
        }
    }
}

static Model BuildWheelModel(WheelSpec w, int *kinds)
{
    Builder B; memset(&B, 0, sizeof(B));
    AddWheel(&B, w);
    Texture2D none = { 0 };
    Model m = BuilderToModel(&B, kinds, 8, none);
    BuilderFree(&B);
    return m;
}

// ------------------------------------------------------------------------------------------
// Decal atlas: number plates and the CACTUS badge
// ------------------------------------------------------------------------------------------
#define ATLAS_W 512
static Rectangle PlateUV(Texture2D atlas) { return (Rectangle){ 0.0f, 0.0f, 1.0f, 112.0f / (float)(atlas.height > 0 ? atlas.height : 256) }; }
static const Rectangle UV_BADGE = { 0.0f, 128.0f / 256.0f, 1.0f, 64.0f / 256.0f };

static void DrawPlate(Image *img, const char *text, int y0)
{
    int w = ATLAS_W, h = 112;
    ImageDrawRectangle(img, 0, y0, w, h, (Color){ 250, 250, 250, 255 });
    ImageDrawRectangleLines(img, (Rectangle){ 2, (float)y0 + 2, (float)w - 4, (float)h - 4 }, 4, (Color){ 20, 20, 20, 255 });
    Color blue = { 0, 51, 153, 255 };
    ImageDrawRectangle(img, 6, y0 + 6, 52, h - 12, blue);
    ImageDrawRectangle(img, w - 58, y0 + 6, 52, h - 12, blue);
    for (int i = 0; i < 12; i++) {   // EU stars
        float a = i * 2 * PI / 12;
        ImageDrawCircle(img, 32 + (int)(15 * cosf(a)), y0 + 38 + (int)(15 * sinf(a)), 2, (Color){ 255, 204, 0, 255 });
    }
    ImageDrawText(img, "F", 22, y0 + 64, 34, WHITE);
    ImageDrawText(img, "13", 462, y0 + 62, 30, WHITE);
    ImageDrawCircle(img, w - 32, y0 + 36, 12, (Color){ 255, 204, 0, 255 });
    int fs = 64;
    int tw = MeasureText(text, fs);
    ImageDrawText(img, text, (w - tw) / 2, y0 + (h - fs) / 2 + 4, fs, (Color){ 15, 15, 15, 255 });
}

static Texture2D MakeDecalAtlas(const char *plate, int badge)
{
    Image img = GenImageColor(ATLAS_W, badge ? 256 : 128, (Color){ 14, 15, 17, 255 });
    DrawPlate(&img, plate, 0);
    if (badge) {
        const char *t = "C A C T U S";
        int fs = 44, tw = MeasureText(t, fs);
        ImageDrawText(&img, t, (ATLAS_W - tw) / 2, 128 + 12, fs, (Color){ 200, 202, 206, 255 });
    }
    Texture2D tex = LoadTextureFromImage(img);
    GenTextureMipmaps(&tex);
    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    UnloadImage(img);
    return tex;
}

// ------------------------------------------------------------------------------------------
// Shared decoration helpers
// ------------------------------------------------------------------------------------------
static PatchStyle Solid(int kind, Color c) { PatchStyle s; memset(&s, 0, sizeof(s)); s.kind = kind; s.col = c; s.uv = (Rectangle){ 0, 0, 1, 1 }; return s; }

static Color PatGrille(float u, float v, void *ud)
{
    (void)u; (void)ud;
    int bar = ((int)(v * 7.0f)) % 2;
    return bar ? (Color){ 44, 45, 48, 255 } : (Color){ 14, 14, 16, 255 };
}

static Color PatHeadlight(float u, float v, void *ud)
{
    (void)ud;
    float d = fabsf(fmodf(u * 3.0f, 1.0f) - 0.5f);
    if (v < 0.2f || v > 0.82f) return (Color){ 30, 32, 36, 255 };
    if (d < 0.16f && v > 0.32f && v < 0.72f) return (Color){ 240, 244, 250, 255 };
    return (Color){ 105, 110, 120, 255 };
}

static Color PatTail(float u, float v, void *ud)
{
    (void)ud;
    // "3D" segments: three bright bars with dark gaps, darker towards the inner end
    float k = 0.75f + 0.25f * u;
    if (fmodf(v * 3.0f, 1.0f) < 0.28f) return (Color){ (unsigned char)(95 * k), 8, 12, 255 };
    return (Color){ (unsigned char)(215 * k), (unsigned char)(26 * k), 30, 255 };
}

// chevron (^) made of two extruded arms, placed on the nose/tail at height yc, centred on x = 0
static void AddChevron(Builder *B, const BodySpec *s, int front, float yc, float halfW, float hgt, float thick, float off, float depth, Color col)
{
    Contour C;
    EndContour(s, front, yc, 0.4f, &C);
    float zc = C.z[0];
    float dir = front ? 1.0f : -1.0f;
    for (int side = -1; side <= 1; side += 2) {
        // arm from apex (0, yc + hgt/2) to tip (side*halfW, yc - hgt/2)
        Vector2 apex = { 0, yc + hgt * 0.5f }, tip = { side * halfW, yc - hgt * 0.5f };
        Vector2 q[4] = { apex, tip, { tip.x, tip.y - thick }, { apex.x, apex.y - thick } };
        Vector3 f[4], b[4];
        for (int i = 0; i < 4; i++) {
            float x = q[i].x;
            // follow the face curvature: z from the contour at |x|
            float zz = zc;
            for (int j = 1; j < C.n; j++) if (C.x[j] >= fabsf(x)) { float t = (fabsf(x) - C.x[j - 1]) / fmaxf(C.x[j] - C.x[j - 1], 1e-5f); zz = Lerpf(C.z[j - 1], C.z[j], t); break; }
            f[i] = (Vector3){ x, q[i].y, zz + dir * (off + depth) };
            b[i] = (Vector3){ x, q[i].y, zz + dir * off };
        }
        Vector3 out = { 0, 0, dir };
        QuadFacing(B, CARMAT_CHROME, f[0], f[1], f[2], f[3], out, col);
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            Vector3 mid = Vector3Scale(Vector3Add(f[i], f[j]), 0.5f);
            Vector3 cen = Vector3Scale(Vector3Add(Vector3Add(f[0], f[1]), Vector3Add(f[2], f[3])), 0.25f);
            Vector3 w = Vector3Subtract(mid, cen); w.z = 0;
            QuadFacing(B, CARMAT_CHROME, f[i], f[j], b[j], b[i], w, Shade(col, 0.8f));
        }
    }
}

static void AddMirror(Builder *B, int side, Vector3 head, Vector3 rad, Vector3 base, Color shell, int chromeStrip)
{
    Vector3 c = { side * head.x, head.y, head.z };
    // shell: ellipsoid; the rear-facing part is the mirror glass, the lower outer edge a chrome indicator
    for (int i = 0; i < 6; i++) {
        float t0 = PI * i / 6 - PI / 2, t1 = PI * (i + 1) / 6 - PI / 2;
        for (int j = 0; j < 12; j++) {
            float p0 = 2 * PI * j / 12, p1 = 2 * PI * (j + 1) / 12;
            Vector3 u[4] = {
                { cosf(t0) * cosf(p0), sinf(t0), cosf(t0) * sinf(p0) }, { cosf(t0) * cosf(p1), sinf(t0), cosf(t0) * sinf(p1) },
                { cosf(t1) * cosf(p1), sinf(t1), cosf(t1) * sinf(p1) }, { cosf(t1) * cosf(p0), sinf(t1), cosf(t1) * sinf(p0) } };
            Vector3 p[4], n[4];
            for (int k = 0; k < 4; k++) {
                Vector3 uu = u[k];
                if (uu.z < -0.3f) uu.z = -0.3f - (uu.z + 0.3f) * 0.15f;   // flat back (mirror glass)
                p[k] = (Vector3){ c.x + uu.x * rad.x, c.y + uu.y * rad.y, c.z + uu.z * rad.z };
                n[k] = Vector3Normalize((Vector3){ uu.x / rad.x, uu.y / rad.y, uu.z / rad.z });
            }
            Vector3 um = Vector3Scale(Vector3Add(Vector3Add(u[0], u[1]), Vector3Add(u[2], u[3])), 0.25f);
            int kind = CARMAT_PAINT; Color col = shell;
            if (um.z < -0.45f) { kind = CARMAT_GLASS; col = (Color){ 120, 130, 140, 255 }; }
            else if (chromeStrip && um.y < -0.45f && um.y > -0.85f && um.x * side > -0.2f) { kind = CARMAT_CHROME; col = (Color){ 205, 208, 212, 255 }; }
            Vector3 fn = TriNormalRaw(p[0], p[1], p[2]);
            if (Vector3DotProduct(fn, Vector3Subtract(p[0], c)) < 0) {
                TriSmooth(B, kind, p[0], p[2], p[1], n[0], n[2], n[1], col);
                TriSmooth(B, kind, p[0], p[3], p[2], n[0], n[3], n[2], col);
            } else {
                TriSmooth(B, kind, p[0], p[1], p[2], n[0], n[1], n[2], col);
                TriSmooth(B, kind, p[0], p[2], p[3], n[0], n[2], n[3], col);
            }
        }
    }
    // arm
    Vector3 b = { side * base.x, base.y, base.z };
    Vector3 h = { c.x - side * rad.x * 0.6f, c.y - rad.y * 0.2f, c.z + 0.01f };
    Vector3 pts[2] = { b, h };
    AddTube(B, CARMAT_PAINT, pts, 2, 0.022f, 6, shell, 0);
}

// door gaps, handles: shared by all passenger cars
static void AddDoorLines(Builder *B, const BodySpec *s, const Vector2 *const *lines, const int *counts, int nLines, Color gap)
{
    for (int i = 0; i < nLines; i++) AddFlankLine(B, s, lines[i], counts[i], 0.0055f, 0.0018f, CARMAT_PLASTIC, gap);
}

static void AddHandle(Builder *B, const BodySpec *s, float z0, float z1, float yc, Color paint)
{
    Vector2 pts[24];
    int n = RoundRectPts((z0 + z1) * 0.5f, yc, (z1 - z0) * 0.5f, 0.017f, 0.012f, pts, 4);
    AddFlankPoly(B, s, pts, n, 0.012f, Solid(CARMAT_PAINT, Shade(paint, 1.08f)), 1);
    Vector2 rec[24];
    int m = RoundRectPts((z0 + z1) * 0.5f + 0.01f, yc - 0.006f, (z1 - z0) * 0.42f, 0.02f, 0.012f, rec, 3);
    AddFlankPoly(B, s, rec, m, 0.003f, Solid(CARMAT_PLASTIC, (Color){ 10, 12, 14, 255 }), 1);
}

// ------------------------------------------------------------------------------------------
// The C4 Cactus phase 2
// ------------------------------------------------------------------------------------------
static BodySpec CactusSpec(void)
{
    BodySpec s; memset(&s, 0, sizeof(s));
    s.hl = 2.085f;
    s.top = Cr(19, -2.085, 0.985, -2.035, 1.035, -1.975, 1.085, -1.89, 1.21, -1.79, 1.34, -1.69, 1.42, -1.52, 1.452,
               -0.60, 1.462, 0.05, 1.458, 0.22, 1.432, 0.50, 1.27, 0.80, 1.085, 0.95, 1.005, 1.10, 0.978, 1.35, 0.958,
               1.70, 0.912, 1.90, 0.862, 2.00, 0.805, 2.085, 0.68);
    s.belt = Cr(14, -2.085, 0.935, -1.95, 1.0, -1.72, 1.045, -1.35, 1.03, -1.08, 0.995, 0.0, 0.975, 0.80, 0.966,
                0.95, 0.958, 1.10, 0.945, 1.35, 0.925, 1.70, 0.872, 1.90, 0.82, 2.00, 0.775, 2.085, 0.665);
    s.bot = Cr(8, -2.085, 0.37, -1.95, 0.31, -1.76, 0.275, -1.40, 0.245, 1.30, 0.245, 1.72, 0.27, 1.95, 0.30, 2.085, 0.34);
    s.wMax = Cr(6, -2.085, 0.80, -1.90, 0.845, -1.40, 0.855, 1.30, 0.855, 1.85, 0.842, 2.085, 0.80);
    s.wSh = Cr(7, -2.085, 0.765, -1.80, 0.80, -1.20, 0.812, 0.90, 0.806, 1.40, 0.792, 1.85, 0.772, 2.085, 0.725);
    s.wRf = Cr(11, -2.085, 0.56, -1.95, 0.60, -1.70, 0.625, -1.20, 0.628, 0.05, 0.625, 0.22, 0.63, 0.60, 0.685,
               0.92, 0.738, 0.96, 0.70, 1.15, 0.56, 2.085, 0.46);
    s.crown = Cr(9, -2.085, 0.0, -1.90, 0.012, -1.60, 0.022, 0.15, 0.022, 0.35, 0.04, 0.92, 0.058, 1.02, 0.035, 2.0, 0.03, 2.085, 0.02);
    s.endF = Cr(9, 0.20, 2.03, 0.30, 2.07, 0.42, 2.085, 0.58, 2.085, 0.68, 2.075, 0.76, 2.05, 0.84, 2.005, 0.92, 1.94, 1.0, 1.87);
    s.endR = Cr(8, 0.25, -2.035, 0.37, -2.075, 0.50, -2.085, 0.90, -2.085, 1.00, -2.068, 1.08, -2.04, 1.20, -1.98, 1.5, -1.85);
    s.roundF = 0.36f; s.expF = 2.15f;
    s.roundR = 0.26f; s.expR = 2.6f;
    s.axleZ[0] = 1.245f; s.axleZ[1] = -1.355f; s.axleY = 0.32f; s.archR = 0.395f; s.xWellIn = 0.30f;
    s.claddingTop = 0.455f; s.bumperBlackF = 0.405f; s.bumperBlackR = 0.475f; s.archCladding = 1;
    s.zWsBase = 0.95f; s.zWsTop = 0.22f; s.zB0 = -0.07f; s.zB1 = 0.05f; s.zRearGlass = -1.08f; s.zRoofEnd = -1.66f; s.zRwBottom = -1.985f;
    s.cStyle = 1; s.blackFrames = 1; s.rearGlass = 1;
    s.paint = (Color){ 24, 84, 110, 255 };      // "Bleu Émeraude"
    s.glass = (Color){ 16, 21, 26, 255 };
    s.plastic = (Color){ 30, 31, 33, 255 };
    s.under = (Color){ 16, 16, 18, 255 };
    s.trim = (Color){ 10, 11, 13, 255 };
    return s;
}

static void DecorateCactus(Builder *B, const BodySpec *s, Texture2D atlas)
{
    const Rectangle UV_PLATE = PlateUV(atlas);
    Color chrome = { 210, 213, 218, 255 };
    Color trim = s->trim;
    // ---- nose
    PatchStyle grille = Solid(CARMAT_PLASTIC, (Color){ 20, 20, 22, 255 }); grille.pat = PatGrille;
    AddEndBand(B, s, 1, 0.33f, 0.525f, -0.50f, 0.50f, 0.004f, 6, 24, grille);
    PatchStyle plate = Solid(CARMAT_DECAL, WHITE); plate.uv = UV_PLATE;
    AddEndBand(B, s, 1, 0.415f, 0.525f, -0.26f, 0.26f, 0.012f, 2, 16, plate);
    // headlights low in the bumper, black housing behind
    AddEndBand(B, s, 1, 0.565f, 0.695f, 0.40f, 0.88f, 0.004f, 3, 14, Solid(CARMAT_PAINT, trim));
    AddEndBand(B, s, 1, 0.565f, 0.695f, -0.88f, -0.40f, 0.004f, 3, 14, Solid(CARMAT_PAINT, trim));
    PatchStyle hl = Solid(CARMAT_LIGHT_FRONT, WHITE); hl.pat = PatHeadlight;
    AddEndBand(B, s, 1, 0.585f, 0.675f, 0.43f, 0.84f, 0.008f, 3, 12, hl);
    AddEndBand(B, s, 1, 0.585f, 0.675f, -0.84f, -0.43f, 0.008f, 3, 12, hl);
    // slim LED daytime lights, chrome bars from the chevron, double chevron
    AddEndBand(B, s, 1, 0.784f, 0.812f, 0.45f, 0.86f, 0.006f, 1, 12, Solid(CARMAT_LIGHT_FRONT, (Color){ 250, 252, 255, 255 }));
    AddEndBand(B, s, 1, 0.784f, 0.812f, -0.86f, -0.45f, 0.006f, 1, 12, Solid(CARMAT_LIGHT_FRONT, (Color){ 250, 252, 255, 255 }));
    AddEndBand(B, s, 1, 0.786f, 0.802f, 0.14f, 0.46f, 0.0065f, 1, 8, Solid(CARMAT_CHROME, chrome));
    AddEndBand(B, s, 1, 0.786f, 0.802f, -0.46f, -0.14f, 0.0065f, 1, 8, Solid(CARMAT_CHROME, chrome));
    AddChevron(B, s, 1, 0.818f, 0.150f, 0.050f, 0.024f, 0.004f, 0.014f, chrome);
    AddChevron(B, s, 1, 0.770f, 0.150f, 0.050f, 0.024f, 0.004f, 0.014f, chrome);
    // fog/indicator dots in the lower bumper corners
    AddEndBand(B, s, 1, 0.365f, 0.395f, 0.64f, 0.73f, 0.005f, 1, 4, Solid(CARMAT_LIGHT_FRONT, (Color){ 120, 122, 120, 255 }));
    AddEndBand(B, s, 1, 0.365f, 0.395f, -0.73f, -0.64f, 0.005f, 1, 4, Solid(CARMAT_LIGHT_FRONT, (Color){ 120, 122, 120, 255 }));

    // ---- tail
    AddEndBand(B, s, 0, 0.850f, 0.998f, 0.40f, 1.03f, 0.003f, 3, 16, Solid(CARMAT_PAINT, trim));
    AddEndBand(B, s, 0, 0.850f, 0.998f, -1.03f, -0.40f, 0.003f, 3, 16, Solid(CARMAT_PAINT, trim));
    PatchStyle tl = Solid(CARMAT_LIGHT_REAR, RED); tl.pat = PatTail;
    AddEndBand(B, s, 0, 0.866f, 0.982f, 0.43f, 1.00f, 0.007f, 6, 14, tl);
    AddEndBand(B, s, 0, 0.866f, 0.982f, -1.00f, -0.43f, 0.007f, 6, 14, tl);
    AddEndBand(B, s, 0, 0.615f, 0.725f, -0.26f, 0.26f, 0.006f, 2, 16, plate);
    AddChevron(B, s, 0, 0.835f, 0.085f, 0.03f, 0.014f, 0.004f, 0.008f, chrome);
    AddChevron(B, s, 0, 0.807f, 0.085f, 0.03f, 0.014f, 0.004f, 0.008f, chrome);
    // reflectors in the black lower bumper
    AddEndBand(B, s, 0, 0.385f, 0.41f, 0.50f, 0.74f, 0.004f, 1, 6, Solid(CARMAT_LIGHT_REAR, (Color){ 150, 12, 16, 255 }));
    AddEndBand(B, s, 0, 0.385f, 0.41f, -0.74f, -0.50f, 0.004f, 1, 6, Solid(CARMAT_LIGHT_REAR, (Color){ 150, 12, 16, 255 }));

    // ---- flanks: door gaps, handles, Airbumps, CACTUS badge
    Color gap = { 8, 14, 18, 255 };
    Vector2 l1[] = { { 0.80f, 0.462f }, { 0.84f, 0.70f }, { 0.89f, 0.90f }, { 0.925f, 0.958f } };
    Vector2 l2[] = { { 0.025f, 0.462f }, { 0.012f, 0.73f }, { 0.0f, 0.97f } };
    Vector2 l3[] = { { -0.915f, 0.462f }, { -0.955f, 0.72f }, { -1.03f, 0.915f }, { -1.075f, 0.99f } };
    const Vector2 *lines[] = { l1, l2, l3 };
    int counts[] = { 4, 3, 4 };
    AddDoorLines(B, s, lines, counts, 3, gap);
    AddHandle(B, s, 0.16f, 0.37f, 0.895f, s->paint);
    AddHandle(B, s, -0.86f, -0.65f, 0.912f, s->paint);
    Vector2 rr[40];
    // Airbump loop (white) on the front door, dark recesses
    int n = RoundRectPts(0.58f, 0.375f, 0.135f, 0.026f, 0.024f, rr, 5);
    AddFlankLoop(B, s, rr, n, 0.013f, 0.007f, CARMAT_PLASTIC, (Color){ 222, 220, 210, 255 });
    n = RoundRectPts(0.21f, 0.37f, 0.12f, 0.024f, 0.02f, rr, 4);
    AddFlankPoly(B, s, rr, n, 0.004f, Solid(CARMAT_PLASTIC, (Color){ 18, 18, 20, 255 }), 1);
    n = RoundRectPts(-0.57f, 0.37f, 0.13f, 0.024f, 0.02f, rr, 4);
    AddFlankPoly(B, s, rr, n, 0.004f, Solid(CARMAT_PLASTIC, (Color){ 18, 18, 20, 255 }), 1);
    // CACTUS badge on the black C-pillar band
    PatchStyle badge = Solid(CARMAT_DECAL, WHITE); badge.uv = UV_BADGE;
    float bz0 = -1.50f, bz1 = -1.29f, byc = Cv(&s->belt, -1.40f) + 0.034f;
    Vector2 bq[4] = { { bz1, byc - 0.016f }, { bz1, byc + 0.016f }, { bz0, byc + 0.016f }, { bz0, byc - 0.016f } };
    AddFlankPoly(B, s, bq, 4, 0.005f, badge, 1);
    // fuel flap outline (right side only would be correct; both is fine at game distance)
    n = RoundRectPts(-1.64f, 0.84f, 0.09f, 0.07f, 0.03f, rr, 4);
    AddFlankLoop(B, s, rr, n, 0.004f, 0.0016f, CARMAT_PLASTIC, gap);

    // ---- mirrors (glossy black caps, chrome indicator strip)
    for (int side = -1; side <= 1; side += 2)
        AddMirror(B, side, (Vector3){ 0.955f, 1.045f, 0.77f }, (Vector3){ 0.115f, 0.068f, 0.082f },
                  (Vector3){ 0.80f, 1.0f, 0.80f }, trim, 1);

    // ---- roof bars (satin black), roof spoiler, antenna, wipers
    for (int side = -1; side <= 1; side += 2) {
        Vector3 pts[12];
        int np = 0;
        for (int i = 0; i <= 10; i++) {
            float z = Lerpf(-1.43f, -0.02f, i / 10.0f);
            pts[np++] = (Vector3){ side * 0.535f, Cv(&s->top, z) + Cv(&s->crown, z) * 0.45f + 0.034f, z };
        }
        AddTube(B, CARMAT_PLASTIC, pts, np, 0.016f, 6, (Color){ 22, 23, 25, 255 }, 1);
        for (int f = 0; f < 2; f++) {
            float z = f ? -0.06f : -1.39f;
            float y = Cv(&s->top, z) + Cv(&s->crown, z) * 0.45f;
            AddBox(B, CARMAT_PLASTIC, (Vector3){ side * 0.535f, y + 0.015f, z }, (Vector3){ 0.016f, 0.02f, 0.045f }, (Color){ 22, 23, 25, 255 });
        }
    }
    {
        // spoiler: body-colour top, black underside, overhanging the rear window
        float x0 = 0.60f, x1 = 0.56f;
        Vector3 v[8] = {
            { -x0, 1.405f, -1.62f }, { x0, 1.405f, -1.62f }, { x1, 1.405f, -1.79f }, { -x1, 1.405f, -1.79f },
            { -x0, 1.462f, -1.62f }, { x0, 1.462f, -1.62f }, { x1, 1.438f, -1.80f }, { -x1, 1.438f, -1.80f } };
        AddHexa(B, CARMAT_PAINT, v, s->paint, s->paint, CARMAT_PAINT);
        Vector3 u[4] = { { -x1, 1.404f, -1.79f }, { x1, 1.404f, -1.79f }, { x0, 1.404f, -1.66f }, { -x0, 1.404f, -1.66f } };
        QuadFacing(B, CARMAT_PAINT, u[0], u[1], u[2], u[3], (Vector3){ 0, -1, 0 }, trim);
        // third brake light under the lip
        AddBox(B, CARMAT_LIGHT_REAR, (Vector3){ 0, 1.412f, -1.795f }, (Vector3){ 0.12f, 0.006f, 0.008f }, (Color){ 170, 15, 20, 255 });
        Vector3 ant[2] = { { 0, Cv(&s->top, -1.40f) + 0.01f, -1.40f }, { 0, Cv(&s->top, -1.40f) + 0.40f, -1.62f } };
        AddTube(B, CARMAT_PLASTIC, ant, 2, 0.005f, 5, (Color){ 15, 15, 16, 255 }, 1);
        AddBox(B, CARMAT_PLASTIC, (Vector3){ 0, Cv(&s->top, -1.40f) + 0.01f, -1.40f }, (Vector3){ 0.02f, 0.012f, 0.03f }, (Color){ 15, 15, 16, 255 });
    }
    for (int w = 0; w < 2; w++) {
        Vector3 pts[2];
        float x0 = w ? 0.06f : -0.62f, x1 = w ? 0.62f : -0.02f;
        float z0 = 0.905f, z1 = 0.88f;
        pts[0] = (Vector3){ x0, Cv(&s->top, z0) + 0.012f, z0 };
        pts[1] = (Vector3){ x1, Cv(&s->top, z1) + 0.012f + Cv(&s->crown, z1) * 0.5f, z1 };
        AddTube(B, CARMAT_PLASTIC, pts, 2, 0.008f, 5, (Color){ 15, 15, 16, 255 }, 1);
    }
    // ---- wheel-well liners (black discs behind the wheels so the body is never seen through)
    for (int a = 0; a < 2; a++)
        for (int side = -1; side <= 1; side += 2) {
            float x = side * (Cv(&s->wMax, s->axleZ[a]) - s->xWellIn + 0.005f);
            for (int i = 0; i < 12; i++) {
                float t0 = PI * i / 12, t1 = PI * (i + 1) / 12;
                float r = s->archR + 0.01f;
                Vector3 c = { x, s->axleY - 0.03f, s->axleZ[a] };
                Vector3 p0 = { x, c.y + r * sinf(t0), c.z + r * cosf(t0) }, p1 = { x, c.y + r * sinf(t1), c.z + r * cosf(t1) };
                TriFacing(B, CARMAT_PLASTIC, c, p0, p1, (Vector3){ (float)side, 0, 0 }, (Color){ 8, 8, 9, 255 });
            }
        }
}

CarModel BuildCactusModel(void)
{
    CarModel m; memset(&m, 0, sizeof(m));
    BodySpec s = CactusSpec();
    Texture2D atlas = MakeDecalAtlas("GQ-813-PR", 1);
    Builder B; memset(&B, 0, sizeof(B));
    BuildBodyLoft(&B, &s, 0);
    DecorateCactus(&B, &s, atlas);
    m.body = BuilderToModel(&B, m.bodyMatKind, 16, atlas);
    BuilderFree(&B);
    WheelSpec ws = { 0.32f, 0.205f, 0.216f, 0, 0 };
    m.wheel = BuildWheelModel(ws, m.wheelMatKind);
    float tr = 0.74f;
    m.wheelPos[0] = (Vector3){ tr, s.axleY, s.axleZ[0] };
    m.wheelPos[1] = (Vector3){ -tr, s.axleY, s.axleZ[0] };
    m.wheelPos[2] = (Vector3){ tr, s.axleY, s.axleZ[1] };
    m.wheelPos[3] = (Vector3){ -tr, s.axleY, s.axleZ[1] };
    m.wheelRadius = 0.32f;
    m.length = 4.17f; m.width = 1.71f; m.height = 1.48f;
    return m;
}

// ------------------------------------------------------------------------------------------
// Traffic vehicles
// ------------------------------------------------------------------------------------------
static void DecorateGeneric(Builder *B, const BodySpec *s, Texture2D atlas, int kind, Color paint, float hlY0, float hlY1, float hlS0, float hlS1,
                            float tlY0, float tlY1, float tlS0, float tlS1, float plateFY, float plateRY)
{
    Color trim = s->trim;
    PatchStyle grille = Solid(CARMAT_PLASTIC, (Color){ 20, 20, 22, 255 }); grille.pat = PatGrille;
    float gy0 = plateFY - 0.10f, gy1 = plateFY + 0.06f;
    AddEndBand(B, s, 1, gy0, gy1, -0.42f, 0.42f, 0.004f, 4, 14, grille);
    if (kind != 3) {
        AddEndBand(B, s, 1, hlY0 - 0.01f, hlY1 + 0.01f, hlS0 - 0.02f, hlS1 + 0.02f, 0.004f, 2, 8, Solid(CARMAT_PAINT, trim));
        AddEndBand(B, s, 1, hlY0 - 0.01f, hlY1 + 0.01f, -hlS1 - 0.02f, -hlS0 + 0.02f, 0.004f, 2, 8, Solid(CARMAT_PAINT, trim));
    }
    PatchStyle hl = Solid(CARMAT_LIGHT_FRONT, WHITE); hl.pat = PatHeadlight;
    AddEndBand(B, s, 1, hlY0, hlY1, hlS0, hlS1, 0.008f, 2, 8, hl);
    AddEndBand(B, s, 1, hlY0, hlY1, -hlS1, -hlS0, 0.008f, 2, 8, hl);
    PatchStyle tl = Solid(CARMAT_LIGHT_REAR, RED); tl.pat = PatTail;
    AddEndBand(B, s, 0, tlY0, tlY1, tlS0, tlS1, 0.006f, 3, 8, tl);
    AddEndBand(B, s, 0, tlY0, tlY1, -tlS1, -tlS0, 0.006f, 3, 8, tl);
    PatchStyle plate = Solid(CARMAT_DECAL, WHITE); plate.uv = PlateUV(atlas);
    AddEndBand(B, s, 1, plateFY - 0.055f, plateFY + 0.055f, -0.26f, 0.26f, 0.012f, 2, 10, plate);
    AddEndBand(B, s, 0, plateRY - 0.055f, plateRY + 0.055f, -0.26f, 0.26f, 0.008f, 2, 10, plate);
    (void)paint;
    // wheel-well liners
    for (int a = 0; a < 2; a++)
        for (int side = -1; side <= 1; side += 2) {
            float x = side * (Cv(&s->wMax, s->axleZ[a]) - s->xWellIn + 0.005f);
            for (int i = 0; i < 10; i++) {
                float t0 = PI * i / 10, t1 = PI * (i + 1) / 10, r = s->archR + 0.01f;
                Vector3 c = { x, s->axleY - 0.03f, s->axleZ[a] };
                Vector3 p0 = { x, c.y + r * sinf(t0), c.z + r * cosf(t0) }, p1 = { x, c.y + r * sinf(t1), c.z + r * cosf(t1) };
                TriFacing(B, CARMAT_PLASTIC, c, p0, p1, (Vector3){ (float)side, 0, 0 }, (Color){ 8, 8, 9, 255 });
            }
        }
}

static BodySpec BaseCar(Color paint)
{
    BodySpec s; memset(&s, 0, sizeof(s));
    s.paint = paint;
    s.glass = (Color){ 18, 23, 28, 255 };
    s.plastic = (Color){ 28, 29, 31, 255 };
    s.under = (Color){ 16, 16, 18, 255 };
    s.trim = (Color){ 12, 13, 15, 255 };
    s.blackFrames = 1; s.rearGlass = 1; s.cStyle = 0;
    s.roundF = 0.32f; s.expF = 2.3f; s.roundR = 0.26f; s.expR = 2.6f;
    s.xWellIn = 0.30f;
    return s;
}

static BodySpec HatchSpec(Color paint)   // 208 / Clio size
{
    BodySpec s = BaseCar(paint);
    s.hl = 2.03f;
    s.top = Cr(15, -2.03, 0.93, -1.97, 0.99, -1.90, 1.05, -1.72, 1.22, -1.52, 1.36, -1.30, 1.425, -0.40, 1.452, 0.12, 1.43,
               0.30, 1.38, 0.70, 1.15, 1.02, 0.975, 1.30, 0.915, 1.72, 0.84, 1.95, 0.76, 2.03, 0.64);
    s.belt = Cr(11, -2.03, 0.90, -1.88, 0.96, -1.55, 0.99, -1.0, 0.955, 0.0, 0.93, 0.90, 0.92, 1.02, 0.91, 1.30, 0.875,
                1.72, 0.80, 1.95, 0.72, 2.03, 0.62);
    s.bot = Cr(6, -2.03, 0.34, -1.85, 0.25, -1.4, 0.19, 1.3, 0.19, 1.80, 0.22, 2.03, 0.30);
    s.wMax = Cr(5, -2.03, 0.80, -1.8, 0.86, 0.0, 0.875, 1.7, 0.86, 2.03, 0.80);
    s.wSh = Cr(5, -2.03, 0.76, -1.6, 0.81, 0.5, 0.815, 1.6, 0.79, 2.03, 0.73);
    s.wRf = Cr(9, -2.03, 0.58, -1.88, 0.64, -1.55, 0.67, 0.12, 0.66, 0.30, 0.67, 0.70, 0.73, 0.98, 0.765, 1.03, 0.68, 2.03, 0.45);
    s.crown = Cr(6, -2.03, 0.0, -1.8, 0.015, 0.2, 0.02, 0.6, 0.045, 1.05, 0.05, 2.03, 0.03);
    s.endF = Cr(6, 0.20, 1.98, 0.32, 2.025, 0.55, 2.03, 0.66, 2.02, 0.76, 1.97, 0.90, 1.88);
    s.endR = Cr(6, 0.25, -1.98, 0.36, -2.02, 0.55, -2.03, 0.90, -2.03, 1.0, -1.99, 1.2, -1.90);
    s.axleZ[0] = 1.27f; s.axleZ[1] = -1.31f; s.axleY = 0.31f; s.archR = 0.38f;
    s.claddingTop = 0.27f; s.bumperBlackF = 0.30f; s.bumperBlackR = 0.32f; s.archCladding = 0;
    s.zWsBase = 1.02f; s.zWsTop = 0.30f; s.zB0 = -0.18f; s.zB1 = -0.07f; s.zRearGlass = -1.0f; s.zRoofEnd = -1.36f; s.zRwBottom = -1.88f;
    return s;
}

static BodySpec CitySpec(Color paint)    // Twingo / C1 size
{
    BodySpec s = BaseCar(paint);
    s.hl = 1.80f;
    s.top = Cr(14, -1.80, 0.95, -1.74, 1.02, -1.62, 1.22, -1.48, 1.40, -1.30, 1.46, -0.5, 1.475, 0.05, 1.45, 0.20, 1.40,
               0.55, 1.18, 0.88, 0.98, 1.15, 0.92, 1.55, 0.86, 1.74, 0.77, 1.80, 0.64);
    s.belt = Cr(10, -1.80, 0.91, -1.62, 0.98, -1.30, 0.99, -0.8, 0.955, 0.0, 0.94, 0.85, 0.93, 1.15, 0.88, 1.55, 0.82, 1.74, 0.73, 1.80, 0.62);
    s.bot = Cr(6, -1.80, 0.33, -1.62, 0.24, -1.2, 0.19, 1.1, 0.19, 1.6, 0.22, 1.80, 0.30);
    s.wMax = Cr(5, -1.80, 0.76, -1.6, 0.815, 0.0, 0.825, 1.5, 0.81, 1.80, 0.76);
    s.wSh = Cr(4, -1.80, 0.72, -1.4, 0.77, 1.4, 0.75, 1.80, 0.69);
    s.wRf = Cr(8, -1.80, 0.56, -1.62, 0.63, 0.05, 0.64, 0.20, 0.65, 0.55, 0.70, 0.85, 0.73, 0.90, 0.64, 1.80, 0.44);
    s.crown = Cr(5, -1.8, 0.0, -1.6, 0.015, 0.2, 0.02, 0.6, 0.045, 1.8, 0.03);
    s.endF = Cr(5, 0.20, 1.75, 0.32, 1.795, 0.60, 1.80, 0.76, 1.76, 0.90, 1.68);
    s.endR = Cr(5, 0.25, -1.76, 0.36, -1.795, 0.9, -1.80, 1.05, -1.77, 1.3, -1.66);
    s.axleZ[0] = 1.12f; s.axleZ[1] = -1.22f; s.axleY = 0.29f; s.archR = 0.36f;
    s.claddingTop = 0.26f; s.bumperBlackF = 0.30f; s.bumperBlackR = 0.32f;
    s.zWsBase = 0.88f; s.zWsTop = 0.20f; s.zB0 = -0.30f; s.zB1 = -0.20f; s.zRearGlass = -1.0f; s.zRoofEnd = -1.30f; s.zRwBottom = -1.62f;
    s.roundF = 0.28f; s.roundR = 0.22f;
    return s;
}

static BodySpec SuvSpec(Color paint)     // 3008 / Kadjar size, black cladding
{
    BodySpec s = BaseCar(paint);
    s.hl = 2.23f;
    s.top = Cr(14, -2.23, 1.06, -2.17, 1.12, -2.08, 1.30, -1.95, 1.52, -1.82, 1.60, -0.5, 1.635, 0.25, 1.61, 0.42, 1.56,
               0.82, 1.32, 1.17, 1.08, 1.45, 1.03, 1.90, 0.97, 2.15, 0.88, 2.23, 0.74);
    s.belt = Cr(10, -2.23, 1.02, -2.05, 1.10, -1.75, 1.13, -1.2, 1.09, 0.0, 1.07, 1.10, 1.06, 1.17, 1.05, 1.90, 0.95, 2.15, 0.86, 2.23, 0.72);
    s.bot = Cr(6, -2.23, 0.40, -2.05, 0.32, -1.6, 0.24, 1.5, 0.24, 2.0, 0.30, 2.23, 0.38);
    s.wMax = Cr(5, -2.23, 0.86, -2.0, 0.915, 0.0, 0.925, 1.9, 0.91, 2.23, 0.86);
    s.wSh = Cr(5, -2.23, 0.82, -1.9, 0.86, 0.0, 0.865, 1.6, 0.845, 2.23, 0.78);
    s.wRf = Cr(9, -2.23, 0.64, -2.05, 0.70, -1.80, 0.72, 0.25, 0.71, 0.42, 0.72, 0.82, 0.77, 1.12, 0.81, 1.17, 0.74, 2.23, 0.50);
    s.crown = Cr(5, -2.23, 0.0, -2.0, 0.015, 0.3, 0.02, 0.8, 0.05, 2.23, 0.03);
    s.endF = Cr(6, 0.22, 2.17, 0.35, 2.22, 0.60, 2.23, 0.74, 2.21, 0.88, 2.15, 1.0, 2.05);
    s.endR = Cr(6, 0.28, -2.18, 0.40, -2.22, 0.6, -2.23, 1.0, -2.23, 1.12, -2.20, 1.3, -2.10);
    s.axleZ[0] = 1.37f; s.axleZ[1] = -1.33f; s.axleY = 0.355f; s.archR = 0.43f;
    s.claddingTop = 0.42f; s.bumperBlackF = 0.40f; s.bumperBlackR = 0.44f; s.archCladding = 1;
    s.zWsBase = 1.15f; s.zWsTop = 0.40f; s.zB0 = -0.20f; s.zB1 = -0.08f; s.zRearGlass = -1.15f; s.zRoofEnd = -1.82f; s.zRwBottom = -2.15f;
    s.roundF = 0.34f; s.roundR = 0.26f;
    return s;
}

static BodySpec VanSpec(Color paint)     // Ducato / Master size panel van
{
    BodySpec s = BaseCar(paint);
    s.hl = 2.60f;
    s.top = Cr(11, -2.60, 2.36, -2.55, 2.41, -2.40, 2.43, 0.80, 2.43, 1.05, 2.33, 1.40, 1.75, 1.78, 1.28, 2.05, 1.16,
               2.35, 1.06, 2.52, 0.95, 2.60, 0.78);
    s.belt = Cr(9, -2.60, 1.20, -2.0, 1.20, 0.0, 1.20, 1.10, 1.18, 1.78, 1.18, 2.05, 1.10, 2.35, 1.00, 2.52, 0.90, 2.60, 0.76);
    s.bot = Cr(6, -2.60, 0.42, -2.40, 0.36, -1.9, 0.30, 1.6, 0.30, 2.3, 0.34, 2.60, 0.40);
    s.wMax = Cr(4, -2.60, 0.98, -2.45, 1.0, 1.9, 1.0, 2.60, 0.94);
    s.wSh = Cr(4, -2.60, 0.97, -2.4, 0.99, 1.8, 0.975, 2.60, 0.90);
    s.wRf = Cr(7, -2.60, 0.88, -2.45, 0.93, 0.80, 0.93, 1.05, 0.90, 1.70, 0.92, 1.80, 0.80, 2.60, 0.60);
    s.crown = Cr(4, -2.6, 0.0, -2.4, 0.03, 1.2, 0.03, 2.6, 0.03);
    s.endF = Cr(5, 0.25, 2.56, 0.40, 2.60, 0.80, 2.60, 0.95, 2.55, 1.2, 2.40);
    s.endR = Cr(4, 0.30, -2.57, 0.45, -2.60, 2.30, -2.60, 2.45, -2.57);
    s.roundF = 0.36f; s.expF = 2.3f; s.roundR = 0.10f; s.expR = 3.0f;
    s.axleZ[0] = 1.62f; s.axleZ[1] = -1.85f; s.axleY = 0.36f; s.archR = 0.44f;
    s.claddingTop = 0.36f; s.bumperBlackF = 0.52f; s.bumperBlackR = 0.50f; s.archCladding = 0;
    s.zWsBase = 1.75f; s.zWsTop = 1.0f; s.zB0 = 0.55f; s.zB1 = 0.66f; s.zRearGlass = 0.55f; s.zRoofEnd = -2.70f; s.zRwBottom = -2.70f;
    s.cStyle = 2; s.rearGlass = 0;
    return s;
}

static void PlateText(char *buf, int seed)
{
    unsigned int h = (unsigned int)seed * 2654435761u;
    char a = (char)('A' + (h >> 3) % 26), b = (char)('B' + (h >> 8) % 24);
    char c = (char)('A' + (h >> 13) % 26), d = (char)('C' + (h >> 18) % 23);
    snprintf(buf, 16, "%c%c-%03u-%c%c", a, b, (h >> 20) % 1000u, c, d);
}

static CarModel FinishCar(Builder *B, const BodySpec *s, Texture2D atlas, WheelSpec ws, float track, float len, float wid, float hgt)
{
    CarModel m; memset(&m, 0, sizeof(m));
    m.body = BuilderToModel(B, m.bodyMatKind, 16, atlas);
    m.wheel = BuildWheelModel(ws, m.wheelMatKind);
    m.wheelPos[0] = (Vector3){ track, s->axleY, s->axleZ[0] };
    m.wheelPos[1] = (Vector3){ -track, s->axleY, s->axleZ[0] };
    m.wheelPos[2] = (Vector3){ track, s->axleY, s->axleZ[1] };
    m.wheelPos[3] = (Vector3){ -track, s->axleY, s->axleZ[1] };
    m.wheelRadius = ws.radius;
    m.length = len; m.width = wid; m.height = hgt;
    return m;
}

static void AddSimpleMirrors(Builder *B, float x, float y, float z, Color shell)
{
    for (int side = -1; side <= 1; side += 2)
        AddMirror(B, side, (Vector3){ x + 0.12f, y, z }, (Vector3){ 0.10f, 0.065f, 0.07f }, (Vector3){ x - 0.02f, y - 0.04f, z + 0.03f }, shell, 0);
}

// semi-truck: cab-over tractor + 13.6 m box trailer
static CarModel BuildTruck(Color paint)
{
    Builder B; memset(&B, 0, sizeof(B));
    char plate[16]; PlateText(plate, paint.r * 7 + paint.g * 3 + paint.b);
    Texture2D atlas = MakeDecalAtlas(plate, 0);
    const Rectangle UV_PLATE = PlateUV(atlas);
    Color trim = { 14, 15, 17, 255 }, steel = { 70, 72, 76, 255 }, white = { 236, 237, 238, 255 };
    // cab: loft spec, box-like
    BodySpec s = BaseCar(paint);
    s.hl = 1.18f;
    s.top = Cr(5, -1.18, 3.55, -1.10, 3.62, 0.9, 3.66, 1.10, 3.58, 1.18, 3.40);
    s.belt = Cr(4, -1.18, 2.02, 0.0, 2.02, 1.10, 2.02, 1.18, 1.98);
    s.bot = Cr(3, -1.18, 0.95, 0.0, 0.95, 1.18, 0.62);
    s.wMax = Cr(3, -1.18, 1.24, 0.0, 1.25, 1.18, 1.22);
    s.wSh = Cr(3, -1.18, 1.23, 0.0, 1.24, 1.18, 1.20);
    s.wRf = Cr(3, -1.18, 1.10, 0.0, 1.14, 1.18, 1.08);
    s.crown = Cr(2, -1.18, 0.04, 1.18, 0.04);
    s.endF = Cr(4, 0.6, 1.14, 0.8, 1.18, 3.2, 1.18, 3.5, 1.10);
    s.endR = Cr(2, 0.6, -1.18, 3.6, -1.18);
    s.roundF = 0.22f; s.expF = 3.5f; s.roundR = 0.12f; s.expR = 3.5f;
    s.axleZ[0] = 0.15f; s.axleZ[1] = -9.0f; s.axleY = 0.50f; s.archR = 0.58f; s.xWellIn = 0.35f;
    s.claddingTop = 0; s.bumperBlackF = 0.95f; s.bumperBlackR = 0; s.archCladding = 0;
    s.zWsBase = 9.0f; s.zWsTop = 9.0f; s.zB0 = -9.0f; s.zB1 = -9.0f; s.zRearGlass = -0.25f; s.zRoofEnd = -9.0f; s.zRwBottom = -9.0f;
    s.cStyle = 3; s.rearGlass = 0; s.blackFrames = 1;
    // the cab sits at the front of the 16 m vehicle: build it in its own frame then shift
    float cabZ = 8.0f - 1.18f;
    B.useXf = 1; B.xf = MatrixTranslate(0, 0, cabZ);
    BuildBodyLoft(&B, &s, 1);
    // windscreen and side windows on the flat cab faces
    AddEndBand(&B, &s, 1, 2.08f, 3.12f, -1.08f, 1.08f, 0.006f, 4, 16, Solid(CARMAT_GLASS, s.glass));
    {
        Vector2 w[4] = { { 1.00f, 2.10f }, { 1.00f, 3.05f }, { 0.20f, 3.05f }, { 0.20f, 2.10f } };
        AddFlankPoly(&B, &s, w, 4, 0.006f, Solid(CARMAT_GLASS, s.glass), 1);
    }
    PatchStyle grille = Solid(CARMAT_PLASTIC, trim); grille.pat = PatGrille;
    AddEndBand(&B, &s, 1, 1.15f, 1.85f, -0.85f, 0.85f, 0.006f, 6, 10, grille);
    PatchStyle hl = Solid(CARMAT_LIGHT_FRONT, WHITE); hl.pat = PatHeadlight;
    AddEndBand(&B, &s, 1, 0.80f, 0.95f, 0.70f, 1.10f, 0.012f, 2, 4, hl);
    AddEndBand(&B, &s, 1, 0.80f, 0.95f, -1.10f, -0.70f, 0.012f, 2, 4, hl);
    PatchStyle plate2 = Solid(CARMAT_DECAL, WHITE); plate2.uv = UV_PLATE;
    AddEndBand(&B, &s, 1, 0.66f, 0.77f, -0.26f, 0.26f, 0.012f, 2, 8, plate2);
    AddBox(&B, CARMAT_PLASTIC, (Vector3){ 0, 0.68f, 1.12f }, (Vector3){ 1.20f, 0.10f, 0.08f }, trim);  // bumper
    for (int side = -1; side <= 1; side += 2) {
        AddBox(&B, CARMAT_PAINT, (Vector3){ side * 1.36f, 2.55f, 1.05f }, (Vector3){ 0.03f, 0.28f, 0.06f }, trim); // big mirrors
        AddBox(&B, CARMAT_PLASTIC, (Vector3){ side * 1.30f, 2.75f, 1.08f }, (Vector3){ 0.08f, 0.02f, 0.02f }, trim);
        AddBox(&B, CARMAT_PLASTIC, (Vector3){ side * 1.10f, 0.55f, -0.70f }, (Vector3){ 0.12f, 0.12f, 0.35f }, steel);  // steps/tank
    }
    B.useXf = 0;
    // chassis rails & fifth wheel
    AddBox(&B, CARMAT_PLASTIC, (Vector3){ 0, 0.85f, 3.2f }, (Vector3){ 0.45f, 0.14f, 3.6f }, trim);
    // trailer box: z from -8.0 to 5.6, floor 1.20, roof 4.0
    {
        float z0 = -8.0f, z1 = 5.55f, y0 = 1.20f, y1 = 4.0f, hw = 1.275f;
        AddBox(&B, CARMAT_PAINT, (Vector3){ 0, (y0 + y1) / 2, (z0 + z1) / 2 }, (Vector3){ hw, (y1 - y0) / 2, (z1 - z0) / 2 }, white);
        // coloured stripe on both flanks (operator livery)
        for (int side = -1; side <= 1; side += 2) {
            Vector3 a = { side * (hw + 0.004f), 1.55f, z0 + 0.3f }, b = { side * (hw + 0.004f), 1.55f, z1 - 0.3f };
            Vector3 c = { side * (hw + 0.004f), 2.05f, z1 - 0.3f }, d = { side * (hw + 0.004f), 2.05f, z0 + 0.3f };
            QuadFacing(&B, CARMAT_PAINT, a, b, c, d, (Vector3){ (float)side, 0, 0 }, paint);
            // side underrun guard
            AddBox(&B, CARMAT_PLASTIC, (Vector3){ side * 1.18f, 0.78f, -1.0f }, (Vector3){ 0.03f, 0.10f, 2.6f }, steel);
        }
        // rear doors frame, lights, bumper
        AddBox(&B, CARMAT_PLASTIC, (Vector3){ 0, 0.62f, z0 + 0.12f }, (Vector3){ 1.20f, 0.08f, 0.06f }, steel);
        for (int side = -1; side <= 1; side += 2) {
            AddBox(&B, CARMAT_LIGHT_REAR, (Vector3){ side * 0.95f, 1.0f, z0 - 0.015f }, (Vector3){ 0.22f, 0.07f, 0.02f }, (Color){ 200, 20, 25, 255 });
            Vector3 p0 = { side * 0.02f, y0 + 0.05f, z0 - 0.003f }, p1 = { side * 0.02f, y1 - 0.05f, z0 - 0.003f };
            Vector3 p2 = { side * 0.035f, y1 - 0.05f, z0 - 0.003f }, p3 = { side * 0.035f, y0 + 0.05f, z0 - 0.003f };
            QuadFacing(&B, CARMAT_PLASTIC, p0, p1, p2, p3, (Vector3){ 0, 0, -1 }, steel);
        }
        AddBox(&B, CARMAT_PLASTIC, (Vector3){ 0, 1.10f, -2.2f }, (Vector3){ 1.10f, 0.12f, 5.6f }, trim);   // trailer chassis
        // rear number plate (seen from behind: +X is on the viewer's right... no: on the left)
        Vector3 a = { 0.26f, 0.80f, z0 - 0.02f }, b = { -0.26f, 0.80f, z0 - 0.02f }, c = { -0.26f, 0.91f, z0 - 0.02f }, d = { 0.26f, 0.91f, z0 - 0.02f };
        Vector3 nb = { 0, 0, -1 };
        float vh = UV_PLATE.height;
        TriEx(&B, CARMAT_DECAL, a, c, b, nb, nb, nb, WHITE, WHITE, WHITE, (Vector2){ 0, vh }, (Vector2){ 1, 0 }, (Vector2){ 1, vh });
        TriEx(&B, CARMAT_DECAL, a, d, c, nb, nb, nb, WHITE, WHITE, WHITE, (Vector2){ 0, vh }, (Vector2){ 0, 0 }, (Vector2){ 1, 0 });
    }
    // baked static wheels: second drive axle? no — tractor drive axle is wheelPos RL/RR; trailer tri-axle baked
    WheelSpec ws = { 0.50f, 0.32f, 0.29f, 2, 1 };
    float trailerAxles[3] = { -4.0f, -5.35f, -6.70f };
    for (int a = 0; a < 3; a++)
        for (int side = -1; side <= 1; side += 2) {
            Matrix m = side > 0 ? MatrixIdentity() : MatrixRotateY(PI);
            m = MatrixMultiply(m, MatrixTranslate(side * 0.98f, 0.50f, trailerAxles[a]));
            B.useXf = 1; B.xf = m;
            AddWheel(&B, ws);
            B.useXf = 0;
        }
    // dark arches above the trailer wheels
    for (int side = -1; side <= 1; side += 2)
        AddBox(&B, CARMAT_PLASTIC, (Vector3){ side * 0.98f, 1.12f, -5.35f }, (Vector3){ 0.18f, 0.06f, 2.2f }, trim);
    CarModel m; memset(&m, 0, sizeof(m));
    m.body = BuilderToModel(&B, m.bodyMatKind, 16, atlas);
    BuilderFree(&B);
    m.wheel = BuildWheelModel(ws, m.wheelMatKind);
    m.wheelPos[0] = (Vector3){ 1.02f, 0.50f, cabZ + 0.15f };
    m.wheelPos[1] = (Vector3){ -1.02f, 0.50f, cabZ + 0.15f };
    m.wheelPos[2] = (Vector3){ 0.98f, 0.50f, cabZ - 3.6f };
    m.wheelPos[3] = (Vector3){ -0.98f, 0.50f, cabZ - 3.6f };
    m.wheelRadius = 0.50f;
    m.length = 16.0f; m.width = 2.55f; m.height = 4.0f;
    return m;
}

CarModel BuildTrafficCarModel(int kind, Color paint)
{
    if (kind == 3) return BuildTruck(paint);
    Builder B; memset(&B, 0, sizeof(B));
    BodySpec s;
    WheelSpec ws;
    float track, len, wid, hgt;
    char plate[16];
    PlateText(plate, kind * 1000 + paint.r * 31 + paint.g * 7 + paint.b);
    Texture2D atlas = MakeDecalAtlas(plate, 0);
    switch (kind) {
    default:
    case 0:
        s = HatchSpec(paint); ws = (WheelSpec){ 0.31f, 0.195f, 0.205f, 1, 1 }; track = 0.76f; len = 4.06f; wid = 1.75f; hgt = 1.45f;
        BuildBodyLoft(&B, &s, 1);
        DecorateGeneric(&B, &s, atlas, kind, paint, 0.70f, 0.78f, 0.40f, 0.84f, 0.86f, 0.96f, 0.36f, 0.95f, 0.42f, 0.62f);
        AddSimpleMirrors(&B, 0.86f, 1.0f, 0.82f, paint);
        break;
    case 1:
        s = SuvSpec(paint); ws = (WheelSpec){ 0.355f, 0.225f, 0.229f, 1, 1 }; track = 0.80f; len = 4.46f; wid = 1.85f; hgt = 1.66f;
        BuildBodyLoft(&B, &s, 1);
        DecorateGeneric(&B, &s, atlas, kind, paint, 0.80f, 0.88f, 0.42f, 0.90f, 0.98f, 1.08f, 0.40f, 1.02f, 0.50f, 0.70f);
        AddSimpleMirrors(&B, 0.90f, 1.12f, 0.88f, s.trim);
        for (int side = -1; side <= 1; side += 2) {   // roof rails
            Vector3 pts[2] = { { side * 0.64f, 1.67f, -1.60f }, { side * 0.64f, 1.66f, 0.25f } };
            AddTube(&B, CARMAT_CHROME, pts, 2, 0.016f, 6, (Color){ 170, 172, 176, 255 }, 1);
        }
        break;
    case 2:
        s = VanSpec(paint); ws = (WheelSpec){ 0.36f, 0.215f, 0.20f, 2, 1 }; track = 0.86f; len = 5.20f; wid = 2.0f; hgt = 2.45f;
        BuildBodyLoft(&B, &s, 1);
        DecorateGeneric(&B, &s, atlas, kind, paint, 0.92f, 1.04f, 0.45f, 0.95f, 0.50f, 0.95f, 0.85f, 1.00f, 0.55f, 0.62f);
        AddSimpleMirrors(&B, 1.0f, 1.42f, 1.62f, s.trim);
        // rear door split line
        {
            Vector3 a = { 0.01f, 0.45f, -2.605f }, b = { -0.01f, 0.45f, -2.605f }, c = { -0.01f, 2.35f, -2.605f }, d = { 0.01f, 2.35f, -2.605f };
            QuadFacing(&B, CARMAT_PLASTIC, a, b, c, d, (Vector3){ 0, 0, -1 }, (Color){ 30, 30, 32, 255 });
        }
        break;
    case 4:
        s = CitySpec(paint); ws = (WheelSpec){ 0.29f, 0.175f, 0.19f, 1, 1 }; track = 0.71f; len = 3.60f; wid = 1.65f; hgt = 1.48f;
        BuildBodyLoft(&B, &s, 1);
        DecorateGeneric(&B, &s, atlas, kind, paint, 0.72f, 0.80f, 0.38f, 0.78f, 0.84f, 0.96f, 0.32f, 0.88f, 0.42f, 0.66f);
        AddSimpleMirrors(&B, 0.80f, 1.0f, 0.70f, paint);
        break;
    }
    // simple door gaps
    {
        Color gap = { 10, 12, 14, 255 };
        Vector2 l1[] = { { s.zB1 + 0.01f, s.claddingTop + 0.05f }, { s.zB1, Cv(&s.belt, s.zB1) } };
        int c1 = 2;
        const Vector2 *lines[] = { l1 };
        if (kind != 2) AddDoorLines(&B, &s, lines, &c1, 1, gap);
        AddHandle(&B, &s, s.zB1 + 0.12f, s.zB1 + 0.30f, Cv(&s.belt, s.zB1) - 0.07f, s.paint);
    }
    CarModel m = FinishCar(&B, &s, atlas, ws, track, len, wid, hgt);
    BuilderFree(&B);
    return m;
}

void UnloadCarModel(CarModel *m)
{
    if (!m) return;
    for (int i = 0; i < m->body.materialCount; i++) {
        Texture2D t = m->body.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        if (m->bodyMatKind[i] == CARMAT_DECAL && t.id > 0) UnloadTexture(t);
        m->body.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture.id = 0;
    }
    UnloadModel(m->body);
    UnloadModel(m->wheel);
    memset(m, 0, sizeof(*m));
}

CarMatParams GetCarMatParams(int kind)
{
    switch (kind) {
    case CARMAT_PAINT:       return (CarMatParams){ 0.70f, 70.0f, 0.55f, 0.05f, 0.0f };
    case CARMAT_PLASTIC:     return (CarMatParams){ 0.10f, 12.0f, 0.05f, 0.02f, 0.0f };
    case CARMAT_GLASS:       return (CarMatParams){ 1.00f, 140.0f, 0.75f, 0.12f, 0.0f };
    case CARMAT_CHROME:      return (CarMatParams){ 1.00f, 90.0f, 0.90f, 0.65f, 0.0f };
    case CARMAT_RUBBER:      return (CarMatParams){ 0.06f, 8.0f, 0.0f, 0.0f, 0.0f };
    case CARMAT_RIM:         return (CarMatParams){ 0.60f, 40.0f, 0.35f, 0.15f, 0.0f };
    case CARMAT_LIGHT_FRONT: return (CarMatParams){ 0.90f, 90.0f, 0.30f, 0.10f, 0.55f };
    case CARMAT_LIGHT_REAR:  return (CarMatParams){ 0.80f, 80.0f, 0.25f, 0.08f, 0.40f };
    case CARMAT_DECAL:       return (CarMatParams){ 0.25f, 30.0f, 0.05f, 0.02f, 0.05f };
    default:                 return (CarMatParams){ 0.2f, 20.0f, 0.0f, 0.0f, 0.0f };
    }
}

const char *CarMatKindName(int kind)
{
    static const char *names[] = { "PAINT", "PLASTIC", "GLASS", "CHROME", "RUBBER", "RIM", "LIGHT_FRONT", "LIGHT_REAR", "DECAL" };
    return (kind >= 0 && kind < CARMAT_COUNT) ? names[kind] : "?";
}

int CarModelTriangleCount(const CarModel *m)
{
    int t = 0;
    for (int i = 0; i < m->body.meshCount; i++) t += m->body.meshes[i].triangleCount;
    for (int i = 0; i < m->wheel.meshCount; i++) t += m->wheel.meshes[i].triangleCount;
    return t;
}
