#include "resolution.h"

static int g_res_w = 1280;
static int g_res_h = 720;

void resolution_init(int width, int height) {
    if (width >= 320 && width <= 3840) g_res_w = width;
    if (height >= 240 && height <= 2160) g_res_h = height;
}

int resolution_width(void)  { return g_res_w; }
int resolution_height(void) { return g_res_h; }
