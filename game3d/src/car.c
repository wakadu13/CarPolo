// Player car physics and car drawing helpers
#include "car.h"
#include "world.h"
#include "scene.h"
#include "render.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stddef.h>

#define WHEELBASE 2.60f
#define WHEEL_R   0.32f

static Mesh shadowMesh;
static Material shadowMat;
static bool sharedReady;

static float approachf(float v, float t, float rate)
{
    if (v < t) return fminf(t, v + rate);
    return fmaxf(t, v - rate);
}

void CarInitShared(void)
{
    if (sharedReady) return;
    Image im = GenImageGradientRadial(64, 64, 0.15f, (Color){ 0, 0, 0, 170 }, (Color){ 0, 0, 0, 0 });
    Texture2D t = LoadTextureFromImage(im);
    UnloadImage(im);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    shadowMesh = GenMeshPlane(1, 1, 1, 1);
    shadowMat = LoadMaterialDefault();
    shadowMat.maps[MATERIAL_MAP_DIFFUSE].texture = t;
    sharedReady = true;
}

int LaneCount(int idx, int dir)
{
    const RoutePt *p = &W.route[idx];
    bool oneway = (p->flags & RF_ONEWAY) || p->lanesForward >= p->lanes;
    if (oneway) return dir > 0 ? p->lanes : 0;
    return dir > 0 ? p->lanesForward : p->lanes - p->lanesForward;
}

float LaneCenterD(int idx, int dir, int lane)
{
    const RoutePt *p = &W.route[idx];
    float lw = p->width / p->lanes;
    if (dir > 0) return p->width * 0.5f - (lane + 0.5f) * lw;
    return -p->width * 0.5f + (lane + 0.5f) * lw;
}

static float pavedRight(const RoutePt *p) { return p->width * 0.5f + (p->roadClass >= RC_MOTORWAY ? 2.6f : 0.0f); }
static float pavedLeft(const RoutePt *p) { return -p->width * 0.5f - (p->roadClass >= RC_MOTORWAY ? 0.9f : 0.0f); }

float CarRoadSurfaceY(float x, float z, int hint, float *outS, float *outD, int *outSeg, bool *onRoad)
{
    float d;
    int seg;
    float s = RouteProject(x, z, hint, hint < 0 ? 0 : 40, &d, &seg);
    const RoutePt *a = &W.route[seg], *b = &W.route[seg + 1 < W.nRoute ? seg + 1 : seg];
    float t = (b->s - a->s) > 1e-3f ? Clamp((s - a->s) / (b->s - a->s), 0, 1) : 0;
    float ry = a->y + (b->y - a->y) * t + 0.03f;
    bool on = d >= pavedLeft(a) - 0.3f && d <= pavedRight(a) + 0.3f && s > 0.01f && s < W.length - 0.01f;
    if (outS) *outS = s;
    if (outD) *outD = d;
    if (outSeg) *outSeg = seg;
    if (onRoad) *onRoad = on;
    if (on) return ry;
    return SceneGround(x, z, seg);
}

void CarReset(PlayerCar *c, float s, float d)
{
    PlayerCar z = { 0 };
    *c = z;
    Vector3 p;
    float hx, hz;
    RouteSample(s, &p, &hx, &hz, NULL, NULL);
    c->pos = (Vector3){ p.x - hz * d, p.y + 0.03f, p.z + hx * d };
    c->yaw = atan2f(hz, hx);
    c->s = s;
    c->d = d;
    c->seg = RouteIndexAtS(s);
    c->onRoad = true;
    c->up = (Vector3){ 0, 1, 0 };
    c->fwd = (Vector3){ hx, 0, hz };
    c->gear = 1;
    c->rpm = 850;
}

void CarUpdate(PlayerCar *c, float dt, float throttle, float brake, float steer, bool analog, bool assist)
{
    float v = c->speed, av = fabsf(v);
    // ---- steering
    if (analog) c->steerIn = steer;
    else c->steerIn = approachf(c->steerIn, steer, (fabsf(steer) > fabsf(c->steerIn) ? 3.2f : 6.0f) * dt);
    float maxSteer = 0.52f / (1.0f + av / 11.0f);
    c->steerAngle = c->steerIn * maxSteer;
    float yawRate = v * tanf(c->steerAngle) / WHEELBASE;

    const RoutePt *rp = &W.route[c->seg];
    float roadYaw = atan2f(rp->hz, rp->hx);
    float diff = roadYaw - c->yaw;
    while (diff > PI) diff -= 2 * PI;
    while (diff < -PI) diff += 2 * PI;
    if (assist && v > 1.0f && fabsf(diff) < 0.6f && fabsf(c->d) < rp->width * 0.5f + 6.0f) {
        // driving aid: follow the road's curvature, and realign gently when the wheel is released
        yawRate += v * rp->curv;
        if (fabsf(c->steerIn) < 0.08f) yawRate += diff * 1.6f;
    }
    // ---- grip limit (understeer)
    float aMax = c->onRoad ? 8.8f : 5.0f;
    float lat = yawRate * v;
    c->skid = 0;
    if (fabsf(lat) > aMax && av > 1) {
        c->skid = Clamp((fabsf(lat) - aMax) / aMax, 0, 1);
        yawRate = (lat > 0 ? 1 : -1) * aMax / av * (v >= 0 ? 1 : -1);
    }
    c->yaw += yawRate * dt;
    c->yawRate = yawRate;

    // ---- longitudinal
    float a = 0;
    if (throttle > 0) {
        if (v >= -0.3f) a += throttle * fminf(4.3f, 72.0f / fmaxf(av, 1.0f));
        else a += throttle * 9.0f;               // throttle while rolling backwards = brake
    }
    if (brake > 0) {
        if (v > 0.3f) a -= brake * 9.6f;
        else if (throttle <= 0) a -= brake * 2.6f; // reverse gear
    }
    float sgn = v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f);
    a -= 0.00042f * v * av + 0.12f * sgn;
    if (throttle <= 0 && brake <= 0) a -= 0.45f * sgn;  // engine braking
    if (!c->onRoad) a -= sgn * (0.6f + av * 0.05f);
    if (brake > 0.5f && av > 14) c->skid = fmaxf(c->skid, 0.35f);
    float nv = v + a * dt;
    if (throttle <= 0 && brake <= 0 && ((v > 0 && nv < 0) || (v < 0 && nv > 0))) nv = 0;
    if (brake > 0 && v > 0 && nv < 0) nv = 0;          // stop before reversing
    if (nv < -5.5f) nv = -5.5f;
    c->lonAcc = c->lonAcc + (a - c->lonAcc) * fminf(1, dt * 5);
    c->latAcc = c->latAcc + (yawRate * v - c->latAcc) * fminf(1, dt * 5);
    c->speed = nv;

    // ---- integrate position
    float fx = cosf(c->yaw), fz = sinf(c->yaw);
    c->pos.x += fx * c->speed * dt;
    c->pos.z += fz * c->speed * dt;
    c->odometer += fabsf(c->speed) * dt;
    c->wheelSpin += c->speed * dt / WHEEL_R;

    // ---- ground following and attitude
    float s, d;
    int seg;
    bool on;
    float y = CarRoadSurfaceY(c->pos.x, c->pos.z, c->seg, &s, &d, &seg, &on);
    c->s = s; c->d = d; c->seg = seg; c->onRoad = on;
    float yF = CarRoadSurfaceY(c->pos.x + fx * 1.3f, c->pos.z + fz * 1.3f, seg, NULL, NULL, NULL, NULL);
    float yB = CarRoadSurfaceY(c->pos.x - fx * 1.3f, c->pos.z - fz * 1.3f, seg, NULL, NULL, NULL, NULL);
    float yR = CarRoadSurfaceY(c->pos.x - fz * 0.75f, c->pos.z + fx * 0.75f, seg, NULL, NULL, NULL, NULL);
    float yL = CarRoadSurfaceY(c->pos.x + fz * 0.75f, c->pos.z - fx * 0.75f, seg, NULL, NULL, NULL, NULL);
    float target = (y + (yF + yB) * 0.5f) * 0.5f;
    if (c->pos.y < target - 0.6f || c->pos.y > target + 1.5f) c->pos.y = target;
    c->pos.y += (target - c->pos.y) * fminf(1, dt * 18);
    Vector3 fwd3 = Vector3Normalize((Vector3){ fx * 2.6f, yF - yB, fz * 2.6f });
    Vector3 right3 = Vector3Normalize((Vector3){ -fz * 1.5f, yR - yL, fx * 1.5f });
    Vector3 up = Vector3Normalize(Vector3CrossProduct(right3, fwd3));
    c->up = Vector3Normalize(Vector3Lerp(c->up, up, fminf(1, dt * 10)));
    c->fwd = fwd3;

    // suspension: visual bounce, stronger off-road
    float exc = !on ? (GetRandomValue(-100, 100) / 100.0f) * av * 0.004f : 0;
    c->bounceV += (-c->bounce * 180.0f - c->bounceV * 14.0f) * dt + exc;
    c->bounce += c->bounceV * dt;
    c->roll = approachf(c->roll, Clamp(-c->latAcc * 0.0065f, -0.07f, 0.07f), dt * 0.6f);
    c->pitch = approachf(c->pitch, Clamp(-c->lonAcc * 0.006f, -0.05f, 0.05f), dt * 0.4f);

    // ---- engine rpm and gear (for the sound)
    static const float top[6] = { 0, 42, 74, 110, 150, 195 };
    float kmh = av * 3.6f;
    int g = 1;
    while (g < 5 && kmh > top[g] * 0.80f) g++;
    if (c->speed < -0.1f) g = 1;
    c->gear = g;
    float rpm = 850 + (kmh / top[g]) * 5400.0f;
    if (throttle > 0 && kmh < 8) rpm = fmaxf(rpm, 1500 + throttle * 1500);
    c->rpm = c->rpm + (Clamp(rpm, 800, 6300) - c->rpm) * fminf(1, dt * 8);
}

Matrix CarMatrix(const PlayerCar *c, bool withBody)
{
    Vector3 up = c->up;
    Vector3 f0 = { cosf(c->yaw), 0, sinf(c->yaw) };
    Vector3 left = Vector3Normalize(Vector3CrossProduct(up, f0));
    Vector3 fwd = Vector3CrossProduct(left, up);
    Matrix m = { left.x, up.x, fwd.x, c->pos.x,
                 left.y, up.y, fwd.y, c->pos.y + (withBody ? c->bounce : 0),
                 left.z, up.z, fwd.z, c->pos.z,
                 0, 0, 0, 1 };
    if (withBody) {
        Matrix att = MatrixMultiply(MatrixRotateX(c->pitch), MatrixRotateZ(c->roll));
        m = MatrixMultiply(att, m);
    }
    return m;
}

static void setCarMat(int kind, bool brakeLights)
{
    CarMatParams p = GetCarMatParams(kind);
    float em = p.emissive;
    if (kind == CARMAT_LIGHT_REAR) em = brakeLights ? 1.0f : 0.35f;
    RenderSetMatEx(p.spec, p.shininess, p.refl, p.reflMin, em);
}

void DrawCarModel(const CarModel *m, Matrix world, float wheelSpin, float steerAngle, bool brakeLights)
{
    for (int i = 0; i < m->body.meshCount; i++) {
        int mi = m->body.meshMaterial ? m->body.meshMaterial[i] : 0;
        setCarMat(m->bodyMatKind[mi], brakeLights);
        Material mat = m->body.materials[mi];
        mat.shader = RenderLitShader();
        DrawMesh(m->body.meshes[i], mat, world);
    }
    for (int w = 0; w < 4; w++) {
        bool right = (w == 1 || w == 3), front = (w < 2);
        Matrix wm = MatrixRotateX(right ? -wheelSpin : wheelSpin);
        if (right) wm = MatrixMultiply(wm, MatrixRotateY(PI));
        if (front) wm = MatrixMultiply(wm, MatrixRotateY(-steerAngle));
        wm = MatrixMultiply(wm, MatrixTranslate(m->wheelPos[w].x, m->wheelPos[w].y, m->wheelPos[w].z));
        wm = MatrixMultiply(wm, world);
        for (int i = 0; i < m->wheel.meshCount; i++) {
            int mi = m->wheel.meshMaterial ? m->wheel.meshMaterial[i] : 0;
            setCarMat(m->wheelMatKind[mi], false);
            Material mat = m->wheel.materials[mi];
            mat.shader = RenderLitShader();
            DrawMesh(m->wheel.meshes[i], mat, wm);
        }
    }
    RenderSetMat(MAT_MATTE);
}

void DrawBlobShadow(Matrix world, float length, float width)
{
    Matrix m = MatrixMultiply(MatrixScale(width * 1.25f, 1, length * 1.12f), MatrixTranslate(0, 0.04f, 0));
    m = MatrixMultiply(m, world);
    DrawMesh(shadowMesh, shadowMat, m);
}
