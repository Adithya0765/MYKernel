// mouse.c - High Res Mouse
#include "mouse.h"
#include "graphics.h"

// IO
static inline void outb(uint16_t port, uint8_t val) { asm volatile("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t ret; asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }

static int mouse_x = 512;
static int mouse_y = 384;
static uint8_t mouse_buttons = 0;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[3];

// Helper Functions
static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { while (timeout--) { if ((inb(0x64) & 1) == 1) return; } }
    else { while (timeout--) { if ((inb(0x64) & 2) == 0) return; } }
}

static void mouse_write(uint8_t data) { 
    mouse_wait(1); outb(0x64, 0xD4); mouse_wait(1); outb(0x60, data); 
}

static uint8_t mouse_read() { 
    mouse_wait(0); return inb(0x60); 
}

void mouse_init() {
    mouse_write(0xFF); mouse_read(); mouse_read(); mouse_read(); // Reset
    mouse_write(0xF6); mouse_read(); // Defaults
    mouse_write(0xF4); mouse_read(); // Enable
    mouse_x = 512; mouse_y = 384;
}

void mouse_handle_byte(uint8_t data) {
    if (mouse_cycle == 0 && !(data & 0x08)) return;
    mouse_byte[mouse_cycle++] = data;
    
    if (mouse_cycle >= 3) {
        mouse_cycle = 0;
        if (!(mouse_byte[0] & 0x08)) return;
        
        mouse_buttons = mouse_byte[0] & 0x07;
        
        // FIX: Cast to signed char.
        // X grows right (+), Y grows down (+). Mouse Y sends up as positive.
        int dx = (int8_t)mouse_byte[1]; 
        int dy = (int8_t)mouse_byte[2];
        
        mouse_x += dx;
        mouse_y -= dy; // Subtract Y because screen Y grows downwards
        
        // Clamp bounds
        int max_w = get_screen_width();
        int max_h = get_screen_height();
        
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= max_w) mouse_x = max_w - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= max_h) mouse_y = max_h - 1;
    }
}

void mouse_get_state(mouse_state_t* state) {
    state->x = mouse_x;
    state->y = mouse_y;
    state->buttons = mouse_buttons;
}