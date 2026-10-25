// Road signs with text (town entry/exit signs, start and finish markers)
#ifndef CARPOLO_SIGNS_H
#define CARPOLO_SIGNS_H

#include "raylib.h"

void SignsInit(Font font, bool fontOk);   // after WorldLoad + RenderInit
void SignsDraw(Camera3D cam);
void SignsShutdown(void);

#endif
