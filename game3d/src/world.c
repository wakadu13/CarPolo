// Loads world.bin (format in DESIGN.md) and answers geometric queries.
#include "world.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

World W;

typedef struct { const unsigned char *p, *end; bool ok; } Reader;

static void rd(Reader *r, void *dst, size_t n)
{
    if (!r->ok || (size_t)(r->end - r->p) < n) { r->ok = false; memset(dst, 0, n); return; }
    memcpy(dst, r->p, n);
    r->p += n;
}
static uint32_t ru32(Reader *r) { uint32_t v; rd(r, &v, 4); return v; }
static uint16_t ru16(Reader *r) { uint16_t v; rd(r, &v, 2); return v; }
static uint8_t  ru8(Reader *r)  { uint8_t v;  rd(r, &v, 1); return v; }
static float    rf(Reader *r)   { float v;    rd(r, &v, 4); return v; }

static void readGrid(Reader *r, Grid *g, bool heights)
{
    g->x0 = rf(r); g->z0 = rf(r); g->cell = rf(r);
    g->nx = (int)ru32(r); g->nz = (int)ru32(r);
    size_t n = (size_t)g->nx * g->nz;
    if (!r->ok || n == 0 || n > 50000000) { r->ok = false; return; }
    if (heights) { g->h = malloc(n * 2); rd(r, g->h, n * 2); }
    else         { g->c = malloc(n);     rd(r, g->c, n); }
}

const char *WorldStr(int id)
{
    if (id < 0 || id >= W.nStrings || id == 0xFFFF) return "";
    return W.strings[id];
}

static void deriveRoute(void)
{
    float s = 0;
    for (int i = 0; i < W.nRoute; i++) {
        RoutePt *p = &W.route[i];
        if (i > 0) {
            RoutePt *q = &W.route[i - 1];
            s += sqrtf((p->x - q->x) * (p->x - q->x) + (p->z - q->z) * (p->z - q->z));
        }
        p->s = s;
    }
    W.length = s;
    for (int i = 0; i < W.nRoute; i++) {
        int a = i > 0 ? i - 1 : i, b = i < W.nRoute - 1 ? i + 1 : i;
        float dx = W.route[b].x - W.route[a].x, dz = W.route[b].z - W.route[a].z;
        float l = sqrtf(dx * dx + dz * dz);
        if (l < 1e-4f) { dx = 1; dz = 0; l = 1; }
        W.route[i].hx = dx / l; W.route[i].hz = dz / l;
    }
    // curvature over a +-3 point window; sign: >0 when turning right (clockwise seen from above)
    for (int i = 0; i < W.nRoute; i++) {
        int a = i > 3 ? i - 3 : 0, b = i < W.nRoute - 4 ? i + 3 : W.nRoute - 1;
        float h1 = atan2f(W.route[a].hz, W.route[a].hx), h2 = atan2f(W.route[b].hz, W.route[b].hx);
        float dh = h2 - h1;
        while (dh > PI) dh -= 2 * PI;
        while (dh < -PI) dh += 2 * PI;
        float ds = W.route[b].s - W.route[a].s;
        // x east, z south: atan2(z, x) increases clockwise seen from above (+Y), i.e. a right turn
        W.route[i].curv = ds > 0.01f ? dh / ds : 0;
    }
}

bool WorldLoad(const char *path)
{
    memset(&W, 0, sizeof(W));
    int size = 0;
    unsigned char *data = LoadFileData(path, &size);
    if (!data) { TraceLog(LOG_ERROR, "WORLD: cannot read %s", path); return false; }
    Reader r = { data, data + size, true };
    char magic[4];
    rd(&r, magic, 4);
    if (memcmp(magic, "CPW1", 4) != 0) { TraceLog(LOG_ERROR, "WORLD: bad magic"); UnloadFileData(data); return false; }
    ru32(&r);
    rd(&r, &W.originLat, 8); rd(&r, &W.originLon, 8);
    W.minX = rf(&r); W.minZ = rf(&r); W.maxX = rf(&r); W.maxZ = rf(&r);

    while (r.ok && r.p + 8 <= r.end) {
        char tag[5] = { 0 };
        rd(&r, tag, 4);
        uint32_t len = ru32(&r);
        if ((size_t)(r.end - r.p) < len) break;
        Reader s = { r.p, r.p + len, true };
        r.p += len;
        if (!memcmp(tag, "STRS", 4)) {
            W.nStrings = (int)ru32(&s);
            W.strings = calloc(W.nStrings + 1, sizeof(char *));
            for (int i = 0; i < W.nStrings && s.ok; i++) {
                int l = ru16(&s);
                W.strings[i] = calloc(l + 1, 1);
                rd(&s, W.strings[i], l);
            }
        } else if (!memcmp(tag, "ROUT", 4)) {
            W.nRoute = (int)ru32(&s);
            W.route = calloc(W.nRoute, sizeof(RoutePt));
            for (int i = 0; i < W.nRoute && s.ok; i++) {
                RoutePt *p = &W.route[i];
                p->x = rf(&s); p->y = rf(&s); p->z = rf(&s); p->width = rf(&s);
                p->lanes = ru8(&s); p->lanesForward = ru8(&s); p->roadClass = ru8(&s); p->flags = ru8(&s);
                p->speedLimit = ru16(&s); p->nameId = ru16(&s); p->placeId = ru16(&s); ru16(&s);
                if (p->width < 3) p->width = 3;
                if (p->lanes < 1) p->lanes = 1;
                if (p->lanesForward < 1) p->lanesForward = 1;
            }
        } else if (!memcmp(tag, "DEM ", 4)) readGrid(&s, &W.dem, true);
        else if (!memcmp(tag, "LCOV", 4)) readGrid(&s, &W.lcov, false);
        else if (!memcmp(tag, "FAR ", 4)) readGrid(&s, &W.far, true);
        else if (!memcmp(tag, "FARC", 4)) readGrid(&s, &W.farc, false);
        else if (!memcmp(tag, "BLDG", 4)) {
            W.nBuildings = (int)ru32(&s);
            W.buildings = calloc(W.nBuildings, sizeof(Building));
            for (int i = 0; i < W.nBuildings && s.ok; i++) {
                Building *b = &W.buildings[i];
                b->nv = ru16(&s); b->kind = ru8(&s); b->roofShape = ru8(&s);
                b->height = rf(&s); b->minHeight = rf(&s);
                b->xz = malloc(sizeof(float) * 2 * (b->nv > 0 ? b->nv : 1));
                rd(&s, b->xz, sizeof(float) * 2 * b->nv);
                float cx = 0, cz = 0;
                for (int k = 0; k < b->nv; k++) { cx += b->xz[2 * k]; cz += b->xz[2 * k + 1]; }
                if (b->nv) { cx /= b->nv; cz /= b->nv; }
                float rr = 0;
                for (int k = 0; k < b->nv; k++) {
                    float dx = b->xz[2 * k] - cx, dz = b->xz[2 * k + 1] - cz;
                    if (dx * dx + dz * dz > rr) rr = dx * dx + dz * dz;
                }
                b->cx = cx; b->cz = cz; b->radius = sqrtf(rr);
                if (b->height < 2.5f) b->height = 6.0f;
            }
        } else if (!memcmp(tag, "ROAD", 4)) {
            W.nRoads = (int)ru32(&s);
            W.roads = calloc(W.nRoads, sizeof(SideRoad));
            for (int i = 0; i < W.nRoads && s.ok; i++) {
                SideRoad *d = &W.roads[i];
                d->np = ru16(&s); d->roadClass = ru8(&s); d->lanes = ru8(&s);
                d->width = rf(&s); d->flags = ru8(&s);
                uint8_t pad[3]; rd(&s, pad, 3);
                d->xz = malloc(sizeof(float) * 2 * (d->np > 0 ? d->np : 1));
                rd(&s, d->xz, sizeof(float) * 2 * d->np);
                if (d->width < 2.5f) d->width = 4.0f;
            }
        } else if (!memcmp(tag, "TREE", 4)) {
            W.nTrees = (int)ru32(&s);
            W.trees = calloc(W.nTrees + 1, sizeof(MappedTree));
            for (int i = 0; i < W.nTrees && s.ok; i++) {
                W.trees[i].x = rf(&s); W.trees[i].z = rf(&s); W.trees[i].type = ru8(&s);
                uint8_t pad[3]; rd(&s, pad, 3);
            }
        } else if (!memcmp(tag, "PLAC", 4)) {
            W.nPlaces = (int)ru32(&s);
            W.places = calloc(W.nPlaces + 1, sizeof(Place));
            for (int i = 0; i < W.nPlaces && s.ok; i++) {
                W.places[i].x = rf(&s); W.places[i].z = rf(&s);
                W.places[i].nameId = ru16(&s); W.places[i].kind = ru8(&s); ru8(&s);
            }
        } else if (!memcmp(tag, "ZONE", 4)) {
            W.nZones = (int)ru32(&s);
            W.zones = calloc(W.nZones + 1, sizeof(Zone));
            for (int i = 0; i < W.nZones && s.ok; i++) {
                W.zones[i].s = rf(&s); W.zones[i].nameId = ru16(&s); W.zones[i].subId = ru16(&s);
            }
        }
        if (!s.ok) TraceLog(LOG_WARNING, "WORLD: section %s truncated", tag);
    }
    UnloadFileData(data);
    if (W.nRoute < 2 || !W.dem.h) { TraceLog(LOG_ERROR, "WORLD: missing route or DEM"); return false; }
    deriveRoute();
    TraceLog(LOG_INFO, "WORLD: route %d pts %.0f m, DEM %dx%d, %d buildings, %d roads, %d places, %d zones",
             W.nRoute, W.length, W.dem.nx, W.dem.nz, W.nBuildings, W.nRoads, W.nPlaces, W.nZones);
    return true;
}

void WorldUnload(void)
{
    for (int i = 0; i < W.nStrings; i++) free(W.strings[i]);
    free(W.strings); free(W.route);
    free(W.dem.h); free(W.lcov.c); free(W.far.h); free(W.farc.c);
    for (int i = 0; i < W.nBuildings; i++) free(W.buildings[i].xz);
    free(W.buildings);
    for (int i = 0; i < W.nRoads; i++) free(W.roads[i].xz);
    free(W.roads); free(W.trees); free(W.places); free(W.zones);
    memset(&W, 0, sizeof(W));
}

float GridHeight(const Grid *g, float x, float z)
{
    if (!g->h) return 0;
    float fx = (x - g->x0) / g->cell, fz = (z - g->z0) / g->cell;
    if (fx < 0) fx = 0;
    if (fz < 0) fz = 0;
    if (fx > g->nx - 1.001f) fx = g->nx - 1.001f;
    if (fz > g->nz - 1.001f) fz = g->nz - 1.001f;
    int i = (int)fx, j = (int)fz;
    float tx = fx - i, tz = fz - j;
    const int16_t *h = g->h;
    float a = h[j * g->nx + i], b = h[j * g->nx + i + 1];
    float c = h[(j + 1) * g->nx + i], d = h[(j + 1) * g->nx + i + 1];
    return ((a + (b - a) * tx) * (1 - tz) + (c + (d - c) * tx) * tz) * 0.1f;
}

int GridClass(const Grid *g, float x, float z)
{
    if (!g->c) return 0;
    int i = (int)floorf((x - g->x0) / g->cell + 0.5f), j = (int)floorf((z - g->z0) / g->cell + 0.5f);
    if (i < 0 || j < 0 || i >= g->nx || j >= g->nz) return 0;
    return g->c[j * g->nx + i];
}

float DemHeight(float x, float z) { return GridHeight(&W.dem, x, z); }

int RouteIndexAtS(float s)
{
    if (s <= 0) return 0;
    if (s >= W.length) return W.nRoute - 2;
    int lo = 0, hi = W.nRoute - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (W.route[mid].s <= s) lo = mid; else hi = mid;
    }
    return lo;
}

void RouteSample(float s, Vector3 *pos, float *hx, float *hz, float *width, const RoutePt **attr)
{
    int i = RouteIndexAtS(s);
    const RoutePt *a = &W.route[i], *b = &W.route[i + 1];
    float seg = b->s - a->s;
    float t = seg > 1e-4f ? (s - a->s) / seg : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (pos) *pos = (Vector3){ a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t, a->z + (b->z - a->z) * t };
    if (hx && hz) {
        float x = a->hx + (b->hx - a->hx) * t, z = a->hz + (b->hz - a->hz) * t;
        float l = sqrtf(x * x + z * z);
        if (l < 1e-5f) l = 1;
        *hx = x / l; *hz = z / l;
    }
    if (width) *width = a->width + (b->width - a->width) * t;
    if (attr) *attr = t < 0.5f ? a : b;
}

float RouteProject(float x, float z, int hint, int window, float *d, int *seg)
{
    int i0 = 0, i1 = W.nRoute - 2;
    if (hint >= 0) {
        i0 = hint - window; i1 = hint + window;
        if (i0 < 0) i0 = 0;
        if (i1 > W.nRoute - 2) i1 = W.nRoute - 2;
    }
    float best = 1e30f, bestS = 0, bestD = 0;
    int bestI = i0;
    for (int i = i0; i <= i1; i++) {
        const RoutePt *a = &W.route[i], *b = &W.route[i + 1];
        float ex = b->x - a->x, ez = b->z - a->z;
        float l2 = ex * ex + ez * ez;
        float t = l2 > 1e-8f ? ((x - a->x) * ex + (z - a->z) * ez) / l2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float px = a->x + ex * t, pz = a->z + ez * t;
        float dd = (x - px) * (x - px) + (z - pz) * (z - pz);
        if (dd < best) {
            best = dd; bestI = i;
            bestS = a->s + (b->s - a->s) * t;
            float l = sqrtf(l2);
            // right of travel = (-hz, hx) in (x,z)
            bestD = l > 1e-5f ? ((x - px) * (-ez / l) + (z - pz) * (ex / l)) : 0;
            if (fabsf(bestD) < 1e-6f) bestD = 0;
            bestD = (bestD >= 0 ? 1 : -1) * sqrtf(dd);
        }
    }
    if (d) *d = bestD;
    if (seg) *seg = bestI;
    return bestS;
}
