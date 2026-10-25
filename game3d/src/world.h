// Real-world data for the route (game3d/assets/world.bin, see DESIGN.md)
#ifndef CARPOLO_WORLD_H
#define CARPOLO_WORLD_H

#include <stdint.h>
#include <stdbool.h>
#include "raylib.h"

enum {
    LC_UNKNOWN = 0, LC_FOREST, LC_SHRUB, LC_GRASS, LC_CROP, LC_VINEYARD, LC_ORCHARD,
    LC_RESIDENTIAL, LC_INDUSTRIAL, LC_BARE, LC_WATER, LC_WETLAND, LC_PAVED, LC_COUNT
};

enum { RF_ONEWAY = 1, RF_ROUNDABOUT = 2, RF_BRIDGE = 4, RF_TUNNEL = 8, RF_BUILTUP = 16 };
enum { RC_SERVICE = 0, RC_RESIDENTIAL, RC_TERTIARY, RC_SECONDARY, RC_PRIMARY, RC_TRUNK, RC_MOTORWAY, RC_MOTORWAY_LINK };

typedef struct {
    float x, y, z;          // centre line (y = road surface)
    float width;            // paved width of our carriageway
    uint8_t lanes, lanesForward, roadClass, flags;
    uint16_t speedLimit, nameId, placeId;
    // derived at load
    float s;                // distance from start (m)
    float hx, hz;           // unit heading (xz)
    float curv;             // signed curvature (1/m), >0 = turning right
} RoutePt;

typedef struct {
    float x0, z0, cell;
    int nx, nz;
    int16_t *h;             // decimetres (DEM) — NULL for class grids
    uint8_t *c;             // classes (LCOV)
} Grid;

typedef struct {
    int nv;
    uint8_t kind, roofShape;
    float height, minHeight;
    float *xz;              // nv * 2
    float cx, cz, radius;   // derived bounding circle
} Building;

typedef struct {
    int np;
    uint8_t roadClass, lanes, flags;
    float width;
    float *xz;              // np * 2
} SideRoad;

typedef struct { float x, z; uint8_t type; } MappedTree;
typedef struct { float x, z; uint16_t nameId; uint8_t kind; } Place;
typedef struct { float s; uint16_t nameId, subId; } Zone;

typedef struct {
    double originLat, originLon;
    float minX, minZ, maxX, maxZ;
    int nStrings; char **strings;
    int nRoute; RoutePt *route; float length;
    Grid dem, lcov, far, farc;
    int nBuildings; Building *buildings;
    int nRoads; SideRoad *roads;
    int nTrees; MappedTree *trees;
    int nPlaces; Place *places;
    int nZones; Zone *zones;
} World;

extern World W;

bool WorldLoad(const char *path);
void WorldUnload(void);
const char *WorldStr(int id);                // "" if none

float GridHeight(const Grid *g, float x, float z);   // bilinear, metres
int   GridClass(const Grid *g, float x, float z);    // nearest
float DemHeight(float x, float z);

// Route queries
int   RouteIndexAtS(float s);                // index i with route[i].s <= s < route[i+1].s
void  RouteSample(float s, Vector3 *pos, float *hx, float *hz, float *width, const RoutePt **attr);
// Nearest point on the route polyline to (x,z), searching around a hint index (-1 = global).
// Returns s, signed lateral offset d (>0 = right of travel direction) and the segment index.
float RouteProject(float x, float z, int hint, int window, float *d, int *seg);

#endif
