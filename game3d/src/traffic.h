// AI traffic driving along the route (same direction and oncoming)
#ifndef CARPOLO_TRAFFIC_H
#define CARPOLO_TRAFFIC_H

#include "raylib.h"
#include "car.h"

void TrafficInit(void);                 // builds the vehicle models (needs GL)
void TrafficShutdown(void);
void TrafficReset(float sPlayer);
void TrafficUpdate(float dt, const PlayerCar *pc, float pcLen, float pcWid);
void TrafficDraw(Camera3D cam);
int  TrafficCollide(const PlayerCar *pc, float len, float wid);   // index of the car hit, or -1
void TrafficOnCrash(int idx);
void TrafficClearNear(float s, float behind, float ahead);
int  TrafficHonking(void);              // >0 while an oncoming car honks at the player (consumed)
int  TrafficNearMiss(void);             // count of cars passed very close since last call

#endif
