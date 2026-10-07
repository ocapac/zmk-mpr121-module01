#pragma once

#define DISC_POS_TAP_CENTER          2
#define DISC_POS_TAP_TOP             3
#define DISC_POS_TAP_BOTTOM          4
#define DISC_POS_TAP_LEFT            5
#define DISC_POS_TAP_RIGHT           6

#define DISC_POS_SWIPE_UP            7
#define DISC_POS_SWIPE_DOWN          8
#define DISC_POS_SWIPE_LEFT          9
#define DISC_POS_SWIPE_RIGHT         10

#define DISC_POS_ROT_TOP_CW          11
#define DISC_POS_ROT_TOP_CCW         12
#define DISC_POS_ROT_BOTTOM_CW       13
#define DISC_POS_ROT_BOTTOM_CCW      14
#define DISC_POS_ROT_LEFT_CW         15
#define DISC_POS_ROT_LEFT_CCW        16
#define DISC_POS_ROT_RIGHT_CW        17
#define DISC_POS_ROT_RIGHT_CCW       18

#define DISC_POS_PROX_ENTER          19
#define DISC_POS_PROX_EXIT           20
#define DISC_POS_PROX_IDLE           21

#define DISC_POS_COUNT               22

#define DISC_MODE_CURSOR             0
#define DISC_MODE_SCROLL             1

void disc_mode_toggle_global(void);
uint8_t disc_mode_get_global(void);
