// Race dressing along the route: start, checkpoint and finish arches, distance boards before the
// finish, kilometre boards, chevron boards and red/white kerbs in the sharp bends, barriers closing
// the side roads, bunting in the villages, sponsor boards, spectators and hay bales.
#ifndef CARPOLO_COURSE_H
#define CARPOLO_COURSE_H

#include "raylib.h"
#include <stdbool.h>

#define COURSE_MAX_CP 8

void        CourseInit(Font font, bool fontOk);    // after SceneInit (uses the conformed ground)
void        CourseShutdown(void);
void        CourseDraw(Camera3D cam);
void        CourseDrawShadow(Vector3 centre, float radius);
// Solid course objects (arch pillars, barriers, boards, bales): true and a short label if hit
bool        CourseCollide(float x, float z, float radius, const char **what);
int         CourseCheckpointCount(void);
float       CourseCheckpointS(int i);              // distance along the route of checkpoint i
const char *CourseCheckpointName(int i);
float       CourseFinishS(void);                   // the race ends when the car reaches this distance

#endif
