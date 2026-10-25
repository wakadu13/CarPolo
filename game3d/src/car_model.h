// CarPolo 3D — procedural vehicle models (C4 Cactus phase 2 + traffic).
//
// Car space: metres, origin on the ground under the centre of the footprint,
// +Y up, +Z forward (front of the car), +X = the car's LEFT side.
//
// Every model is made of one mesh per material kind (vertex colours carry the albedo,
// normals are smooth). Draw them mesh by mesh so the shader parameters can follow the
// material kind, e.g.:
//
//     for (int i = 0; i < car.body.meshCount; i++) {
//         CarMatParams p = GetCarMatParams(car.bodyMatKind[i]);
//         ... set shader uniforms from p ...
//         DrawMesh(car.body.meshes[i], car.body.materials[car.body.meshMaterial[i]], carTransform);
//     }
//
// Wheels: `wheel` is ONE wheel centred on its hub, rolling axis = X, its visible (outer)
// face looking towards +X. For the LEFT wheels (wheelPos[0] FL, wheelPos[2] RL) draw it as is;
// for the RIGHT wheels (wheelPos[1] FR, wheelPos[3] RR) rotate it 180° around Y first so the
// rim faces outwards. Per wheel: M = RotX(spin) [then RotY(PI) for right wheels] [then RotY(steer)
// for front wheels] * Translate(wheelPos[i]) * carTransform  (raylib: MatrixMultiply(a, b) applies a
// first). Spin angle = distance travelled / wheelRadius (for the right wheels after the RotY(PI)
// the visual spin direction flips, so use -spin there).
// The semi-truck (kind 3) has more than 4 wheels: wheelPos holds the steering axle (FL/FR) and the
// first drive axle (RL/RR); the other axles are baked, static, into `body`.
#ifndef CAR_MODEL_H
#define CAR_MODEL_H

#include "raylib.h"

typedef enum {
    CARMAT_PAINT,        // glossy car paint (also glossy black trims: pillars, mirror caps, spoiler)
    CARMAT_PLASTIC,      // matte/satin black plastic: claddings, grilles, underbody, wheel wells
    CARMAT_GLASS,        // tinted glass: strong fresnel sky reflection
    CARMAT_CHROME,       // chrome chevrons and strips: mirror-like reflection
    CARMAT_RUBBER,       // tyres
    CARMAT_RIM,          // alloy wheels (silver + dark grey vertex colours)
    CARMAT_LIGHT_FRONT,  // headlights / LED daytime lights (emissive white)
    CARMAT_LIGHT_REAR,   // tail lights (emissive red)
    CARMAT_DECAL,        // textured: number plates, "CACTUS" badge (uses materials[].maps[DIFFUSE].texture)
    CARMAT_COUNT
} CarMatKind;

typedef struct {
    Model body;              // all non-rotating parts; one mesh per material kind used
    int   bodyMatKind[16];   // CarMatKind of body.materials[i] (== body.meshes[i], meshMaterial[i] == i)
    Model wheel;             // ONE wheel (tyre + rim), centred on its hub, rolling axis = X
    int   wheelMatKind[8];
    Vector3 wheelPos[4];     // hub positions in car space: FL, FR, RL, RR
    float wheelRadius, length, width, height;
} CarModel;

// Suggested shading parameters per material kind (what tools/carviewer.c uses).
typedef struct {
    float spec;       // Blinn-Phong specular strength
    float shininess;  // Blinn-Phong exponent
    float refl;       // environment (sky) reflection strength, multiplied by a fresnel term
    float reflMin;    // fresnel floor (0.04 paint ... 0.6 chrome)
    float emissive;   // 0..1: how much the albedo glows by itself
} CarMatParams;

CarModel     BuildCactusModel(void);                       // the player's C4 Cactus (needs an OpenGL context)
CarModel     BuildTrafficCarModel(int kind, Color paint);  // 0 hatchback, 1 estate/SUV, 2 van, 3 semi-truck, 4 city car
void         UnloadCarModel(CarModel *m);                  // also frees the decal texture
CarMatParams GetCarMatParams(int kind);
const char  *CarMatKindName(int kind);
int          CarModelTriangleCount(const CarModel *m);     // body + one wheel

#endif
