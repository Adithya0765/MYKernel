#ifndef MOUSE_H
#define MOUSE_H
#include "stdint.h"
typedef struct { int x; int y; uint8_t buttons; } mouse_state_t;
void mouse_init();
void mouse_handle_byte(uint8_t data);
void mouse_get_state(mouse_state_t* state);
#endif