// Writes the procedural textures as PNG files for inspection:  texdump <outdir> [quality]
#include "raylib.h"
#include "render.h"
#include "textures.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int q = argc > 2 ? atoi(argv[2]) : 2;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "texdump");
    RenderInit();
    TexturesInit(q);
    TexturesExport(dir);
    TexturesShutdown();
    RenderShutdown();
    CloseWindow();
    printf("textures written to %s\n", dir);
    return 0;
}
