#ifndef KEYBOARD_H
#define KEYBOARD_H
#include "stdint.h"
extern int shift_pressed;
extern int caps_lock;
void keyboard_init();
void keyboard_handle_byte(uint8_t scancode);
#endif