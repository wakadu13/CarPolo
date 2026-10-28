// Lighting shaders + sky + shared mesh-building helpers
#ifndef CARPOLO_RENDER_H
#define CARPOLO_RENDER_H

#include "raylib.h"
#include <stdint.h>

typedef struct {
    float spec;      // specular strength
    float shininess; // Blinn-Phong exponent
    float reflect;   // fresnel sky-reflection strength
    float emissive;  // 0..1 self-lit
} MatParams;

// Vertex contract of the world shaders (see DESIGN.md):
//   texcoord  (uv)  : texture coordinates, in texture tiles (world metres / tile size)
//   texcoord2 (uv2) : x = material id 0..3 (fractions blend the 4 textures of the material),
//                     y = "flat" weight (1 = ignore textures, use the vertex colour only)
//   color           : rgb = tint multiplied with the texture, a = opacity
// gLit samples texture0..texture3 (Material maps ALBEDO, METALNESS, NORMAL, ROUGHNESS);
// gLitCut is the alpha-tested variant for foliage cards (texture0 only, two-sided lighting).
extern Shader gLit, gLitCut;
extern Shader gLitInst;            // foliage shader for GPU-instanced models (attribute instanceTransform, tint in its bottom row)
extern Shader gLitImp;             // foliage shader for impostor cards: texcoord2 = tree x/z, hidden within RenderSetNearCut()
extern Shader gDepth, gDepthCut;   // shadow-map pass (packed depth), plain and alpha-tested
extern Shader gDepthInst;          // shadow-map pass, instanced alpha-tested
extern Shader gUnlitCut;           // unlit alpha-tested (impostor baking)
#define RENDER_NEAR 0.35           // camera clip planes (metres)
#define RENDER_FAR  30000.0
extern MatParams MAT_MATTE, MAT_ROAD, MAT_PAINT, MAT_GLASS, MAT_CHROME, MAT_PLASTIC, MAT_LIGHT, MAT_RUBBER;

void RenderInit(void);
void RenderShutdown(void);
void RenderSetFrame(Camera3D cam, float fogDensity);  // per-frame uniforms (both shaders)
void RenderSetMat(MatParams m);
void RenderSetMatEx(float spec, float shininess, float refl, float reflMin, float emissive);
void RenderSetFog(float density);                       // exponential haze: 1 - exp(-dist * density)
void RenderSetRise(float k);                            // 0..1: detail geometry rises from the ground (chunk fade-in)
// Sun shadow map: render the casters between RenderShadowBegin/End with RenderLitShader() (= gDepth
// during the pass), the lit shaders then sample it (texture unit 7).
void     RenderSetShadows(bool on);
bool     RenderShadowsEnabled(void);
Camera3D RenderShadowBegin(Vector3 centre);              // orthographic light camera around 'centre'
void     RenderShadowEnd(void);
Shader   RenderLitShader(void);                          // gLit, or gDepth inside the shadow pass
float    RenderShadowRadius(void);
Texture2D RenderShadowTexture(void);                    // debug
bool     RenderInShadowPass(void);
bool     RenderInstancing(void);                         // DrawMeshInstanced usable on this GPU
void     RenderSetNearCut(float metres);                 // impostor cards closer than this (x/z) are hidden
void DrawSky(Camera3D cam, int w, int h);
Color FogColor(void);
Vector3 SunDir(void);

// ---- growable vertex buffer -> raylib Mesh (non indexed triangles)
typedef struct {
    float *pos, *nrm, *uv, *uv2;
    unsigned char *col;
    int count, cap;
    // material state applied to the vertices added next (MB_Mat)
    float matId, flat;
    int autoUV;        // 1: planar uv from the world position (dominant axis of the normal)
    float uvScale;     // 1 / tile size in metres
} MeshBuf;

void MB_Init(MeshBuf *b, int cap);
void MB_Free(MeshBuf *b);
// Material for the following vertices: id 0..3 (fractions blend), flat 1 = vertex colour only,
// tileMetres > 0 = automatic planar uv (world metres / tileMetres), <= 0 = explicit uv (0 by default).
void MB_Mat(MeshBuf *b, float matId, float flat, float tileMetres);
void MB_Tri(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Color col);              // flat normal
void MB_TriN(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 na, Vector3 nc, Vector3 nd, Color ca, Color cc, Color cd);
void MB_TriUV(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 na, Vector3 nc, Vector3 nd, Color ca, Color cc, Color cd,
              Vector2 ta, Vector2 tc, Vector2 td);
void MB_Quad(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Color col);  // a,c,d,e counter-clockwise seen from the front
void MB_QuadUV(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Color col, Vector2 ta, Vector2 tc, Vector2 td, Vector2 te);
void MB_QuadN(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Vector3 n, Color col, Vector2 ta, Vector2 tc, Vector2 td, Vector2 te);
void MB_Box(MeshBuf *b, Vector3 center, Vector3 half, float yaw, Color col);      // yaw around Y
void MB_Cylinder(MeshBuf *b, Vector3 base, float r0, float r1, float h, int sides, Color col, bool cap); // cylindrical uv
void MB_Blob(MeshBuf *b, Vector3 c, Vector3 r, int detail, Color col, float jitter, unsigned seed); // lumpy ellipsoid
Mesh MB_Upload(MeshBuf *b);   // creates a GPU mesh (copies data), b stays valid

// ---- indexed grid mesh builder (terrain): vertices shared through a 2D key
typedef struct {
    float *pos, *nrm, *uv, *uv2;
    unsigned char *col;
    unsigned short *idx;
    int nv, capV, ni, capI;
    int *lookup; int lw, lh, lx0, ly0;   // key (kx, ky) -> vertex index or -1
} GridBuf;
void GB_Init(GridBuf *g, int kx0, int ky0, int kw, int kh);
void GB_Free(GridBuf *g);
int  GB_Vertex(GridBuf *g, int kx, int ky, Vector3 p, Vector3 n, Color c, Vector2 uv, float matId, float flat); // shared
int  GB_VertexUnique(GridBuf *g, Vector3 p, Vector3 n, Color c, Vector2 uv, float matId, float flat);
void GB_Tri(GridBuf *g, int a, int b, int c);
Mesh GB_Upload(GridBuf *g);

Color ColorJitter(Color c, unsigned seed, int amount);
unsigned Hash2(int x, int z);
float Rand01(unsigned *s);

#endif
