// CarPolo 3D — game states, camera, input, HUD and menus
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "world.h"
#include "render.h"
#include "scene.h"
#include "car.h"
#include "car_model.h"
#include "traffic.h"
#include "audio.h"
#include "signs.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#if defined(PLATFORM_ANDROID)
#include "platform_android.h"
#endif
#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
EM_JS(void, cp_web_save, (const char *hex), { try { localStorage.setItem('carpolo3d', UTF8ToString(hex)); } catch (e) {} });
EM_JS(char *, cp_web_load, (void), {
    var s = '';
    try { s = localStorage.getItem('carpolo3d') || ''; } catch (e) {}
    var n = lengthBytesUTF8(s) + 1, p = _malloc(n);
    stringToUTF8(s, p, n);
    return p;
});
EM_JS(int, cp_web_flag, (const char *name), { return (location.search.indexOf(UTF8ToString(name)) >= 0) ? 1 : 0; });
EM_JS(int, cp_web_width, (void), { return window.innerWidth; });
EM_JS(int, cp_web_height, (void), { return window.innerHeight; });
EM_JS(void, cp_web_alert, (const char *msg), { alert(UTF8ToString(msg)); });
#endif

typedef enum { ST_LOADING, ST_TITLE, ST_COUNTDOWN, ST_RACE, ST_PAUSE, ST_FINISH, ST_GAMEOVER } State;
typedef enum { CAM_CHASE, CAM_FAR, CAM_HOOD, CAM_COUNT } CamMode;

typedef struct {
    bool left, right, gas, brake, horn, pause, cam;
    float steerAnalog; bool analog;
    bool touchActive;
} Input;

typedef struct {
    State state, prevState;
    PlayerCar car;
    CarModel cactus;
    bool cactusReady;
    Camera3D cam;
    CamMode camMode;
    Vector3 camPos, camTarget;
    float camShake;
    Input in;
    float raceTime, countdown, stateT;
    int points, crashes, nearMisses;
    float crashT; bool crashed; int crashDir; float crashSpin; float invuln;
    float offTime;
    float topSpeed;
    float bestTime;
    bool newRecord;
    bool assist, sound;
    int quality;
    int zone; float bannerT;
    char msg[64]; float msgT; Color msgCol;
    float goT;
    bool touchUI;
    bool quit;
    float horizonFog;
    Font font;
    bool fontOk;
    // minimap
    Vector2 *mini; int nMini;
    float miniMinX, miniMinZ, miniMaxX, miniMaxZ;
} Game;

static Game G;

#if !defined(PLATFORM_ANDROID)
#include <stdlib.h>
// ---- automated test (desktop: CARPOLO_TEST=1, web: ?test): autopilot, screenshots on desktop
static int testMode;
static float testT;
static int testStep;
static bool testWrongLane;
static void Autopilot(Input *in)
{
    PlayerCar *c = &G.car;
    memset(in, 0, sizeof(*in));
    int idx = c->seg;
    float target = LaneCenterD(idx, testWrongLane && LaneCount(idx, -1) > 0 ? -1 : +1, 0);
    float err = target - c->d;
    float roadYaw = atan2f(W.route[idx].hz, W.route[idx].hx), hd = roadYaw - c->yaw;
    while (hd > PI) hd -= 2 * PI;
    while (hd < -PI) hd += 2 * PI;
    in->analog = true;
    in->steerAnalog = Clamp(err * 0.18f + hd * 2.2f, -1, 1);
    // speed: respect bends ahead
    float want = 90 / 3.6f;
    for (float a = 0; a < 120; a += 10) {
        int k = RouteIndexAtS(c->s + a);
        float cv = fabsf(W.route[k].curv);
        if (cv > 1e-4f) want = fminf(want, sqrtf(7.0f / cv));
    }
    in->gas = c->speed < want;
    in->brake = c->speed > want + 3;
}
#endif

// ------------------------------------------------------------------ helpers
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
// GLFW is bundled inside raylib. It is used here only to check, with small invisible windows, which
// OpenGL 3.3 setup the graphics driver accepts before raylib opens the real window: raylib 5.5 crashes
// when window creation fails (some VMs, very old drivers, or a missing 4x multisampling format on
// some Macs). Constants from GLFW 3.4's glfw3.h.
typedef struct GLFWwindow GLFWwindow;
int  glfwInit(void);
void glfwTerminate(void);
void glfwInitHint(int hint, int value);
void glfwDefaultWindowHints(void);
void glfwWindowHint(int hint, int value);
GLFWwindow *glfwCreateWindow(int width, int height, const char *title, void *monitor, void *share);
void glfwDestroyWindow(GLFWwindow *window);
#if defined(_WIN32)
__declspec(dllimport) int __stdcall MessageBoxA(void *hWnd, const char *text, const char *caption, unsigned int type);
#endif

// 2 = OpenGL 3.3 with 4x MSAA, 1 = OpenGL 3.3 without MSAA, 0 = no usable OpenGL 3.3.
static int ProbeOpenGL(void)
{
#if defined(__APPLE__)
    glfwInitHint(0x00051001, 0);             // GLFW_COCOA_CHDIR_RESOURCES = false, as raylib does
#endif
    if (!glfwInit()) return 0;
    const char *noMsaa = getenv("CARPOLO_NO_MSAA");
    int level = 0;
    for (int samples = (noMsaa && *noMsaa) ? 0 : 4; samples >= 0 && level == 0; samples -= 4) {
        glfwDefaultWindowHints();
        glfwWindowHint(0x00020004, 0);           // GLFW_VISIBLE = false
        glfwWindowHint(0x0002100D, samples);     // GLFW_SAMPLES
        glfwWindowHint(0x00022002, 3);           // GLFW_CONTEXT_VERSION_MAJOR
        glfwWindowHint(0x00022003, 3);           // GLFW_CONTEXT_VERSION_MINOR
        glfwWindowHint(0x00022008, 0x00032001);  // GLFW_OPENGL_PROFILE = CORE
#if defined(__APPLE__)
        glfwWindowHint(0x00022006, 1);           // GLFW_OPENGL_FORWARD_COMPAT
#endif
        GLFWwindow *w = glfwCreateWindow(64, 64, "probe", NULL, NULL);
        if (w) { glfwDestroyWindow(w); level = samples ? 2 : 1; }
    }
    glfwTerminate();
    return level;
}

static void ShowNoOpenGLMessage(void)
{
    const char *msg = "CarPolo 3D a besoin d'une carte graphique compatible OpenGL 3.3. "
                      "Mets a jour le pilote graphique de l'ordinateur, puis relance le jeu.";
    TraceLog(LOG_ERROR, "DISPLAY: no usable OpenGL 3.3 window (graphics driver too old, or no GPU)");
    if (testMode) return;                    // never block an automated run on a dialog
#if defined(_WIN32)
    MessageBoxA(NULL, msg, "CarPolo 3D", 0x10);  // MB_ICONERROR
#elif defined(__APPLE__)
    if (system("osascript -e 'display alert \"CarPolo 3D\" message \"CarPolo 3D a besoin d’une carte graphique "
           "compatible OpenGL 3.3.\" as critical' >/dev/null 2>&1")) {}
#else
    char cmd[512];
    snprintf(cmd, sizeof cmd, "(zenity --error --title='CarPolo 3D' --text=\"%s\" || "
             "kdialog --title 'CarPolo 3D' --error \"%s\") >/dev/null 2>&1", msg, msg);
    if (system(cmd)) {}
#endif
    (void)msg;
}
#endif


static const char *AssetPath(const char *name)
{
#if defined(PLATFORM_ANDROID)
    return name;
#elif defined(PLATFORM_WEB)
    static char buf[256];
    snprintf(buf, sizeof(buf), "/assets/%s", name);
    return buf;
#else
    // release layout: <exe dir>/assets ; macOS bundle: Contents/MacOS/../Resources/assets ; dev: build-bin/../assets
    static char buf[1024];
    const char *dir = GetApplicationDirectory();
    const char *cands[] = { "%sassets/%s", "%s../Resources/assets/%s", "%s../assets/%s" };
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), cands[i], dir, name);
        if (FileExists(buf)) return buf;
    }
    snprintf(buf, sizeof(buf), "assets/%s", name);
    return buf;
#endif
}

static const char *SavePath(void)
{
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB)
    return "carpolo3d.sav";
#else
    static char buf[1024];
    char dir[900] = { 0 };
    const char *home = getenv("HOME");
#if defined(_WIN32)
    const char *appdata = getenv("APPDATA");
    if (appdata) snprintf(dir, sizeof(dir), "%s\\CarPolo3D", appdata);
#elif defined(__APPLE__)
    if (home) snprintf(dir, sizeof(dir), "%s/Library/Application Support/CarPolo3D", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/carpolo3d", xdg);
    else if (home) snprintf(dir, sizeof(dir), "%s/.local/share/carpolo3d", home);
#endif
    (void)home;
    if (dir[0] && (DirectoryExists(dir) || MakeDirectory(dir) == 0)) snprintf(buf, sizeof(buf), "%s/carpolo3d.sav", dir);
    else snprintf(buf, sizeof(buf), "%scarpolo3d.sav", GetApplicationDirectory());
    return buf;
#endif
}

typedef struct { float best; int assist, sound, cam, quality; } SaveData;

static void LoadSave(void)
{
#if defined(PLATFORM_ANDROID)
    G.bestTime = 0; G.assist = true; G.sound = true; G.camMode = CAM_CHASE; G.quality = 1;
#else
    G.bestTime = 0; G.assist = true; G.sound = true; G.camMode = CAM_CHASE; G.quality = 2;
#endif
    SaveData s;
    bool ok = false;
#if defined(PLATFORM_WEB)
    // localStorage holds the struct as hex
    char *hex = cp_web_load();
    if (hex && strlen(hex) == 2 * sizeof(SaveData)) {
        unsigned char *raw = (unsigned char *)&s;
        ok = true;
        for (size_t i = 0; i < sizeof(SaveData); i++) {
            unsigned v;
            if (sscanf(hex + 2 * i, "%2x", &v) != 1) { ok = false; break; }
            raw[i] = (unsigned char)v;
        }
    }
    free(hex);
#else
    int size = 0;
    unsigned char *d = LoadFileData(SavePath(), &size);
    if (d && size == (int)sizeof(SaveData)) { memcpy(&s, d, sizeof(s)); ok = true; }
    if (d) UnloadFileData(d);
#endif
    if (ok) {
        G.bestTime = s.best; G.assist = s.assist; G.sound = s.sound;
        G.camMode = (CamMode)(s.cam >= 0 && s.cam < CAM_COUNT ? s.cam : 0);
        G.quality = s.quality < 0 ? 0 : s.quality > 2 ? 2 : s.quality;
    }
#if defined(PLATFORM_WEB)
    if (!ok) G.quality = 1;      // browsers: start at "normal", the menu can raise it
#endif
}

static void WriteSave(void)
{
#if !defined(PLATFORM_ANDROID)
    if (testMode) return;
#endif
    SaveData s = { G.bestTime, G.assist, G.sound, (int)G.camMode, G.quality };
#if defined(PLATFORM_WEB)
    char hex[2 * sizeof(SaveData) + 1];
    const unsigned char *raw = (const unsigned char *)&s;
    for (size_t i = 0; i < sizeof(SaveData); i++) snprintf(hex + 2 * i, 3, "%02x", raw[i]);
    cp_web_save(hex);
#else
    SaveFileData(SavePath(), &s, sizeof(s));
#endif
}

static void Vibrate(int ms)
{
#if defined(PLATFORM_ANDROID)
    AndroidVibrate(ms);
#else
    (void)ms;
#endif
}

static float U(void) { return fminf(GetScreenHeight() / 360.0f, GetScreenWidth() / 640.0f); }

static void Text(const char *t, float x, float y, float size, Color c, int align)
{
    Vector2 m = G.fontOk ? MeasureTextEx(G.font, t, size, size * 0.02f) : (Vector2){ (float)MeasureText(t, (int)size), size };
    if (align == 1) x -= m.x * 0.5f;
    else if (align == 2) x -= m.x;
    Color sh = { 0, 0, 0, (unsigned char)(c.a * 0.55f) };
    if (G.fontOk) {
        DrawTextEx(G.font, t, (Vector2){ x + size * 0.05f, y + size * 0.07f }, size, size * 0.02f, sh);
        DrawTextEx(G.font, t, (Vector2){ x, y }, size, size * 0.02f, c);
    } else {
        DrawText(t, (int)x + 2, (int)y + 2, (int)size, sh);
        DrawText(t, (int)x, (int)y, (int)size, c);
    }
}

static const char *FmtTime(float t)
{
    if (t < 0) t = 0;
    int m = (int)(t / 60), s = (int)t % 60, c = (int)((t - floorf(t)) * 100);
    return TextFormat("%d:%02d.%02d", m, s, c);
}

static void Message(const char *t, Color c, float dur)
{
    snprintf(G.msg, sizeof(G.msg), "%s", t);
    G.msgCol = c;
    G.msgT = dur;
}

// ------------------------------------------------------------------ buttons
typedef struct { Rectangle r; const char *label; } Button;

static bool released(Rectangle r)
{
    return IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), r);
}

static bool UIButton(Rectangle r, const char *label, Color col, float fs)
{
    bool hot = CheckCollisionPointRec(GetMousePosition(), r) && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    float u = U();
    DrawRectangleRounded((Rectangle){ r.x, r.y + 4 * u, r.width, r.height }, 0.35f, 8, (Color){ 0, 0, 0, 90 });
    DrawRectangleRounded(hot ? (Rectangle){ r.x, r.y + 2 * u, r.width, r.height } : r, 0.35f, 8, hot ? ColorBrightness(col, 0.15f) : col);
    Text(label, r.x + r.width / 2, r.y + (hot ? 2 * u : 0) + r.height / 2 - fs * 0.55f, fs, WHITE, 1);
    bool clicked = released(r);
    if (clicked) AudioPlay(SFX_CLICK);
    return clicked;
}

// ------------------------------------------------------------------ input
static void UpdateInput(void)
{
    Input *in = &G.in;
    bool pl = in->pause, pc = in->cam, ph = in->horn;
    memset(in, 0, sizeof(*in));
    int W_ = GetScreenWidth(), H_ = GetScreenHeight();
    float u = U();
#if defined(PLATFORM_ANDROID)
    int n = GetTouchPointCount();
#else
    int n = 0;
#if defined(PLATFORM_WEB)
    n = GetTouchPointCount();
#endif
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && n == 0) {
        Vector2 m = GetMousePosition();
        if (m.x < 70 * u && m.y < 70 * u) in->pause = true;
        else if (m.x > W_ - 70 * u && m.y > 150 * u && m.y < 220 * u) in->cam = true;
    }
#endif
    for (int i = 0; i < n; i++) {
        Vector2 p = GetTouchPosition(i);
        if (p.x <= 0 && p.y <= 0) continue;
        in->touchActive = true;
        G.touchUI = true;
        // left third, lower 2/3: steering (left / right halves)
        if (p.x < W_ * 0.30f && p.y > H_ * 0.30f) {
            if (p.x < 112 * u) in->left = true; else in->right = true;
        } else if (p.x > W_ - 140 * u && p.y > H_ - 160 * u) in->gas = true;
        else if (p.x > W_ - 250 * u && p.x <= W_ - 140 * u && p.y > H_ - 140 * u) in->brake = true;
        else if (p.x > W_ - 200 * u && p.y > H_ - 230 * u && p.y <= H_ - 160 * u) in->horn = true;
        else if (p.x < 70 * u && p.y < 70 * u) in->pause = true;
        else if (p.x > W_ - 70 * u && p.y > 150 * u && p.y < 220 * u) in->cam = true;
    }
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
    // F11 or Alt+Enter: borderless fullscreen (in a browser F11 is the browser's own full screen)
    if (IsKeyPressed(KEY_F11) || ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) && IsKeyPressed(KEY_ENTER))) ToggleBorderlessWindowed();
#endif
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A) || IsKeyDown(KEY_Q)) in->left = true;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) in->right = true;
    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W) || IsKeyDown(KEY_Z)) in->gas = true;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) in->brake = true;
    if (IsKeyDown(KEY_H) || IsKeyDown(KEY_K)) in->horn = true;
    if (IsKeyDown(KEY_C)) in->cam = true;
    if (IsKeyDown(KEY_P) || IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_BACK)) in->pause = true;
    if (IsGamepadAvailable(0)) {
        float ax = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        if (fabsf(ax) > 0.12f) { in->analog = true; in->steerAnalog = ax; }
        if (GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_TRIGGER) > -0.5f || IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) in->gas = true;
        if (GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_TRIGGER) > -0.5f || IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) in->brake = true;
    }
#if !defined(PLATFORM_ANDROID)
    if (testMode && (G.state == ST_RACE || G.state == ST_COUNTDOWN)) Autopilot(in);
#endif
    // edge detection stored in the "pressed" flags via previous values
    static bool prevPause, prevCam, prevHorn;
    bool p0 = in->pause, c0 = in->cam, h0 = in->horn;
#if !defined(PLATFORM_ANDROID)
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { prevPause = false; prevCam = false; }
#endif
    in->pause = p0 && !prevPause;
    in->cam = c0 && !prevCam;
    in->horn = h0 && !prevHorn;
    prevPause = p0; prevCam = c0; prevHorn = h0;
    (void)pl; (void)pc; (void)ph;
}

// ------------------------------------------------------------------ camera
static void UpdateCamera3D(float dt)
{
    PlayerCar *c = &G.car;
    Vector3 f = { cosf(c->yaw), 0, sinf(c->yaw) };
    Vector3 up = { 0, 1, 0 };
    Vector3 wantPos, wantTgt;
    float fovy = 58 + fminf(fabsf(c->speed) * 0.18f, 10);
    if (G.state == ST_TITLE || G.state == ST_LOADING) {
        float a = (float)GetTime() * 0.12f + 2.2f;
        wantPos = (Vector3){ c->pos.x + cosf(a) * 7.0f, c->pos.y + 2.2f, c->pos.z + sinf(a) * 7.0f };
        wantTgt = (Vector3){ c->pos.x, c->pos.y + 0.8f, c->pos.z };
        G.camPos = wantPos; G.camTarget = wantTgt;
        fovy = 50;
    } else if (G.camMode == CAM_HOOD) {
        wantPos = Vector3Add(c->pos, Vector3Add(Vector3Scale(f, 0.55f), Vector3Scale(c->up, 1.32f)));
        wantTgt = Vector3Add(wantPos, Vector3Add(Vector3Scale(f, 20.0f), (Vector3){ 0, -1.2f, 0 }));
        G.camPos = wantPos;
        G.camTarget = wantTgt;
        fovy += 6;
    } else {
        float back = G.camMode == CAM_FAR ? 9.5f : 6.3f, hgt = G.camMode == CAM_FAR ? 3.4f : 2.15f;
        // look direction lags the car heading a bit (smooth)
        wantPos = Vector3Add(c->pos, Vector3Add(Vector3Scale(f, -back), Vector3Scale(up, hgt)));
        wantTgt = Vector3Add(c->pos, Vector3Add(Vector3Scale(f, 4.0f), Vector3Scale(up, 1.0f)));
        float k = 1.0f - expf(-dt * 7.5f);
        G.camPos = Vector3Lerp(G.camPos, wantPos, k);
        G.camTarget = Vector3Lerp(G.camTarget, wantTgt, 1.0f - expf(-dt * 12.0f));
        // never below the ground
        float g = SceneGround(G.camPos.x, G.camPos.z, c->seg) + 0.6f;
        if (G.camPos.y < g) G.camPos.y = g;
    }
    Vector3 shake = { 0 };
    if (G.camShake > 0) {
        shake = (Vector3){ GetRandomValue(-100, 100) / 100.0f * G.camShake * 0.25f, GetRandomValue(-100, 100) / 100.0f * G.camShake * 0.2f, 0 };
        G.camShake = fmaxf(0, G.camShake - dt * 2.5f);
    }
    if (!c->onRoad && fabsf(c->speed) > 3 && G.camMode != CAM_HOOD) shake.y += sinf((float)GetTime() * 40) * 0.03f;
    G.cam.position = Vector3Add(G.camPos, shake);
    G.cam.target = G.camTarget;
    G.cam.up = up;
    G.cam.fovy = fovy;
    G.cam.projection = CAMERA_PERSPECTIVE;
}

// ------------------------------------------------------------------ race logic
static float startD(void) { return LaneCenterD(0, +1, 0); }

static void StartRace(void)
{
    CarReset(&G.car, 4.0f, startD());
    TrafficReset(G.car.s);
    G.raceTime = 0;
    G.points = 12;
    G.crashes = 0;
    G.nearMisses = 0;
    G.crashed = false;
    G.invuln = 0;
    G.offTime = 0;
    G.topSpeed = 0;
    G.zone = -1;
    G.bannerT = 0;
    G.msgT = 0;
    G.countdown = 3.0f;
    G.newRecord = false;
    G.state = ST_COUNTDOWN;
    G.stateT = 0;
    G.camPos = Vector3Add(G.car.pos, (Vector3){ -cosf(G.car.yaw) * 6.3f, 2.15f, -sinf(G.car.yaw) * 6.3f });
    G.camTarget = G.car.pos;
}

static void Respawn(void)
{
    PlayerCar *c = &G.car;
    float s = Clamp(c->s, 4.0f, W.length - 10);
    int idx = RouteIndexAtS(s);
    float d = LaneCenterD(idx, +1, 0);
    CarReset(c, s, d);
    TrafficClearNear(s, 60, 180);
    G.invuln = 2.5f;
    G.crashed = false;
}

static void Crash(int dir, const char *what)
{
    if (G.crashed || G.invuln > 0) return;
    G.crashed = true;
    G.crashT = 0;
    G.crashDir = dir;
    G.crashSpin = dir * (2.0f + fabsf(G.car.speed) * 0.08f);
    G.crashes++;
    G.points -= 3;
    if (G.points < 0) G.points = 0;
    G.camShake = 1.2f;
    AudioPlay(SFX_CRASH);
    Vibrate(350);
    Message(TextFormat("ACCIDENT ! %s  -3 pts", what), (Color){ 255, 90, 70, 255 }, 2.4f);
}

static void UpdateRace(float dt)
{
    PlayerCar *c = &G.car;
    Input *in = &G.in;
    float throttle = in->gas ? 1.0f : 0.0f, brake = in->brake ? 1.0f : 0.0f;
    float steer = in->analog ? in->steerAnalog : (in->right ? 1.0f : 0.0f) - (in->left ? 1.0f : 0.0f);
    if (G.state != ST_RACE) { throttle = 0; brake = G.state == ST_COUNTDOWN ? 1 : 0.4f; steer = 0; }
    if (G.state == ST_FINISH) { throttle = 0; brake = 0.35f; }
    if (G.state == ST_GAMEOVER) { CarUpdate(c, dt, 0, 1, 0, false, false); return; }
    if (G.crashed) {
        G.crashT += dt;
        c->speed *= expf(-dt * 4.0f);
        c->yaw += G.crashSpin * dt;
        G.crashSpin *= expf(-dt * 2.5f);
        CarUpdate(c, dt, 0, 1, 0, false, false);
        if (G.crashT > 2.2f) {
            if (G.points <= 0) {
                G.state = ST_GAMEOVER;
                G.stateT = 0;
                AudioPlay(SFX_FINISH);
            } else Respawn();
        }
        return;
    }
    CarUpdate(c, dt, throttle, brake, steer, in->analog, G.assist);
    if (G.invuln > 0) G.invuln -= dt;
    float kmh = fabsf(c->speed) * 3.6f;
    if (kmh > G.topSpeed) G.topSpeed = kmh;

    // ---- collisions
    if (G.state == ST_RACE) {
        const RoutePt *rp = &W.route[c->seg];
        // motorway style barriers (only if the route had them)
        if (rp->roadClass >= RC_MOTORWAY) {
            float limR = rp->width * 0.5f + 2.6f + 0.6f, limL = -rp->width * 0.5f - 0.9f - 0.4f;
            if (c->d > limR || c->d < limL) {
                float roadYaw = atan2f(rp->hz, rp->hx), diff = c->yaw - roadYaw;
                float vlat = fabsf(c->speed * sinf(diff));
                if (vlat > 7) Crash(c->d > 0 ? -1 : 1, "glissière");
                else {
                    c->speed *= 0.93f;
                    c->yaw = roadYaw + diff * 0.5f;
                    float back = c->d > 0 ? c->d - limR + 0.05f : c->d - limL - 0.05f;
                    c->pos.x += rp->hz * back; c->pos.z -= rp->hx * back;
                    AudioPlay(SFX_SCRAPE);
                }
            }
        }
        Obstacle ob;
        if (!G.crashed && !c->onRoad && SceneCollide(c->pos.x, c->pos.z, 0.9f, &ob)) {
            if (fabsf(c->speed) > 4.0f) Crash(1, ob.kind == 1 ? "contre un mur" : ob.kind == 0 ? "contre un arbre" : "contre un poteau");
            else c->speed = 0;
        }
        int hit = TrafficCollide(c, G.cactus.length, G.cactus.width);
        if (hit >= 0 && !G.crashed && G.invuln <= 0) {
            TrafficOnCrash(hit);
            Crash(1, "collision");
        }
        // lost far from the road: put back on it
        if (fabsf(c->d) > 70) {
            G.offTime += dt;
            if (G.offTime > 2.5f) { Respawn(); Message("Remise sur la route", WHITE, 2); G.offTime = 0; }
        } else G.offTime = 0;
        int honk = TrafficHonking();
        if (honk) { AudioPlay(SFX_HONK_OTHER); Message("Attention : voiture en face !", (Color){ 255, 220, 80, 255 }, 1.6f); }
        int nm = TrafficNearMiss();
        if (nm) { G.nearMisses += nm; AudioPlay(SFX_WHOOSH); }
        if (in->horn) AudioPlay(SFX_HORN);
        // finish
        if (c->s >= W.length - 4) {
            G.state = ST_FINISH;
            G.stateT = 0;
            AudioPlay(SFX_FINISH);
            Vibrate(120);
            if (G.bestTime <= 0 || G.raceTime < G.bestTime) { G.bestTime = G.raceTime; G.newRecord = true; WriteSave(); }
        }
    }
}

// ------------------------------------------------------------------ HUD
static void DrawMinimap(float x, float y, float size, bool labels)
{
    float u = U();
    DrawRectangleRounded((Rectangle){ x, y, size, size }, 0.12f, 6, (Color){ 230, 238, 226, 215 });
    DrawRectangleRoundedLines((Rectangle){ x, y, size, size }, 0.12f, 6, (Color){ 40, 40, 50, 255 });
    float w = G.miniMaxX - G.miniMinX, h = G.miniMaxZ - G.miniMinZ;
    float pad = size * 0.1f;
    float sc = fminf((size - 2 * pad) / w, (size - 2 * pad) / h);
    float ox = x + (size - w * sc) / 2, oy = y + (size - h * sc) / 2;
#define MP(px, pz) (Vector2){ ox + ((px) - G.miniMinX) * sc, oy + ((pz) - G.miniMinZ) * sc }
    for (int i = 1; i < G.nMini; i++) DrawLineEx(MP(G.mini[i - 1].x, G.mini[i - 1].y), MP(G.mini[i].x, G.mini[i].y), 5 * u, WHITE);
    float prog = G.car.s / W.length;
    int done = (int)(prog * (G.nMini - 1));
    for (int i = 1; i < G.nMini; i++)
        DrawLineEx(MP(G.mini[i - 1].x, G.mini[i - 1].y), MP(G.mini[i].x, G.mini[i].y), 3 * u, i <= done ? (Color){ 30, 120, 140, 255 } : (Color){ 91, 61, 245, 255 });
    if (labels) {
        for (int i = 0; i < W.nPlaces; i++) {
            const Place *p = &W.places[i];
            if (p->x < G.miniMinX || p->x > G.miniMaxX || p->z < G.miniMinZ || p->z > G.miniMaxZ) continue;
            Vector2 q = MP(p->x, p->z);
            DrawCircleV(q, 2 * u, (Color){ 60, 64, 67, 255 });
            Text(WorldStr(p->nameId), q.x + 4 * u, q.y - 5 * u, 9 * u, (Color){ 60, 64, 67, 255 }, 0);
        }
    }
    Vector2 st = MP(W.route[0].x, W.route[0].z), en = MP(W.route[W.nRoute - 1].x, W.route[W.nRoute - 1].z);
    DrawCircleV(st, 4 * u, WHITE);
    DrawCircleLinesV(st, 4 * u, DARKGRAY);
    DrawCircleV((Vector2){ en.x, en.y - 7 * u }, 5 * u, (Color){ 234, 67, 53, 255 });
    DrawTriangle((Vector2){ en.x - 4 * u, en.y - 5 * u }, en, (Vector2){ en.x + 4 * u, en.y - 5 * u }, (Color){ 234, 67, 53, 255 });
    Vector2 me = MP(G.car.pos.x, G.car.pos.z);
    DrawCircleV(me, 5.5f * u, (Color){ 32, 110, 130, 255 });
    DrawCircleLinesV(me, 5.5f * u, WHITE);
#undef MP
}

static void DrawPedal(Rectangle r, bool on, Color col, const char *label)
{
    float u = U();
    DrawRectangleRounded((Rectangle){ r.x, r.y + 5 * u, r.width, r.height }, 0.25f, 6, (Color){ 0, 0, 0, 80 });
    Rectangle rr = on ? (Rectangle){ r.x, r.y + 3 * u, r.width, r.height } : r;
    DrawRectangleRounded(rr, 0.25f, 6, Fade(on ? ColorBrightness(col, 0.2f) : col, on ? 0.95f : 0.6f));
    for (int i = 1; i <= 3; i++) DrawRectangle((int)(rr.x + rr.width * 0.22f), (int)(rr.y + rr.height * (0.15f + i * 0.14f)), (int)(rr.width * 0.56f), (int)(3 * u), Fade(BLACK, 0.3f));
    Text(label, rr.x + rr.width / 2, rr.y + rr.height - 22 * u, 13 * u, WHITE, 1);
}

static void DrawCircleBtn(Vector2 c, float r, bool on, Color col, int icon)
{
    float u = U();
    DrawCircleV((Vector2){ c.x, c.y + 4 * u }, r, (Color){ 0, 0, 0, 80 });
    Vector2 cc = on ? (Vector2){ c.x, c.y + 2 * u } : c;
    DrawCircleV(cc, r, Fade(on ? ColorBrightness(col, 0.2f) : col, on ? 0.95f : 0.6f));
    DrawCircleLinesV(cc, r, Fade(WHITE, 0.6f));
    Color ic = WHITE;
    if (icon == 0 || icon == 1) {
        float s = icon == 0 ? -1 : 1;
        DrawTriangle((Vector2){ cc.x + s * r * 0.5f, cc.y }, (Vector2){ cc.x - s * r * 0.3f, cc.y + (s > 0 ? r * 0.45f : -r * 0.45f) },
                     (Vector2){ cc.x - s * r * 0.3f, cc.y + (s > 0 ? -r * 0.45f : r * 0.45f) }, ic);
    } else if (icon == 2) { // pause
        DrawRectangle((int)(cc.x - r * 0.35f), (int)(cc.y - r * 0.4f), (int)(r * 0.24f), (int)(r * 0.8f), ic);
        DrawRectangle((int)(cc.x + r * 0.11f), (int)(cc.y - r * 0.4f), (int)(r * 0.24f), (int)(r * 0.8f), ic);
    } else if (icon == 3) { // horn
        DrawTriangle((Vector2){ cc.x - r * 0.15f, cc.y }, (Vector2){ cc.x + r * 0.45f, cc.y + r * 0.45f }, (Vector2){ cc.x + r * 0.45f, cc.y - r * 0.45f }, ic);
        DrawRectangle((int)(cc.x - r * 0.45f), (int)(cc.y - r * 0.18f), (int)(r * 0.35f), (int)(r * 0.36f), ic);
    } else if (icon == 4) { // camera
        DrawRectangle((int)(cc.x - r * 0.45f), (int)(cc.y - r * 0.28f), (int)(r * 0.9f), (int)(r * 0.6f), ic);
        DrawCircleV((Vector2){ cc.x, cc.y + r * 0.02f }, r * 0.2f, col);
    }
}

static void DrawSpeedLimit(float x, float y, float r, int limit)
{
    DrawCircleV((Vector2){ x, y }, r, WHITE);
    DrawRing((Vector2){ x, y }, r * 0.72f, r, 0, 360, 36, (Color){ 210, 30, 30, 255 });
    Text(TextFormat("%d", limit), x, y - r * 0.5f, r * 0.95f, BLACK, 1);
}

static void DrawHUD(void)
{
    float u = U();
    int W_ = GetScreenWidth(), H_ = GetScreenHeight();
    PlayerCar *c = &G.car;
    bool racing = G.state == ST_RACE || G.state == ST_COUNTDOWN;

    // top-left: pause, chrono, record, licence points
    DrawCircleBtn((Vector2){ 30 * u, 30 * u }, 20 * u, false, (Color){ 70, 76, 92, 255 }, 2);
    float shown = G.raceTime;
    Text(FmtTime(shown), 60 * u, 12 * u, 30 * u, WHITE, 0);
    if (G.bestTime > 0) Text(TextFormat("Record %s", FmtTime(G.bestTime)), 62 * u, 44 * u, 12 * u, (Color){ 255, 236, 120, 255 }, 0);
    Rectangle card = { 12 * u, 64 * u, 104 * u, 36 * u };
    DrawRectangleRounded(card, 0.2f, 6, (Color){ 255, 196, 214, 235 });
    DrawRectangle((int)(card.x + 5 * u), (int)(card.y + 5 * u), (int)(8 * u), (int)(26 * u), (Color){ 43, 95, 217, 255 });
    Text("PERMIS", card.x + 60 * u, card.y + 3 * u, 10 * u, (Color){ 176, 48, 106, 255 }, 1);
    Text(TextFormat("%d / 12 pts", G.points), card.x + 60 * u, card.y + 15 * u, 15 * u, G.points <= 3 ? (Color){ 220, 30, 30, 255 } : (Color){ 40, 40, 50, 255 }, 1);

    // top-centre: speed, gear, current speed limit
    float kmh = fabsf(c->speed) * 3.6f;
    Text(TextFormat("%d", (int)roundf(kmh)), W_ * 0.5f + 10 * u, 6 * u, 40 * u, WHITE, 2);
    Text("km/h", W_ * 0.5f + 16 * u, 26 * u, 13 * u, WHITE, 0);
    Text(c->speed < -0.2f ? "R" : TextFormat("%d", c->gear), W_ * 0.5f + 16 * u, 10 * u, 13 * u, (Color){ 200, 220, 255, 255 }, 0);
    const RoutePt *rp = &W.route[c->seg];
    int lim = rp->speedLimit ? rp->speedLimit : (rp->roadClass >= RC_MOTORWAY ? 130 : (rp->flags & RF_BUILTUP) ? 50 : 80);
    DrawSpeedLimit(W_ * 0.5f + 78 * u, 28 * u, 17 * u, lim);

    // top-right: minimap + distance
    float ms = 118 * u;
    DrawMinimap(W_ - ms - 10 * u, 10 * u, ms, false);
    float km = fmaxf(0, (W.length - c->s) / 1000.0f);
    Text(TextFormat("%.1f km", km), W_ - ms / 2 - 10 * u, ms + 14 * u, 13 * u, WHITE, 1);
    DrawCircleBtn((Vector2){ W_ - 35 * u, 185 * u }, 20 * u, false, (Color){ 70, 76, 92, 255 }, 4);

    // zone banner
    if (G.bannerT > 0 && G.zone >= 0 && G.zone < W.nZones) {
        float a = fminf(1, fminf(G.bannerT, 3.2f - G.bannerT) * 3);
        const char *t1 = WorldStr(W.zones[G.zone].nameId), *t2 = WorldStr(W.zones[G.zone].subId);
        float tw = (G.fontOk ? MeasureTextEx(G.font, t1, 24 * u, 0.5f).x : MeasureText(t1, (int)(24 * u))) + 40 * u;
        if (tw < 220 * u) tw = 220 * u;
        Rectangle r = { W_ * 0.5f - tw / 2, 62 * u, tw, (t2[0] ? 50 : 38) * u };
        DrawRectangleRounded(r, 0.25f, 6, Fade(WHITE, 0.92f * a));
        DrawRectangleRoundedLines(r, 0.25f, 6, Fade((Color){ 210, 30, 30, 255 }, a));
        Text(t1, W_ * 0.5f, r.y + 6 * u, 24 * u, Fade((Color){ 30, 30, 40, 255 }, a), 1);
        if (t2[0]) Text(t2, W_ * 0.5f, r.y + 32 * u, 12 * u, Fade((Color){ 190, 40, 40, 255 }, a), 1);
    }
    // messages
    if (G.msgT > 0) {
        float a = fminf(1, G.msgT * 2);
        Text(G.msg, W_ * 0.5f, H_ * 0.40f, 22 * u, Fade(G.msgCol, a), 1);
    }
    // countdown traffic light
    if (G.state == ST_COUNTDOWN) {
        int n = (int)ceilf(G.countdown);
        Rectangle box = { W_ * 0.5f - 34 * u, H_ * 0.2f, 68 * u, 160 * u };
        DrawRectangleRounded(box, 0.3f, 8, (Color){ 40, 44, 56, 235 });
        Color cols[3] = { { 255, 59, 48, 255 }, { 255, 159, 28, 255 }, { 255, 210, 58, 255 } };
        for (int i = 0; i < 3; i++) {
            bool on = (i == 0 && n >= 3) || (i == 1 && n == 2) || (i == 2 && n == 1);
            DrawCircleV((Vector2){ W_ * 0.5f, box.y + 30 * u + i * 50 * u }, 19 * u, on ? cols[i] : (Color){ 70, 74, 86, 255 });
        }
        Text("Prêt ? Doigt sur GAZ !", W_ * 0.5f, H_ * 0.2f + 170 * u, 14 * u, WHITE, 1);
    }
    if (G.goT > 0) Text("PARTEZ !", W_ * 0.5f, H_ * 0.3f, 54 * u, Fade((Color){ 63, 211, 138, 255 }, fminf(1, G.goT * 2)), 1);

    // touch controls
    if (racing && G.touchUI) {
        Input *in = &G.in;
        DrawCircleBtn((Vector2){ 64 * u, H_ - 70 * u }, 44 * u, in->left, (Color){ 74, 168, 255, 255 }, 0);
        DrawCircleBtn((Vector2){ 168 * u, H_ - 70 * u }, 44 * u, in->right, (Color){ 74, 168, 255, 255 }, 1);
        DrawPedal((Rectangle){ W_ - 236 * u, H_ - 120 * u, 82 * u, 100 * u }, in->brake, (Color){ 230, 80, 70, 255 }, "FREIN");
        DrawPedal((Rectangle){ W_ - 130 * u, H_ - 150 * u, 100 * u, 130 * u }, in->gas, (Color){ 52, 190, 120, 255 }, "GAZ");
        DrawCircleBtn((Vector2){ W_ - 175 * u, H_ - 190 * u }, 24 * u, false, (Color){ 150, 110, 230, 255 }, 3);
    } else if (racing && G.raceTime < 10) {
#if defined(PLATFORM_WEB)
        Text("Flèches ou ZQSD : conduire   H : klaxon   C : caméra   P : pause", W_ * 0.5f, H_ - 26 * u, 13 * u, WHITE, 1);
#else
        Text("Flèches : conduire   H : klaxon   C : caméra   P : pause   F11 : plein écran", W_ * 0.5f, H_ - 26 * u, 13 * u, WHITE, 1);
#endif
    }
}

// ------------------------------------------------------------------ screens
static void DrawPanel(Rectangle r)
{
    float u = U();
    DrawRectangleRounded((Rectangle){ r.x, r.y + 6 * u, r.width, r.height }, 0.08f, 8, (Color){ 0, 0, 0, 100 });
    DrawRectangleRounded(r, 0.08f, 8, (Color){ 250, 246, 236, 240 });
}

static void DrawTitle(void)
{
    float u = U();
    int W_ = GetScreenWidth(), H_ = GetScreenHeight();
    Rectangle p = { W_ * 0.05f, H_ * 0.08f, fminf(W_ * 0.46f, 330 * u), H_ * 0.84f };
    DrawPanel(p);
    float cx = p.x + p.width / 2, y = p.y + 14 * u;
    Text("CARPOLO 3D", cx, y, 40 * u, (Color){ 32, 110, 130, 255 }, 1);
    y += 44 * u;
    Text("Citroën C4 Cactus · trajet réel", cx, y, 14 * u, (Color){ 90, 74, 138, 255 }, 1);
    y += 18 * u;
    Text("Cornillon-Confoux → Lançon → Salon", cx, y, 12 * u, (Color){ 90, 90, 100, 255 }, 1);
    y += 24 * u;
    if (UIButton((Rectangle){ cx - 90 * u, y, 180 * u, 44 * u }, "JOUER", (Color){ 52, 190, 120, 255 }, 24 * u)) { StartRace(); return; }
    y += 54 * u;
    const char *camNames[CAM_COUNT] = { "Vue : poursuite", "Vue : éloignée", "Vue : capot" };
    if (UIButton((Rectangle){ cx - 110 * u, y, 220 * u, 30 * u }, camNames[G.camMode], (Color){ 74, 140, 220, 255 }, 15 * u)) { G.camMode = (G.camMode + 1) % CAM_COUNT; WriteSave(); }
    y += 36 * u;
    if (UIButton((Rectangle){ cx - 110 * u, y, 220 * u, 30 * u }, G.assist ? "Aide à la direction : OUI" : "Aide à la direction : NON", (Color){ 74, 140, 220, 255 }, 15 * u)) { G.assist = !G.assist; WriteSave(); }
    y += 36 * u;
    if (UIButton((Rectangle){ cx - 110 * u, y, 105 * u, 30 * u }, G.sound ? "Son : OUI" : "Son : NON", (Color){ 74, 140, 220, 255 }, 15 * u)) { G.sound = !G.sound; AudioSetEnabled(G.sound); WriteSave(); }
    static const char *qn[3] = { "Graph. : léger", "Graph. : normal", "Graph. : élevé" };
    if (UIButton((Rectangle){ cx + 5 * u, y, 105 * u, 30 * u }, qn[G.quality], (Color){ 74, 140, 220, 255 }, 13 * u)) { G.quality = (G.quality + 1) % 3; SceneSetQuality(G.quality); RenderSetShadows(G.quality == 2); WriteSave(); }
    y += 40 * u;
    Text(G.bestTime > 0 ? TextFormat("Record : %s", FmtTime(G.bestTime)) : "Pas encore de record", cx, y, 15 * u, (Color){ 176, 48, 106, 255 }, 1);
#if !defined(PLATFORM_ANDROID)
    if (UIButton((Rectangle){ W_ - 104 * u, 8 * u, 96 * u, 26 * u }, "Quitter", (Color){ 110, 116, 130, 255 }, 13 * u)) G.quit = true;
#endif
    float ms = fminf(H_ * 0.62f, W_ - (p.x + p.width) - 40 * u);
    if (ms > 120 * u) {
        float mx = p.x + p.width + (W_ - p.x - p.width - ms) / 2, my = H_ * 0.12f;
        DrawMinimap(mx, my, ms, true);
        Text(TextFormat("Parcours réel : %.1f km", W.length / 1000.0f), mx + ms / 2, my + ms + 8 * u, 15 * u, WHITE, 1);
    }
}

static void DrawPause(void)
{
    float u = U();
    int W_ = GetScreenWidth(), H_ = GetScreenHeight();
    DrawRectangle(0, 0, W_, H_, Fade(BLACK, 0.35f));
    Rectangle p = { W_ * 0.5f - 130 * u, H_ * 0.5f - 110 * u, 260 * u, 220 * u };
    DrawPanel(p);
    Text("PAUSE", W_ * 0.5f, p.y + 14 * u, 30 * u, (Color){ 40, 40, 50, 255 }, 1);
    if (UIButton((Rectangle){ p.x + 30 * u, p.y + 58 * u, 200 * u, 40 * u }, "Reprendre", (Color){ 52, 190, 120, 255 }, 20 * u)) { G.state = G.prevState; AudioPause(false); }
    if (UIButton((Rectangle){ p.x + 30 * u, p.y + 108 * u, 200 * u, 36 * u }, "Recommencer", (Color){ 74, 140, 220, 255 }, 17 * u)) { AudioPause(false); StartRace(); }
    if (UIButton((Rectangle){ p.x + 30 * u, p.y + 154 * u, 200 * u, 36 * u }, "Menu", (Color){ 110, 116, 130, 255 }, 17 * u)) { AudioPause(false); G.state = ST_TITLE; CarReset(&G.car, 4.0f, startD()); TrafficReset(4); }
}

static void DrawResults(void)
{
    float u = U();
    int W_ = GetScreenWidth(), H_ = GetScreenHeight();
    bool over = G.state == ST_GAMEOVER;
    Rectangle p = { W_ * 0.5f - 210 * u, H_ * 0.5f - 140 * u, 420 * u, 280 * u };
    DrawPanel(p);
    Text(over ? "PERMIS ANNULÉ !" : "ARRIVÉE !", W_ * 0.5f, p.y + 12 * u, 34 * u, over ? (Color){ 220, 50, 40, 255 } : (Color){ 32, 110, 130, 255 }, 1);
    Text(over ? "Trop d'accidents... la Cactus part à la fourrière." : (G.crashes == 0 ? "Sans accident : bravo !" : "Arrivé avec quelques bosses."), W_ * 0.5f, p.y + 50 * u, 14 * u, (Color){ 70, 70, 80, 255 }, 1);
    if (!over) Text(FmtTime(G.raceTime), W_ * 0.5f, p.y + 72 * u, 44 * u, (Color){ 30, 30, 40, 255 }, 1);
    if (G.newRecord && !over) Text("NOUVEAU RECORD !", W_ * 0.5f, p.y + 118 * u, 16 * u, (Color){ 230, 80, 60, 255 }, 1);
    float avg = G.raceTime > 0 ? (G.car.odometer / G.raceTime) * 3.6f : 0;
    Text(TextFormat("Accidents : %d     Permis : %d/12", G.crashes, G.points), W_ * 0.5f, p.y + 144 * u, 15 * u, (Color){ 50, 50, 60, 255 }, 1);
    Text(TextFormat("Vitesse moyenne : %d km/h     Pointe : %d km/h", (int)avg, (int)G.topSpeed), W_ * 0.5f, p.y + 166 * u, 15 * u, (Color){ 50, 50, 60, 255 }, 1);
    Text(TextFormat("Voitures frôlées : %d", G.nearMisses), W_ * 0.5f, p.y + 188 * u, 15 * u, (Color){ 50, 50, 60, 255 }, 1);
    if (UIButton((Rectangle){ p.x + 30 * u, p.y + 220 * u, 170 * u, 42 * u }, "Rejouer", (Color){ 52, 190, 120, 255 }, 20 * u)) StartRace();
    if (UIButton((Rectangle){ p.x + 220 * u, p.y + 220 * u, 170 * u, 42 * u }, "Menu", (Color){ 110, 116, 130, 255 }, 20 * u)) { G.state = ST_TITLE; CarReset(&G.car, 4.0f, startD()); TrafficReset(4); }
}

// ------------------------------------------------------------------ main loop
static void Draw3D(void)
{
    if (RenderShadowsEnabled()) {
        Vector3 f = { cosf(G.car.yaw), 0, sinf(G.car.yaw) };
        Vector3 centre = Vector3Add(G.car.pos, Vector3Scale(f, 70.0f));
        RenderShadowBegin(centre);
        SceneDrawShadow(centre, RenderShadowRadius());
        TrafficDraw(G.cam);
        if (G.cactusReady) DrawCarModel(&G.cactus, CarMatrix(&G.car, true), G.car.wheelSpin, G.car.steerAngle, false);
        RenderShadowEnd();
    }
    BeginMode3D(G.cam);
    RenderSetFrame(G.cam, SceneQuality() == 2 ? 1.0f / 9000.0f : SceneQuality() == 1 ? 1.0f / 7000.0f : 1.0f / 5000.0f);
    SceneDraw(G.cam);
    SignsDraw(G.cam);
    TrafficDraw(G.cam);
    bool blink = G.invuln > 0 && ((int)(G.invuln * 8) % 2 == 0);
    if (G.cactusReady && !blink) {
        Matrix body = CarMatrix(&G.car, true), base = CarMatrix(&G.car, false);
        DrawBlobShadow(base, G.cactus.length, G.cactus.width);
        bool brakeL = G.in.brake && G.car.speed > 0.5f;
        if (!(G.camMode == CAM_HOOD && G.state != ST_TITLE)) DrawCarModel(&G.cactus, body, G.car.wheelSpin, G.car.steerAngle, brakeL);
        else DrawCarModel(&G.cactus, body, G.car.wheelSpin, G.car.steerAngle, brakeL);
    }
#if defined(PLATFORM_WEB) || defined(PLATFORM_ANDROID)
    // without vertex array objects (GLES2 fallback) the extra mesh attributes would stay enabled
    // and break the 2D batch ("no buffer is bound to enabled attribute")
    rlDisableVertexAttribute(4);
    rlDisableVertexAttribute(5);
#endif
    EndMode3D();
}

static float acc;
static const float STEP = 1.0f / 120.0f;
static void Frame(void);

int GameRun(void)
{
#if defined(PLATFORM_WEB)
    testMode = cp_web_flag("test") != 0;
#elif !defined(PLATFORM_ANDROID)
    testMode = getenv("CARPOLO_TEST") != NULL;
#endif
    unsigned int flags = FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE;
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB)
    flags |= FLAG_MSAA_4X_HINT;
#else
    int gl = ProbeOpenGL();
    if (gl == 0) { ShowNoOpenGLMessage(); return 1; }
    if (gl == 2) flags |= FLAG_MSAA_4X_HINT;
    else TraceLog(LOG_WARNING, "DISPLAY: 4x multisampling unavailable, antialiasing disabled");
#endif
    SetConfigFlags(flags);
#if defined(PLATFORM_WEB)
    InitWindow(cp_web_width() > 320 ? cp_web_width() : 320, cp_web_height() > 200 ? cp_web_height() : 200, "CarPolo 3D");
    if (!IsWindowReady()) { cp_web_alert("CarPolo 3D a besoin de WebGL : active l'acceleration materielle du navigateur ou essaie un autre navigateur."); return 1; }
#else
    InitWindow(1280, 720, "CarPolo 3D");
    if (!IsWindowReady()) {
        TraceLog(LOG_ERROR, "DISPLAY: cannot open an OpenGL 3.3 window (graphics driver too old?)");
        return 1;
    }
#endif
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB) && !defined(__APPLE__)
    {
        Image icon = LoadImage(AssetPath("icon.png"));
        if (icon.data) { SetWindowIcon(icon); UnloadImage(icon); }
    }
#endif
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    rlSetClipPlanes(0.25, 30000.0);     // the horizon is 20 km away (raylib's default far plane is 1 km)
    // loading screen
    BeginDrawing();
    ClearBackground((Color){ 74, 128, 196, 255 });
    DrawText("CarPolo 3D - chargement...", 40, 40, 30, WHITE);
    EndDrawing();

    AudioInit();
    LoadSave();
    AudioSetEnabled(G.sound);
    int cp[160], ncp = 0;
    for (int c = 32; c < 127; c++) cp[ncp++] = c;
    const char *extra = "éèêëàâäçôöùûüîïÉÈÊÀÇÔ’·°«»–→";
    int n = 0;
    int *more = LoadCodepoints(extra, &n);
    for (int i = 0; i < n && ncp < 160; i++) cp[ncp++] = more[i];
    UnloadCodepoints(more);
    G.font = LoadFontEx(AssetPath("font.ttf"), 64, cp, ncp);
    G.fontOk = G.font.texture.id != 0 && G.font.glyphCount > 0;
    if (G.fontOk) SetTextureFilter(G.font.texture, TEXTURE_FILTER_BILINEAR);

    if (!WorldLoad(AssetPath("world.bin"))) {
        while (!WindowShouldClose()) { BeginDrawing(); ClearBackground(BLACK); DrawText("world.bin introuvable", 40, 40, 30, RED); EndDrawing(); }
        CloseWindow();
        return 1;
    }
    RenderInit();
    CarInitShared();
    SceneSetQuality(G.quality);
    {
        char treesBin[1024];
        snprintf(treesBin, sizeof(treesBin), "%s", AssetPath("trees.bin"));
        SceneInit(treesBin, AssetPath("trees.png"));
    }
    RenderSetShadows(G.quality == 2);
    SignsInit(G.font, G.fontOk);
    G.cactus = BuildCactusModel();
    G.cactusReady = true;
    TrafficInit();
    // minimap points
    G.nMini = 0;
    G.mini = MemAlloc(sizeof(Vector2) * (W.nRoute / 8 + 2));
    G.miniMinX = G.miniMinZ = 1e9f; G.miniMaxX = G.miniMaxZ = -1e9f;
    for (int i = 0; i < W.nRoute; i += 8) {
        G.mini[G.nMini++] = (Vector2){ W.route[i].x, W.route[i].z };
        G.miniMinX = fminf(G.miniMinX, W.route[i].x); G.miniMaxX = fmaxf(G.miniMaxX, W.route[i].x);
        G.miniMinZ = fminf(G.miniMinZ, W.route[i].z); G.miniMaxZ = fmaxf(G.miniMaxZ, W.route[i].z);
    }
    G.mini[G.nMini++] = (Vector2){ W.route[W.nRoute - 1].x, W.route[W.nRoute - 1].z };

    CarReset(&G.car, 4.0f, startD());
    TrafficReset(4);
    G.state = ST_LOADING;
    G.cam = (Camera3D){ { 0, 10, 10 }, { 0, 0, 0 }, { 0, 1, 0 }, 55, CAMERA_PERSPECTIVE };
    acc = 0;

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(Frame, 0, 1);     // never returns: the browser owns the loop
#else
    while (!WindowShouldClose() && !G.quit) Frame();
#endif
    TrafficShutdown();
    UnloadCarModel(&G.cactus);
    SignsShutdown();
    SceneShutdown();
    RenderShutdown();
    WorldUnload();
    if (G.fontOk) UnloadFont(G.font);
    AudioShutdown();
    CloseWindow();
    return 0;
}

static void Frame(void)
{
    {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;
        UpdateInput();
        if (G.state == ST_LOADING) {
            SceneUpdate(G.car.pos, 40);
            float pr = SceneProgress(G.car.pos);
            UpdateCamera3D(dt);
            BeginDrawing();
            ClearBackground(FogColor());
            DrawSky(G.cam, GetScreenWidth(), GetScreenHeight());
            Draw3D();
            float u = U();
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade((Color){ 20, 40, 60, 255 }, 0.55f));
            Text("CARPOLO 3D", GetScreenWidth() / 2.0f, GetScreenHeight() * 0.35f, 44 * u, WHITE, 1);
            Text("Chargement de la Provence...", GetScreenWidth() / 2.0f, GetScreenHeight() * 0.35f + 54 * u, 16 * u, WHITE, 1);
            DrawRectangle((int)(GetScreenWidth() * 0.3f), (int)(GetScreenHeight() * 0.6f), (int)(GetScreenWidth() * 0.4f), (int)(10 * u), Fade(WHITE, 0.3f));
            DrawRectangle((int)(GetScreenWidth() * 0.3f), (int)(GetScreenHeight() * 0.6f), (int)(GetScreenWidth() * 0.4f * pr), (int)(10 * u), WHITE);
            EndDrawing();
            if (pr >= 1.0f) G.state = ST_TITLE;
            return;
        }
        if (G.in.pause) {
            if (G.state == ST_RACE || G.state == ST_COUNTDOWN) { G.prevState = G.state; G.state = ST_PAUSE; AudioPause(true); }
            else if (G.state == ST_PAUSE) { G.state = G.prevState; AudioPause(false); }
        }
        if (G.in.cam && (G.state == ST_RACE || G.state == ST_COUNTDOWN)) G.camMode = (G.camMode + 1) % CAM_COUNT;
        if (!IsWindowFocused() && (G.state == ST_RACE || G.state == ST_COUNTDOWN)) { G.prevState = G.state; G.state = ST_PAUSE; AudioPause(true); }

        if (G.state != ST_PAUSE) {
            acc += dt;
            int steps = 0;
            while (acc >= STEP && steps < 12) {
                if (G.state == ST_COUNTDOWN) {
                    float prev = G.countdown;
                    G.countdown -= STEP;
                    if ((int)ceilf(prev) != (int)ceilf(G.countdown) && G.countdown > 0) AudioPlay(SFX_BEEP);
                    if (G.countdown <= 0) { G.state = ST_RACE; G.goT = 1.2f; AudioPlay(SFX_GO); if (G.zone < 0) { G.zone = 0; G.bannerT = 3.2f; } }
                } else if (G.state == ST_RACE) G.raceTime += STEP;
                if (G.state != ST_TITLE) UpdateRace(STEP);
                if (G.state == ST_RACE || G.state == ST_COUNTDOWN || G.state == ST_FINISH) TrafficUpdate(STEP, &G.car, G.cactus.length, G.cactus.width);
                acc -= STEP;
                steps++;
            }
            if (steps >= 12) acc = 0;
            G.stateT += dt;
            if (G.msgT > 0) G.msgT -= dt;
            if (G.goT > 0) G.goT -= dt;
            if (G.bannerT > 0) G.bannerT -= dt;
            // zones
            if (G.state == ST_RACE) {
                int z = -1;
                for (int i = 0; i < W.nZones; i++) if (G.car.s >= W.zones[i].s) z = i;
                if (z != G.zone && z >= 0) { G.zone = z; G.bannerT = 3.2f; }
            }
        }
        bool running = G.state == ST_RACE || G.state == ST_COUNTDOWN || G.state == ST_FINISH;
        AudioEngine(G.car.rpm, G.in.gas && G.state == ST_RACE ? 1.0f : 0.0f, G.car.speed, running || G.state == ST_TITLE);
        AudioTyres(G.state == ST_RACE ? G.car.skid : 0, (!G.car.onRoad && G.state == ST_RACE) ? fminf(1, fabsf(G.car.speed) / 20.0f) : 0);

        SceneUpdate(G.car.pos, -1);   // background streaming
#if !defined(PLATFORM_ANDROID)
        bool shot = false;
        const char *shotName = NULL;
        if (testMode) {
            testT += dt;
            static const float tp[] = { 4640, 6440, 2560, 9050, 30 };
            switch (testStep) {
            case 0: if (testT > 1.0f) { shot = true; shotName = "t_title.png"; testStep++; testT = 0; } break;
            case 1: StartRace(); testStep++; testT = 0; break;
            case 2: if (testT > 1.5f) { shot = true; shotName = "t_countdown.png"; testStep++; testT = 0; } break;
            case 3: if (testT > 7.0f) { shot = true; shotName = "t_drive1.png"; testStep++; testT = 0; } break;
            case 4: if (testT > 6.0f) { shot = true; shotName = "t_drive2.png"; testStep++; testT = 0; } break;
            default:
                if (testStep >= 5 && testStep < 5 + 10) {
                    int k = (testStep - 5) / 2;
                    if ((testStep - 5) % 2 == 0) {
                        if (k < 5) { CarReset(&G.car, tp[k], LaneCenterD(RouteIndexAtS(tp[k]), 1, 0)); G.car.speed = 15; TrafficReset(tp[k]); G.camPos = G.car.pos; testStep++; testT = 0; }
                    } else if (testT > 3.5f) { int nl; long nv, nb; SceneStats(&nl, &nv, &nb); TraceLog(LOG_INFO, "TEST: chunks loaded %d, vertices %ld (%.1f MB GPU)", nl, nv, nb / 1e6); static char nm[32]; snprintf(nm, sizeof(nm), "t_place%d.png", k); shot = true; shotName = nm; testStep++; testT = 0; }
                } else if (testStep == 15) { CarReset(&G.car, W.length - 75, LaneCenterD(W.nRoute - 2, 1, 0)); G.car.speed = 14; testStep++; testT = 0; }
                else if (testStep == 16 && testT > 2.6f) { shot = true; shotName = "t_arrival.png"; testStep = 21; testT = 0; }
                else if (testStep == 21 && testT > 6.0f) { shot = true; shotName = "t_finish.png"; testStep = 17; testT = 0; }
                else if (testStep == 17) { G.camMode = CAM_HOOD; StartRace(); testStep++; testT = 0; }
                else if (testStep == 18 && testT > 9.0f) { shot = true; shotName = "t_hood.png"; testStep++; testT = 0; }
                else if (testStep == 19) { G.camMode = CAM_CHASE; testWrongLane = true; StartRace(); testStep = 22; testT = 0; }
                else if (testStep == 22 && (G.crashed || testT > 40)) { shot = G.crashed; shotName = "t_crash.png"; TraceLog(LOG_INFO, "TEST: wrong-lane crash=%d after %.1fs points=%d", G.crashed, testT, G.points); testStep = 23; testT = 0; }
                else if (testStep == 23 && testT > 3.0f) { shot = true; shotName = "t_respawn.png"; TraceLog(LOG_INFO, "TEST: after respawn crashed=%d invuln=%.1f", G.crashed, G.invuln); testWrongLane = false; testStep = 20; testT = 0; }
                break;
            }
            if (testStep == 20 && testT > 0.5f) {
                TraceLog(LOG_INFO, "TEST: done, crashes=%d time=%.1f", G.crashes, G.raceTime);
                G.quit = true;
#if defined(PLATFORM_WEB)
                emscripten_cancel_main_loop();
#endif
                return;
            }
        }
#endif
        UpdateCamera3D(dt);

        BeginDrawing();
        ClearBackground(FogColor());
        DrawSky(G.cam, GetScreenWidth(), GetScreenHeight());
        Draw3D();
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
        if (getenv("CARPOLO_DBGSHADOW") && RenderShadowsEnabled()) {
            Texture2D st = RenderShadowTexture();
            DrawTexturePro(st, (Rectangle){ 0, 0, (float)st.width, (float)-st.height }, (Rectangle){ 10, 90, 300, 300 }, (Vector2){ 0, 0 }, 0, WHITE);
        }
#endif
        if (G.state == ST_TITLE) DrawTitle();
        else {
            DrawHUD();
            if (G.state == ST_PAUSE) DrawPause();
            if ((G.state == ST_FINISH || G.state == ST_GAMEOVER) && G.stateT > 2.0f) DrawResults();
        }
        EndDrawing();
#if defined(PLATFORM_WEB)
        (void)shot; (void)shotName;          // no screenshots in the browser
#elif !defined(PLATFORM_ANDROID)
        if (shot && shotName) TakeScreenshot(shotName);
        if (shot && getenv("CARPOLO_DBGSHADOW") && RenderShadowsEnabled()) {
            Image im = LoadImageFromTexture(RenderShadowTexture());
            int lo = 255, hi = 0; long sum = 0;
            unsigned char *px = im.data;
            for (int i = 0; i < im.width * im.height; i++) { int r = px[i * 4]; if (r < lo) lo = r; if (r > hi) hi = r; sum += r; }
            TraceLog(LOG_INFO, "TEST: shadow map R min %d max %d mean %.1f (%dx%d, fmt %d)", lo, hi, (double)sum / (im.width * im.height), im.width, im.height, im.format);
            ExportImage(im, "shadowmap.png");
            UnloadImage(im);
        }
#endif
    }
}
