// Procedural, tileable textures generated at start-up (no image assets to ship).
// Ground: 4 textures per material group (see render.h: texture0..3 selected by the vertex material id).
#ifndef CARPOLO_TEXTURES_H
#define CARPOLO_TEXTURES_H

#include "raylib.h"

typedef enum {
    TEX_GRASS, TEX_SCRUB, TEX_ROCK, TEX_SOIL,          // terrain group  (tile 6 m)
    TEX_ASPHALT, TEX_GRAVEL, TEX_DIRT, TEX_CONCRETE,   // road group     (tile 4 m)
    TEX_STUCCO, TEX_ROOF, TEX_BARK, TEX_PLASTER2,      // detail group   (stucco 3 m, roof 2.4 m, bark 1.2 m, concrete 3 m)
    TEX_LEAVES,                                        // foliage atlas (alpha, coverage-preserving mipmaps)
    TEX_COUNT
} TexId;

// Regions of the foliage atlas, as uv rectangles (x, y, width, height in 0..1)
typedef enum { LEAF_BROAD, LEAF_PINE, LEAF_OLIVE, LEAF_CYPRESS, LEAF_GRASS, LEAF_SHRUB, LEAF_VINE, LEAF_PLANE, LEAF_COUNT } LeafKind;

void      TexturesInit(int quality);          // 0 light (half resolution), 1 normal, 2 high
void      TexturesShutdown(void);
Texture2D Tex(int id);
Rectangle LeafUV(int kind);
void      TexturesExport(const char *dir);    // debug: writes <dir>/tex_*.png

#endif
