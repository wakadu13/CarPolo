// Lighting shader (GLSL 100 / 330), sky, and CPU mesh building helpers.
#include "render.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

Shader gLit;
MatParams MAT_MATTE   = { 0.05f, 8.0f, 0.00f, 0.0f };
MatParams MAT_ROAD    = { 0.10f, 20.0f, 0.04f, 0.0f };
MatParams MAT_PAINT   = { 0.75f, 70.0f, 0.38f, 0.0f };
MatParams MAT_GLASS   = { 1.20f, 140.0f, 0.70f, 0.0f };
MatParams MAT_CHROME  = { 1.20f, 90.0f, 0.85f, 0.05f };
MatParams MAT_PLASTIC = { 0.15f, 20.0f, 0.06f, 0.0f };
MatParams MAT_LIGHT   = { 0.80f, 80.0f, 0.30f, 0.55f };
MatParams MAT_RUBBER  = { 0.04f, 6.0f, 0.00f, 0.0f };

static int locSun, locSunCol, locSky, locGround, locView, locFogCol, locFogDen, locMat, locReflMin;
static Vector3 sunDir;
static Color fogColor = { 200, 214, 226, 255 };

#if defined(PLATFORM_ANDROID) || defined(GRAPHICS_API_OPENGL_ES2)
#define VS_PREFIX "#version 100\n"
#define FS_PREFIX "#version 100\nprecision mediump float;\n"
#else
#define VS_PREFIX "#version 330\n#define attribute in\n#define varying out\n"
#define FS_PREFIX "#version 330\n#define varying in\n#define texture2D texture\nout vec4 fragOut;\n#define gl_FragColor fragOut\n"
#endif

static const char *VS_BODY =
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "uniform vec3 viewPos;\n"
    "varying vec3 vView;\n"
    "varying vec3 vNormal;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  vec4 wp = matModel * vec4(vertexPosition, 1.0);\n"
    "  vView = viewPos - wp.xyz;\n"
    "  vNormal = (matNormal * vec4(vertexNormal, 0.0)).xyz;\n"
    "  vColor = vertexColor;\n"
    "  vUV = vertexTexCoord;\n"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *FS_BODY =
    "varying vec3 vView;\n"
    "varying vec3 vNormal;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D texture0;\n"
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
    "  vec4 albedo = texture2D(texture0, vUV) * colDiffuse * vColor;\n"
    "  vec3 n = normalize(vNormal);\n"
    "  float dist = length(vView);\n"
    "  vec3 v = vView / max(dist, 0.001);\n"
    "  if (!gl_FrontFacing) n = -n;\n"
    "  float ndl = max(dot(n, sunDir), 0.0);\n"
    "  vec3 amb = mix(groundColor, skyColor, n.y * 0.5 + 0.5);\n"
    "  vec3 col = albedo.rgb * (amb + sunColor * ndl);\n"
    "  vec3 h = normalize(sunDir + v);\n"
    "  float spec = pow(max(dot(n, h), 0.0), matParams.y) * matParams.x * step(0.001, ndl);\n"
    "  float fres = mix(reflMin, 1.0, pow(1.0 - max(dot(n, v), 0.0), 4.0));\n"
    "  vec3 r = reflect(-v, n);\n"
    "  vec3 env = mix(groundColor * 0.9, skyColor * 1.25, smoothstep(-0.15, 0.35, r.y));\n"
    "  col = mix(col, env, clamp(fres * matParams.z, 0.0, 1.0));\n"
    "  col += sunColor * spec + albedo.rgb * matParams.w;\n"
    "  float fd = dist * fogDensity;\n"
    "  float fog = 1.0 - exp(-fd * fd);\n"
    "  gl_FragColor = vec4(mix(col, fogColor, fog), albedo.a);\n"
    "}\n";

void RenderInit(void)
{
    char *vs = MemAlloc(8192), *fs = MemAlloc(8192);
    strcpy(vs, VS_PREFIX); strcat(vs, VS_BODY);
    strcpy(fs, FS_PREFIX); strcat(fs, FS_BODY);
    gLit = LoadShaderFromMemory(vs, fs);
    MemFree(vs); MemFree(fs);
    gLit.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(gLit, "matModel");
    gLit.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(gLit, "matNormal");
    gLit.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(gLit, "viewPos");
    locSun = GetShaderLocation(gLit, "sunDir");
    locSunCol = GetShaderLocation(gLit, "sunColor");
    locSky = GetShaderLocation(gLit, "skyColor");
    locGround = GetShaderLocation(gLit, "groundColor");
    locView = gLit.locs[SHADER_LOC_VECTOR_VIEW];
    locFogCol = GetShaderLocation(gLit, "fogColor");
    locFogDen = GetShaderLocation(gLit, "fogDensity");
    locMat = GetShaderLocation(gLit, "matParams");
    locReflMin = GetShaderLocation(gLit, "reflMin");

    // Late-afternoon autumn sun from the south-west (+Z = south, -X = west)
    sunDir = Vector3Normalize((Vector3){ -0.55f, 0.62f, 0.56f });
    float sunCol[3] = { 0.90f, 0.82f, 0.70f };
    float sky[3] = { 0.40f, 0.47f, 0.58f };
    float ground[3] = { 0.30f, 0.27f, 0.22f };
    float fog[3] = { fogColor.r / 255.f, fogColor.g / 255.f, fogColor.b / 255.f };
    SetShaderValue(gLit, locSun, &sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(gLit, locSunCol, sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(gLit, locSky, sky, SHADER_UNIFORM_VEC3);
    SetShaderValue(gLit, locGround, ground, SHADER_UNIFORM_VEC3);
    SetShaderValue(gLit, locFogCol, fog, SHADER_UNIFORM_VEC3);
    RenderSetMat(MAT_MATTE);
    RenderSetFog(1.0f / 1500.0f);
}

void RenderShutdown(void) { UnloadShader(gLit); }

void RenderSetFrame(Camera3D cam, float fogDensity)
{
    SetShaderValue(gLit, locView, &cam.position, SHADER_UNIFORM_VEC3);
    RenderSetFog(fogDensity);
}

void RenderSetFog(float density) { SetShaderValue(gLit, locFogDen, &density, SHADER_UNIFORM_FLOAT); }

void RenderSetMat(MatParams m) { RenderSetMatEx(m.spec, m.shininess, m.reflect, 0.12f, m.emissive); }

void RenderSetMatEx(float spec, float shininess, float refl, float reflMin, float emissive)
{
    float v[4] = { spec, shininess, refl, emissive };
    SetShaderValue(gLit, locMat, v, SHADER_UNIFORM_VEC4);
    SetShaderValue(gLit, locReflMin, &reflMin, SHADER_UNIFORM_FLOAT);
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
    Color top = { 74, 128, 196, 255 }, mid = { 140, 180, 222, 255 };
    int split = hy - h / 3;
    DrawRectangleGradientV(0, split - 2 * h, w, 2 * h, (Color){ 52, 102, 178, 255 }, top);
    DrawRectangleGradientV(0, split, w, hy - split, top, mid);
    DrawRectangleGradientV(0, hy, w, h / 12 + 2, mid, fogColor);
    DrawRectangle(0, hy + h / 12, w, h * 2, fogColor);
    // sun glow
    Vector3 sp = Vector3Add(cam.position, Vector3Scale(sunDir, 5000.0f));
    Vector3 toSun = Vector3Subtract(sp, cam.position);
    if (Vector3DotProduct(toSun, Vector3Subtract(cam.target, cam.position)) > 0) {
        Vector2 s = GetWorldToScreenEx(sp, cam, w, h);
        DrawCircleGradient((int)s.x, (int)s.y, h * 0.35f, (Color){ 255, 240, 200, 90 }, (Color){ 255, 240, 200, 0 });
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
    b->col = malloc(4 * cap);
}

void MB_Free(MeshBuf *b) { free(b->pos); free(b->nrm); free(b->col); memset(b, 0, sizeof(*b)); }

static void grow(MeshBuf *b, int add)
{
    if (b->count + add <= b->cap) return;
    while (b->count + add > b->cap) b->cap *= 2;
    b->pos = realloc(b->pos, sizeof(float) * 3 * b->cap);
    b->nrm = realloc(b->nrm, sizeof(float) * 3 * b->cap);
    b->col = realloc(b->col, 4 * b->cap);
}

static inline void put(MeshBuf *b, Vector3 p, Vector3 n, Color c)
{
    int i = b->count++;
    b->pos[3 * i] = p.x; b->pos[3 * i + 1] = p.y; b->pos[3 * i + 2] = p.z;
    b->nrm[3 * i] = n.x; b->nrm[3 * i + 1] = n.y; b->nrm[3 * i + 2] = n.z;
    b->col[4 * i] = c.r; b->col[4 * i + 1] = c.g; b->col[4 * i + 2] = c.b; b->col[4 * i + 3] = c.a;
}

void MB_TriN(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 na, Vector3 nc, Vector3 nd, Color ca, Color cc, Color cd)
{
    grow(b, 3);
    put(b, a, na, ca); put(b, c, nc, cc); put(b, d, nd, cd);
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
    for (int i = 0; i < sides; i++) {
        float a0 = 2 * PI * i / sides, a1 = 2 * PI * (i + 1) / sides;
        Vector3 p0 = { base.x + cosf(a0) * r0, base.y, base.z + sinf(a0) * r0 };
        Vector3 p1 = { base.x + cosf(a1) * r0, base.y, base.z + sinf(a1) * r0 };
        Vector3 q0 = { base.x + cosf(a0) * r1, base.y + h, base.z + sinf(a0) * r1 };
        Vector3 q1 = { base.x + cosf(a1) * r1, base.y + h, base.z + sinf(a1) * r1 };
        Vector3 n0 = Vector3Normalize((Vector3){ cosf(a0), slope, sinf(a0) });
        Vector3 n1 = Vector3Normalize((Vector3){ cosf(a1), slope, sinf(a1) });
        MB_TriN(b, p0, q1, p1, n0, n1, n1, col, col, col);
        MB_TriN(b, p0, q0, q1, n0, n0, n1, col, col, col);
        if (cap && r1 > 0.001f) {
            Vector3 top = { base.x, base.y + h, base.z }, up = { 0, 1, 0 };
            MB_TriN(b, top, q1, q0, up, up, up, col, col, col);
        }
    }
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
    m.texcoords = MemAlloc(sizeof(float) * 2 * b->count);   // zeros
    memcpy(m.vertices, b->pos, sizeof(float) * 3 * b->count);
    memcpy(m.normals, b->nrm, sizeof(float) * 3 * b->count);
    memcpy(m.colors, b->col, 4 * b->count);
    UploadMesh(&m, false);
    // CPU copies are not needed afterwards: free them to save RAM
    MemFree(m.vertices); m.vertices = NULL;
    MemFree(m.normals); m.normals = NULL;
    MemFree(m.colors); m.colors = NULL;
    MemFree(m.texcoords); m.texcoords = NULL;
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
