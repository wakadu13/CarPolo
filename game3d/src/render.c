// Lighting shaders (GLSL 100 / 330), sky, and CPU mesh building helpers.
#include "render.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

Shader gLit, gLitCut, gDepth, gDepthCut;
MatParams MAT_MATTE   = { 0.05f, 8.0f, 0.00f, 0.0f };
MatParams MAT_ROAD    = { 0.10f, 20.0f, 0.04f, 0.0f };
MatParams MAT_PAINT   = { 0.75f, 70.0f, 0.38f, 0.0f };
MatParams MAT_GLASS   = { 1.20f, 140.0f, 0.70f, 0.0f };
MatParams MAT_CHROME  = { 1.20f, 90.0f, 0.85f, 0.05f };
MatParams MAT_PLASTIC = { 0.15f, 20.0f, 0.06f, 0.0f };
MatParams MAT_LIGHT   = { 0.80f, 80.0f, 0.30f, 0.55f };
MatParams MAT_RUBBER  = { 0.04f, 6.0f, 0.00f, 0.0f };

typedef struct { Shader sh; int view, fogCol, fogDen, mat, reflMin, rise, lightVP, shadowOn, shadowTexel; } LitShader;
static LitShader L[2];
static RenderTexture2D shadowRT;
static bool shadowsOn, inShadowPass;
static int shadowSize;
static const float SHADOW_EXTENT = 340.0f;      // metres covered by the shadow map
static Vector3 sunDir;
static Color fogColor = { 196, 209, 224, 255 };

#if defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB) || defined(GRAPHICS_API_OPENGL_ES2)
#define VS_PREFIX "#version 100\n"
#define FS_PREFIX "#version 100\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
#else
#define VS_PREFIX "#version 330\n#define attribute in\n#define varying out\n"
#define FS_PREFIX "#version 330\n#define varying in\n#define texture2D texture\nout vec4 fragOut;\n#define gl_FragColor fragOut\n"
#endif

static const char *VS_BODY =
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "attribute vec2 vertexTexCoord2;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "uniform vec3 viewPos;\n"
    "uniform float rise;\n"
    "uniform mat4 lightVP;\n"
    "varying vec4 vShadow;\n"
    "varying vec3 vView;\n"
    "varying vec3 vNormal;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vUV;\n"
    "varying vec2 vUV2;\n"
    "void main() {\n"
    "  vec3 lp = vertexPosition;\n"
    "  lp.y -= (1.0 - rise) * 30.0;\n"
    "  vec4 wp = matModel * vec4(lp, 1.0);\n"
    "  vView = viewPos - wp.xyz;\n"
    "  vShadow = lightVP * wp;\n"
    "  vNormal = (matNormal * vec4(vertexNormal, 0.0)).xyz;\n"
    "  vColor = vertexColor;\n"
    "  vUV = vertexTexCoord;\n"
    "  vUV2 = vertexTexCoord2;\n"
    "  gl_Position = mvp * vec4(lp, 1.0);\n"
    "}\n";

static const char *FS_BODY =
    "varying vec4 vShadow;\n"
    "uniform sampler2D shadowMap;\n"
    "uniform float shadowOn;\n"
    "uniform float shadowTexel;\n"
    "float shadowTap(vec2 uv, float d) {\n"
    "  float sd = dot(texture2D(shadowMap, uv).rgb, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0));\n"
    "  return sd < d ? 0.0 : 1.0;\n"
    "}\n"
    "float shadowFactor(float ndl) {\n"
    "  if (shadowOn < 0.5) return 1.0;\n"
    "  vec3 sc = vShadow.xyz / vShadow.w * 0.5 + 0.5;\n"
    "  vec2 e = abs(sc.xy - 0.5) * 2.0;\n"
    "  float edge = 1.0 - smoothstep(0.75, 1.0, max(e.x, e.y));\n"
    "  if (edge <= 0.0 || sc.z >= 1.0) return 1.0;\n"
    "#ifdef FOLIAGE\n"
    "  float d = sc.z - 0.0022;\n"
    "#else\n"
    "  float d = sc.z - (0.0006 + 0.0016 * (1.0 - ndl));\n"
    "#endif\n"
    "#ifdef PCF\n"
    "  float t = shadowTexel * 0.6;\n"
    "  float s = (shadowTap(sc.xy + vec2(-t, -t), d) + shadowTap(sc.xy + vec2(t, -t), d) + shadowTap(sc.xy + vec2(-t, t), d) + shadowTap(sc.xy + vec2(t, t), d)) * 0.25;\n"
    "#else\n"
    "  float s = shadowTap(sc.xy, d);\n"
    "#endif\n"
    "  return mix(1.0, s, edge);\n"
    "}\n"
    "varying vec3 vView;\n"
    "varying vec3 vNormal;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vUV;\n"
    "varying vec2 vUV2;\n"
    "uniform sampler2D texture0;\n"
    "#ifndef FOLIAGE\n"
    "uniform sampler2D texture1;\n"
    "uniform sampler2D texture2;\n"
    "uniform sampler2D texture3;\n"
    "#endif\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 sunDir;\n"
    "uniform vec3 sunColor;\n"
    "uniform vec3 skyColor;\n"
    "uniform vec3 groundColor;\n"
    "uniform vec3 fogColor;\n"
    "uniform float fogDensity;\n"
    "uniform vec4 matParams;\n"
    "uniform float reflMin;\n"
    "void main() {\n"
    "#ifdef FOLIAGE\n"
    "  vec4 t = texture2D(texture0, vUV);\n"
    "  if (t.a < 0.5) discard;\n"
    "#else\n"
    "  float m = vUV2.x;\n"
    "  vec4 w = max(vec4(1.0) - abs(vec4(m) - vec4(0.0, 1.0, 2.0, 3.0)), vec4(0.0));\n"
    "  vec4 t = texture2D(texture0, vUV) * w.x + texture2D(texture1, vUV) * w.y + texture2D(texture2, vUV) * w.z + texture2D(texture3, vUV) * w.w;\n"
    "#endif\n"
    "  vec4 albedo = vec4(vColor.rgb * mix(t.rgb, vec3(1.0), vUV2.y), vColor.a) * colDiffuse;\n"
    "  vec3 n = normalize(vNormal);\n"
    "  float dist = length(vView);\n"
    "  vec3 v = vView / max(dist, 0.001);\n"
    "#ifdef FOLIAGE\n"
    "  // leaf cards: normals come from the crown shape; light passes through thin foliage\n"
    "  float ndl = max(dot(n, sunDir), 0.0) * 0.85 + max(-dot(n, sunDir), 0.0) * 0.30 + 0.12;\n"
    "  float sh = shadowFactor(1.0);\n"
    "  vec3 amb = mix(groundColor, skyColor, n.y * 0.5 + 0.5) * (1.0 + 0.15 * sh);\n"
    "  vec3 col = albedo.rgb * (amb + sunColor * ndl * (0.15 + 0.85 * sh));\n"
    "#else\n"
    "  if (!gl_FrontFacing) n = -n;\n"
    "  float ndl = max(dot(n, sunDir), 0.0);\n"
    "  float sh = shadowFactor(ndl);\n"
    "  vec3 amb = mix(groundColor, skyColor, n.y * 0.5 + 0.5);\n"
    "  vec3 col = albedo.rgb * (amb + sunColor * ndl * sh);\n"
    "  vec3 h = normalize(sunDir + v);\n"
    "  float spec = pow(max(dot(n, h), 0.0), matParams.y) * matParams.x * step(0.001, ndl) * sh;\n"
    "  float fres = mix(reflMin, 1.0, pow(1.0 - max(dot(n, v), 0.0), 4.0));\n"
    "  vec3 r = reflect(-v, n);\n"
    "  vec3 env = mix(groundColor * 0.9, skyColor * 1.25, smoothstep(-0.15, 0.35, r.y));\n"
    "  col = mix(col, env, clamp(fres * matParams.z, 0.0, 1.0));\n"
    "  col += sunColor * spec + albedo.rgb * matParams.w;\n"
    "#endif\n"
    "  // aerial perspective: exponential haze, brighter and warmer towards the sun\n"
    "  float fog = 1.0 - exp(-dist * fogDensity);\n"
    "  float sunAmt = pow(max(dot(-v, sunDir), 0.0), 8.0);\n"
    "  vec3 fogC = fogColor + vec3(0.22, 0.15, 0.05) * sunAmt;\n"
    "  gl_FragColor = vec4(mix(col, fogC, fog), albedo.a);\n"
    "}\n";

static const char *DEPTH_VS =
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "uniform mat4 mvp;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = vertexTexCoord; gl_Position = mvp * vec4(vertexPosition, 1.0); }\n";
static const char *DEPTH_FS =
    "varying vec2 vUV;\n"
    "uniform sampler2D texture0;\n"
    "void main() {\n"
    "#ifdef FOLIAGE\n"
    "  if (texture2D(texture0, vUV).a < 0.5) discard;\n"
    "#endif\n"
    "  vec3 enc = fract(vec3(1.0, 255.0, 65025.0) * gl_FragCoord.z);\n"
    "  enc -= enc.yzz * vec3(1.0 / 255.0, 1.0 / 255.0, 0.0);\n"
    "  gl_FragColor = vec4(enc, 1.0);\n"
    "}\n";

static Shader loadDepth(const char *defines)
{
    char *vs = MemAlloc(4096), *fs = MemAlloc(4096);
    strcpy(vs, VS_PREFIX); strcat(vs, defines); strcat(vs, DEPTH_VS);
    strcpy(fs, FS_PREFIX); strcat(fs, defines); strcat(fs, DEPTH_FS);
    Shader sh = LoadShaderFromMemory(vs, fs);
    MemFree(vs); MemFree(fs);
    sh.locs[SHADER_LOC_MAP_ALBEDO] = GetShaderLocation(sh, "texture0");
    return sh;
}

static LitShader loadLit(const char *defines)
{
    char *vs = MemAlloc(8192), *fs = MemAlloc(8192);
    strcpy(vs, VS_PREFIX); strcat(vs, defines); strcat(vs, VS_BODY);
    strcpy(fs, FS_PREFIX); strcat(fs, defines); strcat(fs, FS_BODY);
    LitShader s;
    s.sh = LoadShaderFromMemory(vs, fs);
    MemFree(vs); MemFree(fs);
    s.sh.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(s.sh, "matModel");
    s.sh.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(s.sh, "matNormal");
    s.sh.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(s.sh, "viewPos");
    s.sh.locs[SHADER_LOC_VERTEX_TEXCOORD02] = GetShaderLocationAttrib(s.sh, "vertexTexCoord2");
    s.sh.locs[SHADER_LOC_MAP_ALBEDO] = GetShaderLocation(s.sh, "texture0");
    s.sh.locs[SHADER_LOC_MAP_METALNESS] = GetShaderLocation(s.sh, "texture1");
    s.sh.locs[SHADER_LOC_MAP_NORMAL] = GetShaderLocation(s.sh, "texture2");
    s.sh.locs[SHADER_LOC_MAP_ROUGHNESS] = GetShaderLocation(s.sh, "texture3");
    s.view = s.sh.locs[SHADER_LOC_VECTOR_VIEW];
    s.fogCol = GetShaderLocation(s.sh, "fogColor");
    s.fogDen = GetShaderLocation(s.sh, "fogDensity");
    s.mat = GetShaderLocation(s.sh, "matParams");
    s.reflMin = GetShaderLocation(s.sh, "reflMin");
    s.rise = GetShaderLocation(s.sh, "rise");
    s.lightVP = GetShaderLocation(s.sh, "lightVP");
    s.shadowOn = GetShaderLocation(s.sh, "shadowOn");
    s.shadowTexel = GetShaderLocation(s.sh, "shadowTexel");
    int unit = 7;
    SetShaderValue(s.sh, GetShaderLocation(s.sh, "shadowMap"), &unit, SHADER_UNIFORM_INT);
    float zero = 0.0f;
    SetShaderValue(s.sh, s.shadowOn, &zero, SHADER_UNIFORM_FLOAT);
    Matrix id = MatrixIdentity();
    SetShaderValueMatrix(s.sh, s.lightVP, id);

    // Late-afternoon autumn sun from the south-west (+Z = south, -X = west)
    sunDir = Vector3Normalize((Vector3){ -0.55f, 0.62f, 0.56f });
    float sunCol[3] = { 0.95f, 0.86f, 0.72f };
    float sky[3] = { 0.42f, 0.49f, 0.60f };
    float ground[3] = { 0.30f, 0.27f, 0.22f };
    float fog[3] = { fogColor.r / 255.f, fogColor.g / 255.f, fogColor.b / 255.f };
    SetShaderValue(s.sh, GetShaderLocation(s.sh, "sunDir"), &sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(s.sh, GetShaderLocation(s.sh, "sunColor"), sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(s.sh, GetShaderLocation(s.sh, "skyColor"), sky, SHADER_UNIFORM_VEC3);
    SetShaderValue(s.sh, GetShaderLocation(s.sh, "groundColor"), ground, SHADER_UNIFORM_VEC3);
    SetShaderValue(s.sh, s.fogCol, fog, SHADER_UNIFORM_VEC3);
    float one = 1.0f;
    SetShaderValue(s.sh, s.rise, &one, SHADER_UNIFORM_FLOAT);
    return s;
}

void RenderInit(void)
{
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB)
    const char *pcf = "";
    shadowSize = 1024;
#else
    const char *pcf = "#define PCF\n";
    shadowSize = 2048;
#endif
    char d0[64], d1[64];
    snprintf(d0, sizeof(d0), "%s", pcf);
    snprintf(d1, sizeof(d1), "#define FOLIAGE\n%s", pcf);
    L[0] = loadLit(d0);
    L[1] = loadLit(d1);
    gLit = L[0].sh;
    gLitCut = L[1].sh;
    gDepth = loadDepth("");
    gDepthCut = loadDepth("#define FOLIAGE\n");
    RenderSetMat(MAT_MATTE);
    RenderSetFog(1.0f / 9000.0f);
}

void RenderShutdown(void)
{
    UnloadShader(gLit); UnloadShader(gLitCut); UnloadShader(gDepth); UnloadShader(gDepthCut);
    if (shadowRT.id) UnloadRenderTexture(shadowRT);
    shadowRT = (RenderTexture2D){ 0 };
}

// ---------------------------------------------------------------- shadows
void RenderSetShadows(bool on)
{
    if (on && !shadowRT.id) {
        shadowRT = LoadRenderTexture(shadowSize, shadowSize);
        if (!shadowRT.id) { TraceLog(LOG_WARNING, "SHADOW: render texture unavailable, shadows off"); on = false; }
        else {
            SetTextureFilter(shadowRT.texture, TEXTURE_FILTER_POINT);
            SetTextureWrap(shadowRT.texture, TEXTURE_WRAP_CLAMP);
            float texel = 1.0f / shadowSize;
            for (int i = 0; i < 2; i++) SetShaderValue(L[i].sh, L[i].shadowTexel, &texel, SHADER_UNIFORM_FLOAT);
        }
    }
    shadowsOn = on;
    float v = on ? 1.0f : 0.0f;
    for (int i = 0; i < 2; i++) SetShaderValue(L[i].sh, L[i].shadowOn, &v, SHADER_UNIFORM_FLOAT);
}

bool RenderShadowsEnabled(void) { return shadowsOn && shadowRT.id; }
float RenderShadowRadius(void) { return SHADOW_EXTENT * 0.72f; }
Texture2D RenderShadowTexture(void) { return shadowRT.texture; }
Shader RenderLitShader(void) { return inShadowPass ? gDepth : gLit; }

Camera3D RenderShadowBegin(Vector3 centre)
{
    Camera3D lc = { 0 };
    lc.position = Vector3Add(centre, Vector3Scale(sunDir, 600.0f));   // from the sun towards the centre
    lc.target = centre;
    lc.up = (Vector3){ 0, 1, 0 };
    lc.fovy = SHADOW_EXTENT;
    lc.projection = CAMERA_ORTHOGRAPHIC;
    BeginTextureMode(shadowRT);
    ClearBackground(WHITE);                 // depth 1 = nothing
    rlSetClipPlanes(1.0, 1300.0);
    BeginMode3D(lc);
    // the lit shaders will need the same view-projection
    Matrix view = MatrixLookAt(lc.position, lc.target, lc.up);
    float top = SHADOW_EXTENT * 0.5f;
    Matrix proj = MatrixOrtho(-top, top, -top, top, 1.0, 1300.0);
    Matrix vp = MatrixMultiply(view, proj);
    for (int i = 0; i < 2; i++) SetShaderValueMatrix(L[i].sh, L[i].lightVP, vp);
    inShadowPass = true;
    return lc;
}

void RenderShadowEnd(void)
{
    EndMode3D();
    EndTextureMode();
    rlSetClipPlanes(0.25, 30000.0);
    inShadowPass = false;
    rlActiveTextureSlot(7);
    rlEnableTexture(shadowRT.texture.id);
    rlActiveTextureSlot(0);
}

void RenderSetFrame(Camera3D cam, float fogDensity)
{
    for (int i = 0; i < 2; i++) SetShaderValue(L[i].sh, L[i].view, &cam.position, SHADER_UNIFORM_VEC3);
    RenderSetFog(fogDensity);
}

void RenderSetFog(float density)
{
    for (int i = 0; i < 2; i++) SetShaderValue(L[i].sh, L[i].fogDen, &density, SHADER_UNIFORM_FLOAT);
}

void RenderSetRise(float k)
{
    for (int i = 0; i < 2; i++) SetShaderValue(L[i].sh, L[i].rise, &k, SHADER_UNIFORM_FLOAT);
}

void RenderSetMat(MatParams m) { RenderSetMatEx(m.spec, m.shininess, m.reflect, 0.12f, m.emissive); }

void RenderSetMatEx(float spec, float shininess, float refl, float reflMin, float emissive)
{
    float v[4] = { spec, shininess, refl, emissive };
    SetShaderValue(L[0].sh, L[0].mat, v, SHADER_UNIFORM_VEC4);
    SetShaderValue(L[0].sh, L[0].reflMin, &reflMin, SHADER_UNIFORM_FLOAT);
}

Color FogColor(void) { return fogColor; }
Vector3 SunDir(void) { return sunDir; }

void DrawSky(Camera3D cam, int w, int h)
{
    // screen-space height of the horizon
    Vector3 fwd = Vector3Subtract(cam.target, cam.position);
    fwd.y = 0;
    if (Vector3Length(fwd) < 1e-3f) fwd = (Vector3){ 0, 0, -1 };
    fwd = Vector3Normalize(fwd);
    Vector3 far = Vector3Add(cam.position, Vector3Scale(fwd, 20000.0f));
    far.y = cam.position.y;
    Vector2 hp = GetWorldToScreenEx(far, cam, w, h);
    int hy = (int)hp.y;
    if (hy < -h) hy = -h;
    if (hy > 2 * h) hy = 2 * h;
    Color top = { 74, 128, 196, 255 }, mid = { 146, 184, 222, 255 };
    int split = hy - h / 3;
    DrawRectangleGradientV(0, split - 2 * h, w, 2 * h, (Color){ 52, 102, 178, 255 }, top);
    DrawRectangleGradientV(0, split, w, hy - split, top, mid);
    DrawRectangleGradientV(0, hy - h / 5, w, h / 5 + 2, mid, fogColor);
    DrawRectangle(0, hy, w, h * 2, fogColor);
    // sun glow
    Vector3 sp = Vector3Add(cam.position, Vector3Scale(sunDir, 5000.0f));
    Vector3 toSun = Vector3Subtract(sp, cam.position);
    if (Vector3DotProduct(toSun, Vector3Subtract(cam.target, cam.position)) > 0) {
        Vector2 s = GetWorldToScreenEx(sp, cam, w, h);
        DrawCircleGradient((int)s.x, (int)s.y, h * 0.45f, (Color){ 255, 240, 200, 80 }, (Color){ 255, 240, 200, 0 });
        DrawCircleGradient((int)s.x, (int)s.y, h * 0.06f, (Color){ 255, 252, 235, 255 }, (Color){ 255, 245, 210, 0 });
    }
}

// ---------------------------------------------------------------- mesh building
void MB_Init(MeshBuf *b, int cap)
{
    memset(b, 0, sizeof(*b));
    if (cap < 64) cap = 64;
    b->cap = cap;
    b->pos = malloc(sizeof(float) * 3 * cap);
    b->nrm = malloc(sizeof(float) * 3 * cap);
    b->uv = malloc(sizeof(float) * 2 * cap);
    b->uv2 = malloc(sizeof(float) * 2 * cap);
    b->col = malloc(4 * cap);
    b->flat = 1.0f;
}

void MB_Free(MeshBuf *b) { free(b->pos); free(b->nrm); free(b->uv); free(b->uv2); free(b->col); memset(b, 0, sizeof(*b)); }

void MB_Mat(MeshBuf *b, float matId, float flat, float tileMetres)
{
    b->matId = matId;
    b->flat = flat;
    b->autoUV = tileMetres > 0;
    b->uvScale = tileMetres > 0 ? 1.0f / tileMetres : 0;
}

static void grow(MeshBuf *b, int add)
{
    if (b->count + add <= b->cap) return;
    while (b->count + add > b->cap) b->cap *= 2;
    b->pos = realloc(b->pos, sizeof(float) * 3 * b->cap);
    b->nrm = realloc(b->nrm, sizeof(float) * 3 * b->cap);
    b->uv = realloc(b->uv, sizeof(float) * 2 * b->cap);
    b->uv2 = realloc(b->uv2, sizeof(float) * 2 * b->cap);
    b->col = realloc(b->col, 4 * b->cap);
}

static inline void put(MeshBuf *b, Vector3 p, Vector3 n, Color c, Vector2 t)
{
    int i = b->count++;
    b->pos[3 * i] = p.x; b->pos[3 * i + 1] = p.y; b->pos[3 * i + 2] = p.z;
    b->nrm[3 * i] = n.x; b->nrm[3 * i + 1] = n.y; b->nrm[3 * i + 2] = n.z;
    b->col[4 * i] = c.r; b->col[4 * i + 1] = c.g; b->col[4 * i + 2] = c.b; b->col[4 * i + 3] = c.a;
    if (b->autoUV) {
        float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
        if (ay >= ax && ay >= az) t = (Vector2){ p.x * b->uvScale, p.z * b->uvScale };
        else if (ax >= az) t = (Vector2){ p.z * b->uvScale, p.y * b->uvScale };
        else t = (Vector2){ p.x * b->uvScale, p.y * b->uvScale };
    }
    b->uv[2 * i] = t.x; b->uv[2 * i + 1] = t.y;
    b->uv2[2 * i] = b->matId; b->uv2[2 * i + 1] = b->flat;
}

void MB_TriUV(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 na, Vector3 nc, Vector3 nd, Color ca, Color cc, Color cd,
              Vector2 ta, Vector2 tc, Vector2 td)
{
    grow(b, 3);
    put(b, a, na, ca, ta); put(b, c, nc, cc, tc); put(b, d, nd, cd, td);
}

void MB_TriN(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 na, Vector3 nc, Vector3 nd, Color ca, Color cc, Color cd)
{
    Vector2 z = { 0, 0 };
    MB_TriUV(b, a, c, d, na, nc, nd, ca, cc, cd, z, z, z);
}

void MB_Tri(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Color col)
{
    Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(c, a), Vector3Subtract(d, a)));
    MB_TriN(b, a, c, d, n, n, n, col, col, col);
}

void MB_Quad(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Color col)
{
    MB_Tri(b, a, c, d, col);
    MB_Tri(b, a, d, e, col);
}

void MB_QuadUV(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Color col, Vector2 ta, Vector2 tc, Vector2 td, Vector2 te)
{
    Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(c, a), Vector3Subtract(d, a)));
    MB_QuadN(b, a, c, d, e, n, col, ta, tc, td, te);
}

void MB_QuadN(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Vector3 n, Color col, Vector2 ta, Vector2 tc, Vector2 td, Vector2 te)
{
    MB_TriUV(b, a, c, d, n, n, n, col, col, col, ta, tc, td);
    MB_TriUV(b, a, d, e, n, n, n, col, col, col, ta, td, te);
}

void MB_Box(MeshBuf *b, Vector3 ce, Vector3 hf, float yaw, Color col)
{
    float cs = cosf(yaw), sn = sinf(yaw);
    Vector3 v[8];
    for (int i = 0; i < 8; i++) {
        float x = (i & 1) ? hf.x : -hf.x, y = (i & 2) ? hf.y : -hf.y, z = (i & 4) ? hf.z : -hf.z;
        v[i] = (Vector3){ ce.x + x * cs + z * sn, ce.y + y, ce.z - x * sn + z * cs };
    }
    // faces (CCW seen from outside)
    MB_Quad(b, v[4], v[5], v[7], v[6], col); // +z
    MB_Quad(b, v[1], v[0], v[2], v[3], col); // -z
    MB_Quad(b, v[5], v[1], v[3], v[7], col); // +x
    MB_Quad(b, v[0], v[4], v[6], v[2], col); // -x
    MB_Quad(b, v[6], v[7], v[3], v[2], col); // +y
}

void MB_Cylinder(MeshBuf *b, Vector3 base, float r0, float r1, float h, int sides, Color col, bool cap)
{
    float slope = (r0 - r1) / h;
    float us = b->autoUV ? b->uvScale : 0;        // cylindrical uv: u = arc length, v = height (metres / tile)
    float circ = 2 * PI * (r0 + r1) * 0.5f;
    int savedAuto = b->autoUV;
    b->autoUV = 0;
    for (int i = 0; i < sides; i++) {
        float a0 = 2 * PI * i / sides, a1 = 2 * PI * (i + 1) / sides;
        Vector3 p0 = { base.x + cosf(a0) * r0, base.y, base.z + sinf(a0) * r0 };
        Vector3 p1 = { base.x + cosf(a1) * r0, base.y, base.z + sinf(a1) * r0 };
        Vector3 q0 = { base.x + cosf(a0) * r1, base.y + h, base.z + sinf(a0) * r1 };
        Vector3 q1 = { base.x + cosf(a1) * r1, base.y + h, base.z + sinf(a1) * r1 };
        Vector3 n0 = Vector3Normalize((Vector3){ cosf(a0), slope, sinf(a0) });
        Vector3 n1 = Vector3Normalize((Vector3){ cosf(a1), slope, sinf(a1) });
        Vector2 t00 = { circ * i / sides * us, base.y * us }, t10 = { circ * (i + 1) / sides * us, base.y * us };
        Vector2 t01 = { t00.x, (base.y + h) * us }, t11 = { t10.x, (base.y + h) * us };
        MB_TriUV(b, p0, q1, p1, n0, n1, n1, col, col, col, t00, t11, t10);
        MB_TriUV(b, p0, q0, q1, n0, n0, n1, col, col, col, t00, t01, t11);
        if (cap && r1 > 0.001f) {
            Vector3 top = { base.x, base.y + h, base.z }, up = { 0, 1, 0 };
            MB_TriUV(b, top, q1, q0, up, up, up, col, col, col, t01, t11, t01);
        }
    }
    b->autoUV = savedAuto;
}

void MB_Blob(MeshBuf *b, Vector3 c, Vector3 r, int detail, Color col, float jitter, unsigned seed)
{
    int rings = detail < 2 ? 2 : (detail > 8 ? 8 : detail);
    int segs = rings * 2;
    Vector3 grid[9][17], nrm[9][17];
    unsigned s = seed;
    for (int i = 0; i <= rings; i++) {
        float th = PI * i / rings;
        for (int j = 0; j < segs; j++) {
            float ph = 2 * PI * j / segs;
            float k = (i == 0 || i == rings) ? 1.0f : 1.0f + (Rand01(&s) - 0.5f) * jitter;
            Vector3 d = { sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph) };
            nrm[i][j] = d;
            grid[i][j] = (Vector3){ c.x + d.x * r.x * k, c.y + d.y * r.y * k, c.z + d.z * r.z * k };
        }
        grid[i][segs] = grid[i][0];
        nrm[i][segs] = nrm[i][0];
    }
    Color top = ColorBrightness(col, 0.10f), bot = ColorBrightness(col, -0.30f);
    for (int i = 0; i < rings; i++) {
        Color ca = ColorLerp(top, bot, (float)i / rings), cb = ColorLerp(top, bot, (float)(i + 1) / rings);
        for (int j = 0; j < segs; j++) {
            if (i < rings - 1)
                MB_TriN(b, grid[i][j], grid[i + 1][j + 1], grid[i + 1][j], nrm[i][j], nrm[i + 1][j + 1], nrm[i + 1][j], ca, cb, cb);
            if (i > 0)
                MB_TriN(b, grid[i][j], grid[i][j + 1], grid[i + 1][j + 1], nrm[i][j], nrm[i][j + 1], nrm[i + 1][j + 1], ca, ca, cb);
        }
    }
}

Mesh MB_Upload(MeshBuf *b)
{
    Mesh m = { 0 };
    m.vertexCount = b->count;
    m.triangleCount = b->count / 3;
    m.vertices = MemAlloc(sizeof(float) * 3 * b->count);
    m.normals = MemAlloc(sizeof(float) * 3 * b->count);
    m.colors = MemAlloc(4 * b->count);
    m.texcoords = MemAlloc(sizeof(float) * 2 * b->count);
    m.texcoords2 = MemAlloc(sizeof(float) * 2 * b->count);
    memcpy(m.vertices, b->pos, sizeof(float) * 3 * b->count);
    memcpy(m.normals, b->nrm, sizeof(float) * 3 * b->count);
    memcpy(m.colors, b->col, 4 * b->count);
    memcpy(m.texcoords, b->uv, sizeof(float) * 2 * b->count);
    memcpy(m.texcoords2, b->uv2, sizeof(float) * 2 * b->count);
    UploadMesh(&m, false);
    // CPU copies are not needed afterwards: free them to save RAM
    MemFree(m.vertices); m.vertices = NULL;
    MemFree(m.normals); m.normals = NULL;
    MemFree(m.colors); m.colors = NULL;
    MemFree(m.texcoords); m.texcoords = NULL;
    MemFree(m.texcoords2); m.texcoords2 = NULL;
    return m;
}

// ---------------------------------------------------------------- indexed grid builder
void GB_Init(GridBuf *g, int kx0, int ky0, int kw, int kh)
{
    memset(g, 0, sizeof(*g));
    g->lx0 = kx0; g->ly0 = ky0; g->lw = kw > 0 ? kw : 1; g->lh = kh > 0 ? kh : 1;
    g->lookup = malloc(sizeof(int) * g->lw * g->lh);
    for (int i = 0; i < g->lw * g->lh; i++) g->lookup[i] = -1;
    g->capV = 4096; g->capI = 8192;
    g->pos = malloc(sizeof(float) * 3 * g->capV);
    g->nrm = malloc(sizeof(float) * 3 * g->capV);
    g->uv = malloc(sizeof(float) * 2 * g->capV);
    g->uv2 = malloc(sizeof(float) * 2 * g->capV);
    g->col = malloc(4 * g->capV);
    g->idx = malloc(sizeof(unsigned short) * g->capI);
}

void GB_Free(GridBuf *g)
{
    free(g->pos); free(g->nrm); free(g->uv); free(g->uv2); free(g->col); free(g->idx); free(g->lookup);
    memset(g, 0, sizeof(*g));
}

int GB_VertexUnique(GridBuf *g, Vector3 p, Vector3 n, Color c, Vector2 uv, float matId, float flat)
{
    if (g->nv >= 65535) { static int warned; if (!warned++) TraceLog(LOG_WARNING, "SCENE: terrain chunk exceeds 65535 vertices"); return 65534; }
    if (g->nv >= g->capV) {
        g->capV *= 2;
        g->pos = realloc(g->pos, sizeof(float) * 3 * g->capV);
        g->nrm = realloc(g->nrm, sizeof(float) * 3 * g->capV);
        g->uv = realloc(g->uv, sizeof(float) * 2 * g->capV);
        g->uv2 = realloc(g->uv2, sizeof(float) * 2 * g->capV);
        g->col = realloc(g->col, 4 * g->capV);
    }
    int i = g->nv++;
    g->pos[3 * i] = p.x; g->pos[3 * i + 1] = p.y; g->pos[3 * i + 2] = p.z;
    g->nrm[3 * i] = n.x; g->nrm[3 * i + 1] = n.y; g->nrm[3 * i + 2] = n.z;
    g->uv[2 * i] = uv.x; g->uv[2 * i + 1] = uv.y;
    g->uv2[2 * i] = matId; g->uv2[2 * i + 1] = flat;
    g->col[4 * i] = c.r; g->col[4 * i + 1] = c.g; g->col[4 * i + 2] = c.b; g->col[4 * i + 3] = c.a;
    return i;
}

int GB_Vertex(GridBuf *g, int kx, int ky, Vector3 p, Vector3 n, Color c, Vector2 uv, float matId, float flat)
{
    int lx = kx - g->lx0, ly = ky - g->ly0;
    if (lx < 0 || ly < 0 || lx >= g->lw || ly >= g->lh) return GB_VertexUnique(g, p, n, c, uv, matId, flat);
    int *slot = &g->lookup[ly * g->lw + lx];
    if (*slot >= 0) return *slot;
    *slot = GB_VertexUnique(g, p, n, c, uv, matId, flat);
    return *slot;
}

void GB_Tri(GridBuf *g, int a, int b, int c)
{
    if (g->ni + 3 > g->capI) { g->capI *= 2; g->idx = realloc(g->idx, sizeof(unsigned short) * g->capI); }
    g->idx[g->ni++] = (unsigned short)a; g->idx[g->ni++] = (unsigned short)b; g->idx[g->ni++] = (unsigned short)c;
}

Mesh GB_Upload(GridBuf *g)
{
    Mesh m = { 0 };
    m.vertexCount = g->nv;
    m.triangleCount = g->ni / 3;
    m.vertices = MemAlloc(sizeof(float) * 3 * g->nv);
    m.normals = MemAlloc(sizeof(float) * 3 * g->nv);
    m.colors = MemAlloc(4 * g->nv);
    m.texcoords = MemAlloc(sizeof(float) * 2 * g->nv);
    m.texcoords2 = MemAlloc(sizeof(float) * 2 * g->nv);
    m.indices = MemAlloc(sizeof(unsigned short) * g->ni);
    memcpy(m.vertices, g->pos, sizeof(float) * 3 * g->nv);
    memcpy(m.normals, g->nrm, sizeof(float) * 3 * g->nv);
    memcpy(m.colors, g->col, 4 * g->nv);
    memcpy(m.texcoords, g->uv, sizeof(float) * 2 * g->nv);
    memcpy(m.texcoords2, g->uv2, sizeof(float) * 2 * g->nv);
    memcpy(m.indices, g->idx, sizeof(unsigned short) * g->ni);
    UploadMesh(&m, false);
    MemFree(m.vertices); m.vertices = NULL;
    MemFree(m.normals); m.normals = NULL;
    MemFree(m.colors); m.colors = NULL;
    MemFree(m.texcoords); m.texcoords = NULL;
    MemFree(m.texcoords2); m.texcoords2 = NULL;
    // raylib's DrawMesh only uses glDrawElements while mesh.indices is non-NULL: keep that copy
    return m;
}

Color ColorJitter(Color c, unsigned seed, int amount)
{
    unsigned s = seed;
    int d = (int)((Rand01(&s) - 0.5f) * 2 * amount);
    int r = c.r + d + (int)((Rand01(&s) - 0.5f) * amount * 0.5f);
    int g = c.g + d + (int)((Rand01(&s) - 0.5f) * amount * 0.5f);
    int bb = c.b + d;
    return (Color){ (unsigned char)Clamp(r, 0, 255), (unsigned char)Clamp(g, 0, 255), (unsigned char)Clamp(bb, 0, 255), c.a };
}

unsigned Hash2(int x, int z)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)z * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float Rand01(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    unsigned v = *s ^ (*s >> 15);
    return (v & 0xFFFFFF) / 16777216.0f;
}
