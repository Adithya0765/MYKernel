// keyboard.c - Processing Logic Only
#include "keyboard.h"

// Scancodes
#define SCANCODE_LSHIFT_PRESS   0x2A
#define SCANCODE_LSHIFT_RELEASE 0xAA
#define SCANCODE_RSHIFT_PRESS   0x36
#define SCANCODE_RSHIFT_RELEASE 0xB6
#define SCANCODE_CAPSLOCK       0x3A

extern void process_key(char c); // In kernel.c
extern void update_status_line(); // In kernel.c

int shift_pressed = 0;
int caps_lock = 0;

// Maps
unsigned char keyboard_map_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-',
    0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

unsigned char keyboard_map_shifted[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-',
    0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

char apply_caps_lock(char c) {
    if (!caps_lock) return c;
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

void keyboard_init() {
    shift_pressed = 0;
    caps_lock = 0;
}

// This function is called by the Kernel when it knows data is for the keyboard
void keyboard_handle_byte(uint8_t scancode) {
    // Handle Modifiers
    if (scancode == SCANCODE_LSHIFT_PRESS || scancode == SCANCODE_RSHIFT_PRESS) {
        shift_pressed = 1; return;
    }
    if (scancode == SCANCODE_LSHIFT_RELEASE || scancode == SCANCODE_RSHIFT_RELEASE) {
        shift_pressed = 0; return;
    }
    if (scancode == SCANCODE_CAPSLOCK) {
        caps_lock = !caps_lock; update_status_line(); return;
    }

    // Ignore key releases (high bit set)
    if (scancode & 0x80) return;

    // Convert to ASCII
    if (scancode < 128) {
        char c;
        if (shift_pressed) c = keyboard_map_shifted[scancode];
        else c = keyboard_map_normal[scancode];
        
        if (!shift_pressed) c = apply_caps_lock(c);
        
        if (c != 0) process_key(c);
    }
}