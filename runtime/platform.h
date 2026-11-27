#ifndef ASPHALT6_PLATFORM_H
#define ASPHALT6_PLATFORM_H

#include <stdint.h>

int   platform_init(int width, int height, const char *title);
void  platform_shutdown(void);
void  platform_poll(void);
void  platform_swap(void);
int   platform_should_quit(void);
int   platform_width(void);
int   platform_height(void);
void  platform_set_design_size(int width, int height);
void *platform_gl_proc(const char *name);

int   platform_take_key(void);
int   platform_take_game_key(int *code, int *down);
int   platform_held_key(int code);
void  platform_inject_key(int code, int down);
int   platform_take_pointer(int *x, int *y, int *down);

float  platform_gamepad_steer(void);
int    platform_gamepad_gas(void);
int    platform_gamepad_brake(void);
int    platform_gamepad_nitro(void);
int    platform_gamepad_b(void);
double platform_frame_dt(void);

#endif /* ASPHALT6_PLATFORM_H */
