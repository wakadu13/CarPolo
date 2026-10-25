// AI traffic: vehicles follow the route at the real speed limits, keep their distance,
// change lanes on the motorway, and oncoming cars brake/honk when the player is in their lane.
#include "traffic.h"
#include "world.h"
#include "render.h"
#include "raymath.h"
#include <stdlib.h>
#include <math.h>

#define MAX_TRAFFIC 44
#define NMODELS 11

typedef struct {
    bool active;
    int model, dir, lane;
    float s, d, v, vWanted, spin, laneCd, honkT;
    bool braking, passed, nearChecked;
    Matrix m;
    Vector3 pos;
    float yaw;
} TCar;

static TCar cars[MAX_TRAFFIC];
static CarModel models[NMODELS];
static int modelKind[NMODELS];
static int nModels;
static int *lastNotTwoWay;
static float cursorSame, cursorOnc;
static int honkEvents, nearMiss;

static float frand(float a, float b) { return a + (b - a) * (GetRandomValue(0, 10000) / 10000.0f); }

static bool isTwoWayPt(const RoutePt *p) { return !(p->flags & (RF_ONEWAY | RF_ROUNDABOUT)) && p->lanesForward < p->lanes; }

void TrafficInit(void)
{
    struct { int kind; Color c; } defs[NMODELS] = {
        { 0, { 176, 28, 34, 255 } }, { 0, { 232, 232, 228, 255 } }, { 0, { 58, 60, 64, 255 } }, { 0, { 168, 170, 174, 255 } },
        { 1, { 40, 64, 120, 255 } }, { 1, { 214, 214, 210, 255 } }, { 2, { 240, 240, 236, 255 } }, { 3, { 30, 80, 150, 255 } },
        { 4, { 220, 170, 40, 255 } }, { 4, { 120, 30, 40, 255 } }, { 0, { 24, 24, 28, 255 } }
    };
    nModels = NMODELS;
    for (int i = 0; i < NMODELS; i++) { models[i] = BuildTrafficCarModel(defs[i].kind, defs[i].c); modelKind[i] = defs[i].kind; }
    lastNotTwoWay = malloc(sizeof(int) * W.nRoute);
    int last = -1;
    for (int i = 0; i < W.nRoute; i++) { if (!isTwoWayPt(&W.route[i])) last = i; lastNotTwoWay[i] = last; }
}

void TrafficShutdown(void)
{
    for (int i = 0; i < nModels; i++) UnloadCarModel(&models[i]);
    nModels = 0;
    free(lastNotTwoWay);
    lastNotTwoWay = NULL;
}

static float limitAt(int idx)
{
    const RoutePt *p = &W.route[idx];
    float lim = p->speedLimit;
    if (lim <= 0) lim = p->roadClass >= RC_MOTORWAY ? 130 : (p->flags & RF_BUILTUP) ? 50 : 80;
    if (p->flags & RF_ROUNDABOUT) lim = fminf(lim, 30);
    // slow down in tight bends
    float c = fabsf(p->curv);
    if (c > 1e-4f) lim = fminf(lim, sqrtf(3.2f / c) * 3.6f);
    return lim / 3.6f;
}

static float carLen(const TCar *c) { return models[c->model].length; }

static int pickModel(bool motorway)
{
    int r = GetRandomValue(0, 99);
    if (motorway && r < 22) return 7;          // semi-truck
    if (r < 30) return GetRandomValue(0, 3);
    if (r < 48) return 4 + GetRandomValue(0, 1);
    if (r < 58) return 6;
    if (r < 70) return 8 + GetRandomValue(0, 1);
    if (r < 76 && !motorway) return 7;
    return GetRandomValue(0, 3) == 0 ? 10 : GetRandomValue(0, 3);
}

static bool laneFree(int dir, float s, float d, float range, int self)
{
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        if (i == self || !cars[i].active || cars[i].dir != dir) continue;
        if (fabsf(cars[i].d - d) < 2.2f && fabsf(cars[i].s - s) < range) return false;
    }
    return true;
}

static void spawn(float s, int dir, float playerS)
{
    int idx = RouteIndexAtS(s);
    const RoutePt *p = &W.route[idx];
    if (p->flags & RF_ROUNDABOUT) return;
    int lanes = LaneCount(idx, dir);
    if (lanes <= 0) return;
    if (dir < 0 && lastNotTwoWay[idx] >= RouteIndexAtS(playerS)) return;
    int slot = -1;
    for (int i = 0; i < MAX_TRAFFIC; i++) if (!cars[i].active) { slot = i; break; }
    if (slot < 0) return;
    bool mw = p->roadClass >= RC_MOTORWAY;
    int lane = lanes == 1 ? 0 : GetRandomValue(0, lanes - 1);
    int model = pickModel(mw);
    if (modelKind[model] == 3 && lanes > 1) lane = 0;   // trucks keep right
    float d = LaneCenterD(idx, dir, lane);
    if (!laneFree(dir, s, d, 60, -1)) return;
    TCar *c = &cars[slot];
    *c = (TCar){ 0 };
    c->active = true; c->dir = dir; c->lane = lane; c->model = model; c->s = s; c->d = d;
    float f = frand(0.84f, 1.03f);
    if (modelKind[model] == 3) f = fminf(f, 0.9f);
    c->vWanted = f;
    c->v = limitAt(idx) * f;
    if (modelKind[model] == 3) c->v = fminf(c->v, mw ? 25.0f : 22.0f);
    c->laneCd = frand(2, 6);
    c->spin = frand(0, 6);
}

void TrafficReset(float sPlayer)
{
    for (int i = 0; i < MAX_TRAFFIC; i++) cars[i].active = false;
    cursorSame = sPlayer + 120;
    cursorOnc = sPlayer + 200;
    honkEvents = 0;
    nearMiss = 0;
}

void TrafficClearNear(float s, float behind, float ahead)
{
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        if (!cars[i].active) continue;
        float ds = cars[i].s - s;
        float a = cars[i].dir > 0 ? ahead : ahead * 3;
        if (ds > -behind && ds < a) cars[i].active = false;
    }
}

static void computeTransform(TCar *c)
{
    Vector3 p;
    float hx, hz;
    RouteSample(c->s, &p, &hx, &hz, NULL, NULL);
    Vector3 pa, pb;
    RouteSample(c->s - 2.0f, &pa, NULL, NULL, NULL, NULL);
    RouteSample(c->s + 2.0f, &pb, NULL, NULL, NULL, NULL);
    float slope = (pb.y - pa.y) / 4.0f;
    // c->d is measured in the route frame (positive = right of the route direction)
    c->pos = (Vector3){ p.x - hz * c->d, p.y + 0.03f, p.z + hx * c->d };
    if (c->dir < 0) { hx = -hx; hz = -hz; slope = -slope; }
    Vector3 fwd = Vector3Normalize((Vector3){ hx, slope, hz });
    Vector3 left = Vector3Normalize(Vector3CrossProduct((Vector3){ 0, 1, 0 }, fwd));
    Vector3 up = Vector3CrossProduct(fwd, left);
    c->yaw = atan2f(hz, hx);
    c->m = (Matrix){ left.x, up.x, fwd.x, c->pos.x, left.y, up.y, fwd.y, c->pos.y, left.z, up.z, fwd.z, c->pos.z, 0, 0, 0, 1 };
}

void TrafficUpdate(float dt, const PlayerCar *pc, float pcLen, float pcWid)
{
    (void)pcWid;
    float ps = pc->s;
    int pIdx = RouteIndexAtS(ps);
    // ---- spawning ahead of the player
    while (cursorSame < ps + 1300 && cursorSame < W.length - 60) {
        int idx = RouteIndexAtS(cursorSame);
        const RoutePt *p = &W.route[idx];
        bool mw = p->roadClass >= RC_MOTORWAY;
        if (cursorSame > 150) spawn(cursorSame, +1, ps);
        cursorSame += mw ? frand(55, 150) : (p->flags & RF_BUILTUP) ? frand(140, 300) : frand(190, 420);
    }
    while (cursorOnc < ps + 1500 && cursorOnc < W.length - 30) {
        int idx = RouteIndexAtS(cursorOnc);
        const RoutePt *p = &W.route[idx];
        if (isTwoWayPt(p)) spawn(cursorOnc, -1, ps);
        cursorOnc += (p->flags & RF_BUILTUP) ? frand(200, 400) : frand(240, 520);
    }
    if (cursorSame < ps + 100) cursorSame = ps + 900;
    if (cursorOnc < ps + 100) cursorOnc = ps + 1100;

    for (int i = 0; i < MAX_TRAFFIC; i++) {
        TCar *c = &cars[i];
        if (!c->active) continue;
        if (c->s < ps - 220 || c->s > ps + 1900 || c->s < 1 || c->s > W.length - 2) { c->active = false; continue; }
        int idx = RouteIndexAtS(c->s);
        const RoutePt *p = &W.route[idx];
        int lanes = LaneCount(idx, c->dir);
        if (lanes <= 0) {
            // oncoming car reaching a one-way section: drop it if far from view
            if (fabsf(c->s - ps) > 250) { c->active = false; continue; }
            lanes = 1;
        }
        if (c->lane >= lanes) c->lane = lanes - 1;
        float want = limitAt(idx) * c->vWanted;
        if (modelKind[c->model] == 3) want = fminf(want, p->roadClass >= RC_MOTORWAY ? 25.0f : 22.0f);
        // anticipate bends / roundabouts 60 m ahead
        float look = c->s + c->dir * 60.0f;
        if (look > 0 && look < W.length) want = fminf(want, limitAt(RouteIndexAtS(look)) * c->vWanted * 1.05f);

        // follow the vehicle ahead (same direction, same lane)
        float ahead = 1e9f, leadV = 0;
        for (int k = 0; k < MAX_TRAFFIC; k++) {
            if (k == i || !cars[k].active || cars[k].dir != c->dir) continue;
            if (fabsf(cars[k].d - c->d) > 1.9f) continue;
            float gap = (cars[k].s - c->s) * c->dir - (carLen(c) + carLen(&cars[k])) * 0.5f;
            if (gap > -1 && gap < ahead) { ahead = gap; leadV = cars[k].v; }
        }
        // the player is an obstacle too
        float pd = pc->d;                 // both measured in the route frame
        float cdRoute = c->d;
        if (fabsf(pd - cdRoute) < 2.1f) {
            float gap = (ps - c->s) * c->dir - (carLen(c) + pcLen) * 0.5f;
            if (gap > -1 && gap < ahead) { ahead = gap; leadV = c->dir > 0 ? fmaxf(pc->speed, 0) : 0; }
            if (c->dir < 0 && gap > 0 && gap < 140 && c->honkT <= 0) { c->honkT = 3.0f; honkEvents++; }
        }
        float safe = 8 + c->v * 1.5f;
        bool blocked = false;
        if (ahead < safe) {
            want = fminf(want, leadV * Clamp(ahead / safe, 0, 1));
            blocked = ahead < safe * 0.8f;
        }
        // overtaking on multi-lane carriageways
        c->laneCd -= dt;
        if (lanes > 1 && c->laneCd <= 0) {
            int target = -1;
            if (blocked && c->lane + 1 < lanes) target = c->lane + 1;        // move left to overtake
            else if (!blocked && c->lane > 0 && GetRandomValue(0, 3) == 0) target = c->lane - 1;   // back to the right
            if (target >= 0) {
                float td = LaneCenterD(idx, c->dir, target);
                bool freeP = !(c->dir > 0 && fabsf(pc->d - td) < 2.4f && fabsf(ps - c->s) < 30);
                if (laneFree(c->dir, c->s, td, 28, i) && freeP) c->lane = target;
            }
            c->laneCd = frand(2.5f, 5.0f);
        }
        float acc = want > c->v ? 2.2f : (blocked ? 7.5f : 4.0f);
        c->braking = want < c->v - 0.3f;
        if (c->v < want) c->v = fminf(want, c->v + acc * dt);
        else c->v = fmaxf(want, c->v - acc * dt);
        c->s += c->dir * c->v * dt;
        float td = LaneCenterD(RouteIndexAtS(c->s), c->dir, c->lane);
        float rate = 1.3f * dt;
        c->d += Clamp(td - c->d, -rate, rate);
        c->spin += c->v * dt / fmaxf(0.3f, models[c->model].wheelRadius);
        if (c->honkT > 0) c->honkT -= dt;
        computeTransform(c);

        // near miss (passed within ~1 m)
        if (!c->passed) {
            float rel = (c->s - ps) * c->dir;
            if ((c->dir > 0 && c->s < ps) || (c->dir < 0 && c->s < ps)) {
                c->passed = true;
                float lat = fabsf(pc->d - cdRoute) - 1.75f;
                if (lat < 0.9f && fabsf(pc->speed) > 15) nearMiss++;
            }
            (void)rel;
        }
    }
    (void)pIdx;
}

void TrafficDraw(Camera3D cam)
{
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        TCar *c = &cars[i];
        if (!c->active) continue;
        Vector3 rel = Vector3Subtract(c->pos, cam.position);
        float dist = Vector3Length(rel);
        if (dist > 1100) continue;
        if (Vector3DotProduct(rel, fwd) < -20) continue;
        const CarModel *m = &models[c->model];
        DrawBlobShadow(c->m, m->length, m->width);
        DrawCarModel(m, c->m, c->spin, 0, c->braking);
    }
}

static bool obbOverlap(Vector3 pa, float yawA, float la, float wa, Vector3 pb, float yawB, float lb, float wb)
{
    float ax[2][2] = { { cosf(yawA), sinf(yawA) }, { -sinf(yawA), cosf(yawA) } };
    float bx[2][2] = { { cosf(yawB), sinf(yawB) }, { -sinf(yawB), cosf(yawB) } };
    float ha[2] = { la * 0.5f, wa * 0.5f }, hb[2] = { lb * 0.5f, wb * 0.5f };
    float dx = pb.x - pa.x, dz = pb.z - pa.z;
    for (int k = 0; k < 4; k++) {
        float *axis = k < 2 ? ax[k] : bx[k - 2];
        float ra = fabsf(ha[0] * (ax[0][0] * axis[0] + ax[0][1] * axis[1])) + fabsf(ha[1] * (ax[1][0] * axis[0] + ax[1][1] * axis[1]));
        float rb = fabsf(hb[0] * (bx[0][0] * axis[0] + bx[0][1] * axis[1])) + fabsf(hb[1] * (bx[1][0] * axis[0] + bx[1][1] * axis[1]));
        if (fabsf(dx * axis[0] + dz * axis[1]) > ra + rb) return false;
    }
    return true;
}

int TrafficCollide(const PlayerCar *pc, float len, float wid)
{
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        TCar *c = &cars[i];
        if (!c->active || fabsf(c->s - pc->s) > 25) continue;
        const CarModel *m = &models[c->model];
        if (obbOverlap(pc->pos, pc->yaw, len * 0.94f, wid * 0.9f, c->pos, c->yaw, m->length * 0.94f, m->width * 0.9f)) return i;
    }
    return -1;
}

void TrafficOnCrash(int idx)
{
    if (idx < 0 || idx >= MAX_TRAFFIC) return;
    cars[idx].v *= 0.3f;
    cars[idx].vWanted = 0.0f;   // it stops (hazard)
}

int TrafficHonking(void) { int h = honkEvents; honkEvents = 0; return h; }
int TrafficNearMiss(void) { int n = nearMiss; nearMiss = 0; return n; }
