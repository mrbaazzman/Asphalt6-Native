#ifndef ASPHALT6_RESOLUTION_H
#define ASPHALT6_RESOLUTION_H

/* Logical render resolution presented to the game (default native 800x480 landscape or scaled window). */
void resolution_init(int width, int height);
int  resolution_width(void);
int  resolution_height(void);

#endif /* ASPHALT6_RESOLUTION_H */
