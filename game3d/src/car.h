// Player car: arcade-realistic physics in world space + projection on the route
#ifndef CARPOLO_CAR_H
#define CARPOLO_CAR_H

#include "raylib.h"
#include "car_model.h"
#include <stdbool.h>

typedef struct {
    Vector3 pos;          // ground point under the centre of the car
    float yaw;            // forward = (cos yaw, 0, sin yaw); yaw grows when turning right
    float speed;          // m/s along forward (negative = reverse)
    float steerIn;        // smoothed steering input, -1..1, + = right
    float steerAngle;     // front wheel angle (rad, + = right)
    float yawRate, latAcc, lonAcc;
    float roll, pitch;    // visual body attitude (rad)
    float s, d;           // route projection (d > 0 = right of the centre line)
    int seg;
    bool onRoad;          // on the paved surface
    float skid;           // 0..1 (tyre screech)
    float wheelSpin;
    float odometer;
    float rpm; int gear;
    Vector3 up, fwd;      // attitude from the ground
    float bounce, bounceV;
} PlayerCar;

void   CarReset(PlayerCar *c, float s, float d);
void   CarUpdate(PlayerCar *c, float dt, float throttle, float brake, float steer, bool analog, bool assist);
float  CarRoadSurfaceY(float x, float z, int hint, float *outS, float *outD, int *outSeg, bool *onRoad);
Matrix CarMatrix(const PlayerCar *c, bool withBody);
void   DrawCarModel(const CarModel *m, Matrix world, float wheelSpin, float steerAngle, bool brakeLights);
void   DrawBlobShadow(Matrix world, float length, float width);
void   CarInitShared(void);
float  LaneCenterD(int routeIdx, int dir, int lane);   // lateral offset of a lane centre
int    LaneCount(int routeIdx, int dir);

#endif
