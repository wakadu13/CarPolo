// Lighting shader + sky + shared mesh-building helpers
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

extern Shader gLit;            // world/car lighting shader
extern MatParams MAT_MATTE, MAT_ROAD, MAT_PAINT, MAT_GLASS, MAT_CHROME, MAT_PLASTIC, MAT_LIGHT, MAT_RUBBER;

void RenderInit(void);
void RenderShutdown(void);
void RenderSetFrame(Camera3D cam, float fogDensity);  // per-frame uniforms
void RenderSetMat(MatParams m);
void RenderSetMatEx(float spec, float shininess, float refl, float reflMin, float emissive);
void RenderSetFog(float density);
void DrawSky(Camera3D cam, int w, int h);
Color FogColor(void);
Vector3 SunDir(void);

// ---- growable vertex buffer -> raylib Mesh (non indexed triangles)
typedef struct {
    float *pos, *nrm;
    unsigned char *col;
    int count, cap;
} MeshBuf;

void MB_Init(MeshBuf *b, int cap);
void MB_Free(MeshBuf *b);
void MB_Tri(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Color col);              // flat normal
void MB_TriN(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 na, Vector3 nc, Vector3 nd, Color ca, Color cc, Color cd);
void MB_Quad(MeshBuf *b, Vector3 a, Vector3 c, Vector3 d, Vector3 e, Color col);  // a,c,d,e counter-clockwise seen from the front
void MB_Box(MeshBuf *b, Vector3 center, Vector3 half, float yaw, Color col);      // yaw around Y
void MB_Cylinder(MeshBuf *b, Vector3 base, float r0, float r1, float h, int sides, Color col, bool cap);
void MB_Blob(MeshBuf *b, Vector3 c, Vector3 r, int detail, Color col, float jitter, unsigned seed); // lumpy ellipsoid
Mesh MB_Upload(MeshBuf *b);   // creates a GPU mesh (copies data), b stays valid

Color ColorJitter(Color c, unsigned seed, int amount);
unsigned Hash2(int x, int z);
float Rand01(unsigned *s);

#endif
