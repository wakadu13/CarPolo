// Streaming world geometry around the route (terrain, road, vegetation, buildings, side roads)
#ifndef CARPOLO_SCENE_H
#define CARPOLO_SCENE_H

#include "raylib.h"
#include <stdbool.h>

#define CHUNK_LEN 100.0f

typedef struct { float x, z, r; unsigned char kind; } Obstacle; // kind: 0 tree, 1 building, 2 pole, 3 barrier

void  SceneInit(const char *treesBinPath, const char *treesPngPath);   // after WorldLoad + RenderInit (model pack paths)
void  SceneShutdown(void);
// Streams chunks around 'focus'. budgetMs > 0: build on this thread for at most that long (loading
// screen); -1: background thread (or a short slice per frame on platforms without threads).
void  SceneUpdate(Vector3 focus, float budgetMs);
float SceneProgress(Vector3 focus);            // 0..1: fraction of wanted chunks that are built
void  SceneDraw(Camera3D cam);
void  SceneDrawShadow(Vector3 centre, float radius);   // casters for the sun shadow map (depth shaders)

// Terrain height with the road corridor flattened (same function used to build the meshes)
float SceneGround(float x, float z, int routeHint);
// Corridor distance to the route (coarse, metres; large value outside the corridor)
float SceneRouteDist(float x, float z);
// Obstacles near (x,z) within 'radius': returns true and the obstacle hit
void  SceneStats(int *loaded, long *verts, long *bytes);   // bytes = GPU vertex/index memory
void  SceneSetQuality(int q);                 // 0 light, 1 normal, 2 high (call before SceneInit for the texture size)
int   SceneQuality(void);
bool  SceneCollide(float x, float z, float radius, Obstacle *hit);

#endif
