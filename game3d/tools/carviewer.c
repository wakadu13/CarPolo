// carviewer.c — turntable viewer / screenshot tool for src/car_model.c (not part of the game build).
//
// Build (from game3d/):
//   cc -O2 -std=c99 -Isrc -I/opt/raylib-desktop/include tools/carviewer.c src/car_model.c \
//      /opt/raylib-desktop/lib/libraylib.a -lm -lpthread -ldl -lGL -lX11 -o build-bin/carviewer
// Interactive : ./build-bin/carviewer                 (drag = orbit, wheel = zoom, 1..6 = vehicle)
// Screenshots : xvfb-run -a -s "-screen 0 1280x720x24" ./build-bin/carviewer --shots /some/dir
//
// The lighting shader below is the reference for the game: sun from the south-west, hemispherical
// sky/ground ambient, Blinn-Phong specular + fresnel sky reflection driven by GetCarMatParams(kind),
// emissive lights, exponential fog. Written once, prefixed for GLSL 330 (desktop) or GLSL 100 (GLES2).
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "car_model.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *VS_BODY =
    "ATTR vec3 vertexPosition;\n"
    "ATTR vec2 vertexTexCoord;\n"
    "ATTR vec3 vertexNormal;\n"
    "ATTR vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "VARY vec3 fragPosition;\n"
    "VARY vec2 fragTexCoord;\n"
    "VARY vec4 fragColor;\n"
    "VARY vec3 fragNormal;\n"
    "void main() {\n"
    "  fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));\n"
    "  fragTexCoord = vertexTexCoord;\n"
    "  fragColor = vertexColor;\n"
    "  fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));\n"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *FS_BODY =
    "VARY vec3 fragPosition;\n"
    "VARY vec2 fragTexCoord;\n"
    "VARY vec4 fragColor;\n"
    "VARY vec3 fragNormal;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 viewPos;\n"
    "uniform vec3 sunDir;\n"       // direction TO the sun
    "uniform vec3 sunColor;\n"
    "uniform vec3 skyZenith;\n"
    "uniform vec3 skyHorizon;\n"
    "uniform vec3 groundColor;\n"
    "uniform vec4 matParams;\n"    // spec, shininess, refl, reflMin
    "uniform float emissive;\n"
    "uniform vec3 fogColor;\n"
    "uniform float fogDensity;\n"
    "vec3 skyEnv(vec3 d) {\n"
    "  float t = d.y;\n"
    "  vec3 up = mix(skyHorizon, skyZenith, smoothstep(0.0, 0.55, t));\n"
    "  vec3 dn = mix(skyHorizon * 0.85, groundColor, smoothstep(0.0, 0.18, -t));\n"
    "  vec3 c = t > 0.0 ? up : dn;\n"
    "  float sunSpot = pow(max(dot(d, sunDir), 0.0), 200.0);\n"
    "  return c + sunColor * sunSpot * 2.0;\n"
    "}\n"
    "void main() {\n"
    "  vec4 albedo = TEX(texture0, fragTexCoord) * colDiffuse * fragColor;\n"
    "  vec3 N = normalize(fragNormal);\n"
    "  vec3 V = normalize(viewPos - fragPosition);\n"
    "  float ndl = max(dot(N, sunDir), 0.0);\n"
    "  vec3 amb = mix(groundColor, skyZenith, 0.5 + 0.5 * N.y) * 0.62 + skyHorizon * 0.12;\n"
    "  vec3 col = albedo.rgb * (amb + sunColor * ndl);\n"
    "  vec3 H = normalize(sunDir + V);\n"
    "  float spec = ndl > 0.0 ? pow(max(dot(N, H), 0.0), matParams.y) * matParams.x : 0.0;\n"
    "  float fres = matParams.w + (1.0 - matParams.w) * pow(1.0 - max(dot(N, V), 0.0), 5.0);\n"
    "  vec3 env = skyEnv(reflect(-V, N));\n"
    "  col = mix(col, env, clamp(matParams.z * fres, 0.0, 1.0));\n"
    "  col += sunColor * spec;\n"
    "  col += albedo.rgb * emissive;\n"
    "  float d = length(viewPos - fragPosition);\n"
    "  col = mix(col, fogColor, 1.0 - exp(-d * fogDensity));\n"
    "  FRAG = vec4(col, albedo.a);\n"
    "}\n";

static char vsBuf[8192], fsBuf[8192];

static Shader LoadLitShader(void)
{
#if defined(GRAPHICS_API_OPENGL_ES2) || defined(PLATFORM_ANDROID)
    const char *pv = "#version 100\n#define ATTR attribute\n#define VARY varying\n";
    const char *pf = "#version 100\nprecision mediump float;\n#define VARY varying\n#define TEX texture2D\n#define FRAG gl_FragColor\n";
#else
    const char *pv = "#version 330\n#define ATTR in\n#define VARY out\n";
    const char *pf = "#version 330\n#define VARY in\n#define TEX texture\nout vec4 finalColor;\n#define FRAG finalColor\n";
#endif
    snprintf(vsBuf, sizeof(vsBuf), "%s%s", pv, VS_BODY);
    snprintf(fsBuf, sizeof(fsBuf), "%s%s", pf, FS_BODY);
    return LoadShaderFromMemory(vsBuf, fsBuf);
}

typedef struct {
    Shader sh;
    int locView, locSun, locSunCol, locZen, locHor, locGround, locMat, locEmi, locFog, locFogD;
} Lit;

static Lit lit;

static void LitSetMaterial(int kind)
{
    CarMatParams p = GetCarMatParams(kind);
    float mp[4] = { p.spec, p.shininess, p.refl, p.reflMin };
    SetShaderValue(lit.sh, lit.locMat, mp, SHADER_UNIFORM_VEC4);
    SetShaderValue(lit.sh, lit.locEmi, &p.emissive, SHADER_UNIFORM_FLOAT);
}

static void DrawModelLit(Model m, const int *kinds, Matrix xf)
{
    for (int i = 0; i < m.meshCount; i++) {
        LitSetMaterial(kinds[i]);
        Material mat = m.materials[m.meshMaterial[i]];
        mat.shader = lit.sh;
        DrawMesh(m.meshes[i], mat, MatrixMultiply(m.transform, xf));
    }
}

static void DrawCar(const CarModel *c, Matrix xf, float spin, float steer)
{
    DrawModelLit(c->body, c->bodyMatKind, xf);
    for (int w = 0; w < 4; w++) {
        int right = (w == 1 || w == 3);
        Matrix m = MatrixRotateX(right ? -spin : spin);
        if (right) m = MatrixMultiply(m, MatrixRotateY(PI));
        if (w < 2) m = MatrixMultiply(m, MatrixRotateY(steer));
        m = MatrixMultiply(m, MatrixTranslate(c->wheelPos[w].x, c->wheelPos[w].y, c->wheelPos[w].z));
        DrawModelLit(c->wheel, c->wheelMatKind, MatrixMultiply(m, xf));
    }
}

static Texture2D shadowTex;
static Model shadowQuad;
static Model groundModel;

static void DrawShadow(const CarModel *c, Matrix xf)
{
    Matrix s = MatrixScale(c->width * 1.25f, 1.0f, c->length * 1.12f);
    s = MatrixMultiply(s, MatrixTranslate(0, 0.012f, 0));
    rlDisableDepthMask();
    DrawMesh(shadowQuad.meshes[0], shadowQuad.materials[0], MatrixMultiply(s, xf));
    rlEnableDepthMask();
}

#define NCARS 6
static CarModel cars[NCARS];
static const char *carNames[NCARS] = { "Cactus", "hatchback", "SUV", "van", "truck", "city car" };

static void SetupLights(Camera3D cam)
{
    Vector3 sun = Vector3Normalize((Vector3){ -0.55f, 0.62f, 0.56f });  // from the south-west (x east, z south)
    float sunCol[3] = { 1.05f, 0.98f, 0.88f };
    float zen[3] = { 0.36f, 0.56f, 0.86f }, hor[3] = { 0.78f, 0.84f, 0.90f }, gnd[3] = { 0.36f, 0.33f, 0.28f };
    float fog[3] = { 0.78f, 0.83f, 0.88f }, fogD = 0.004f;
    SetShaderValue(lit.sh, lit.locView, &cam.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locSun, &sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locSunCol, sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locZen, zen, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locHor, hor, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locGround, gnd, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locFog, fog, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit.sh, lit.locFogD, &fogD, SHADER_UNIFORM_FLOAT);
}

static void DrawScene(Camera3D cam, int sel, int lineup, float spin)
{
    BeginDrawing();
    ClearBackground((Color){ 200, 214, 228, 255 });
    DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight() / 2 + 40, (Color){ 92, 143, 214, 255 }, (Color){ 200, 214, 228, 255 });
    BeginMode3D(cam);
    SetupLights(cam);
    int gk = CARMAT_PLASTIC;
    DrawModelLit(groundModel, &gk, MatrixIdentity());
    if (!lineup) {
        DrawShadow(&cars[sel], MatrixIdentity());
        DrawCar(&cars[sel], MatrixIdentity(), spin, 0.0f);
    } else {
        float xs[NCARS] = { 0.0f, 3.2f, 6.6f, 10.5f, -9.0f, -3.0f };
        float zs[NCARS] = { 0.0f, 0.0f, 0.0f, 0.0f, -4.0f, 0.0f };
        for (int i = 0; i < NCARS; i++) {
            Matrix xf = MatrixTranslate(xs[i], 0, zs[i]);
            DrawShadow(&cars[i], xf);
            DrawCar(&cars[i], xf, spin, 0.0f);
        }
    }
    EndMode3D();
}

static void Shot(const char *dir, const char *name, Camera3D cam, int sel, int lineup)
{
    for (int f = 0; f < 3; f++) {   // a few frames so the swap chain settles
        DrawScene(cam, sel, lineup, 0.3f);
        if (f == 2) {
            Image img = LoadImageFromScreen();
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.png", dir, name);
            ExportImage(img, path);
            UnloadImage(img);
        }
        EndDrawing();
    }
}

int main(int argc, char **argv)
{
    const char *shotDir = NULL;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--shots") && i + 1 < argc) shotDir = argv[++i];
    SetConfigFlags(FLAG_MSAA_4X_HINT | (shotDir ? 0 : FLAG_VSYNC_HINT));
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "CarPolo 3D — car viewer");

    lit.sh = LoadLitShader();
    lit.locView = GetShaderLocation(lit.sh, "viewPos");
    lit.locSun = GetShaderLocation(lit.sh, "sunDir");
    lit.locSunCol = GetShaderLocation(lit.sh, "sunColor");
    lit.locZen = GetShaderLocation(lit.sh, "skyZenith");
    lit.locHor = GetShaderLocation(lit.sh, "skyHorizon");
    lit.locGround = GetShaderLocation(lit.sh, "groundColor");
    lit.locMat = GetShaderLocation(lit.sh, "matParams");
    lit.locEmi = GetShaderLocation(lit.sh, "emissive");
    lit.locFog = GetShaderLocation(lit.sh, "fogColor");
    lit.locFogD = GetShaderLocation(lit.sh, "fogDensity");

    double t0 = GetTime();
    cars[0] = BuildCactusModel();
    double t1 = GetTime();
    cars[1] = BuildTrafficCarModel(0, (Color){ 178, 28, 32, 255 });
    cars[2] = BuildTrafficCarModel(1, (Color){ 120, 124, 130, 255 });
    cars[3] = BuildTrafficCarModel(2, (Color){ 238, 238, 236, 255 });
    cars[4] = BuildTrafficCarModel(3, (Color){ 30, 70, 150, 255 });
    cars[5] = BuildTrafficCarModel(4, (Color){ 235, 200, 40, 255 });
    double t2 = GetTime();
    printf("build: cactus %.1f ms, traffic %.1f ms\n", (t1 - t0) * 1000, (t2 - t1) * 1000);
    for (int i = 0; i < NCARS; i++) {
        printf("%-10s tris %5d  body meshes:", carNames[i], CarModelTriangleCount(&cars[i]));
        for (int k = 0; k < cars[i].body.meshCount; k++) printf(" %s(%d)", CarMatKindName(cars[i].bodyMatKind[k]), cars[i].body.meshes[k].triangleCount);
        printf(" | wheel:");
        for (int k = 0; k < cars[i].wheel.meshCount; k++) printf(" %s(%d)", CarMatKindName(cars[i].wheelMatKind[k]), cars[i].wheel.meshes[k].triangleCount);
        printf("\n");
    }

    Image sh = GenImageGradientRadial(256, 256, 0.15f, (Color){ 0, 0, 0, 175 }, (Color){ 0, 0, 0, 0 });
    shadowTex = LoadTextureFromImage(sh);
    UnloadImage(sh);
    shadowQuad = LoadModelFromMesh(GenMeshPlane(1, 1, 1, 1));
    shadowQuad.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = shadowTex;
    Mesh gm = GenMeshPlane(80, 80, 8, 8);
    gm.colors = (unsigned char *)MemAlloc(gm.vertexCount * 4);
    for (int i = 0; i < gm.vertexCount; i++) { gm.colors[i * 4] = 150; gm.colors[i * 4 + 1] = 142; gm.colors[i * 4 + 2] = 128; gm.colors[i * 4 + 3] = 255; }
    UnloadMesh(gm);
    gm = GenMeshPlane(80, 80, 8, 8);
    groundModel = LoadModelFromMesh(gm);
    groundModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 168, 160, 146, 255 };

    if (shotDir) {
        struct { const char *name; Vector3 pos, target; float fovy; int sel, lineup; } shots[] = {
            { "cactus_front34", { -3.7f, 1.45f, 4.1f }, { 0, 0.68f, 0.15f }, 34, 0, 0 },
            { "cactus_rear34", { 3.9f, 1.75f, -4.0f }, { 0, 0.72f, -0.1f }, 34, 0, 0 },
            { "cactus_side_right", { -7.2f, 0.85f, 0.0f }, { 0, 0.72f, 0 }, 30, 0, 0 },
            { "cactus_side_left", { 7.2f, 0.85f, 0.0f }, { 0, 0.72f, 0 }, 30, 0, 0 },
            { "cactus_front", { 0, 0.95f, 7.5f }, { 0, 0.72f, 0 }, 22, 0, 0 },
            { "cactus_rear", { 0, 1.05f, -7.5f }, { 0, 0.75f, 0 }, 22, 0, 0 },
            { "cactus_top", { 0.001f, 9.5f, 0.0f }, { 0, 0, 0 }, 30, 0, 0 },
            { "cactus_wheel", { -1.75f, 0.45f, 2.05f }, { -0.74f, 0.33f, 1.245f }, 34, 0, 0 },
            { "cactus_nose", { -1.4f, 1.05f, 3.7f }, { 0, 0.72f, 1.8f }, 36, 0, 0 },
            { "cactus_low_rear", { 2.2f, 0.9f, -3.6f }, { 0.2f, 0.85f, -1.6f }, 40, 0, 0 },
            { "lineup", { -4.5f, 4.0f, 17.0f }, { 1.0f, 0.9f, 0.0f }, 42, 0, 1 },
            { "lineup_rear", { 8.0f, 4.0f, -17.0f }, { 1.0f, 0.9f, -1.0f }, 42, 0, 1 },
            { "truck", { -9.0f, 3.5f, 12.0f }, { 0, 1.8f, 0 }, 45, 4, 0 },
        };
        int n = (int)(sizeof(shots) / sizeof(shots[0]));
        for (int i = 0; i < n; i++) {
            Camera3D cam = { shots[i].pos, shots[i].target, { 0, 1, 0 }, shots[i].fovy, CAMERA_PERSPECTIVE };
            if (shots[i].pos.y > 9.0f) cam.up = (Vector3){ 0, 0, 1 };
            Shot(shotDir, shots[i].name, cam, shots[i].sel, shots[i].lineup);
        }
        printf("screenshots written to %s\n", shotDir);
    } else {
        float yaw = 0.8f, pitch = 0.25f, dist = 7.5f, spin = 0;
        int sel = 0, lineup = 0;
        while (!WindowShouldClose()) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { Vector2 d = GetMouseDelta(); yaw -= d.x * 0.01f; pitch = Clamp(pitch + d.y * 0.01f, -0.1f, 1.4f); }
            dist = Clamp(dist - GetMouseWheelMove() * 0.5f, 3.0f, 40.0f);
            for (int k = 0; k < NCARS; k++) if (IsKeyPressed(KEY_ONE + k)) { sel = k; lineup = 0; }
            if (IsKeyPressed(KEY_L)) lineup = !lineup;
            spin += GetFrameTime() * 4.0f;
            Camera3D cam = { { sinf(yaw) * cosf(pitch) * dist, 0.7f + sinf(pitch) * dist, cosf(yaw) * cosf(pitch) * dist }, { 0, 0.7f, 0 }, { 0, 1, 0 }, 35, CAMERA_PERSPECTIVE };
            DrawScene(cam, sel, lineup, spin);
            DrawText(TextFormat("%s  —  1..6 vehicle, L lineup, drag to orbit", lineup ? "lineup" : carNames[sel]), 12, 12, 20, BLACK);
            DrawFPS(12, 40);
            EndDrawing();
        }
    }
    for (int i = 0; i < NCARS; i++) UnloadCarModel(&cars[i]);
    UnloadTexture(shadowTex);
    CloseWindow();
    return 0;
}
