#include "graphics.h"
#include "font.h"

uint32_t* framebuffer;
static int fb_width, fb_height;
int fb_pitch;
uint32_t backbuf[1024 * 768];

// Aliases for GPU driver layer
int screen_width = 1024;
int screen_height = 768;
static uint32_t mouse_bg[24 * 24];
static int mouse_bg_saved = 0;

// Classic arrow cursor - clean straight pointer (white outline, black fill)
static const uint8_t cursor_bmp[19][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,0},
};
#define CUR_W 12
#define CUR_H 19

static uint32_t rng_state = 12345;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

void init_graphics(uint64_t addr) {
    multiboot_info_t* mb = (multiboot_info_t*)addr;
    if ((mb->flags & (1 << 12)) == 0) return;
    framebuffer = (uint32_t*)mb->framebuffer_addr;
    fb_width = mb->framebuffer_width;
    fb_height = mb->framebuffer_height;
    fb_pitch = mb->framebuffer_pitch;
    screen_width = fb_width;
    screen_height = fb_height;
}

int get_screen_width(void) { return fb_width; }
int get_screen_height(void) { return fb_height; }
uint32_t* get_backbuf(void) { return backbuf; }

void flip_buffer(void) {
    int stride = fb_pitch / 4;
    for (int y = 0; y < fb_height; y++) {
        uint32_t* src = &backbuf[y * fb_width];
        uint32_t* dst = &framebuffer[y * stride];
        for (int x = 0; x < fb_width; x++) dst[x] = src[x];
    }
}

void put_pixel(int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)fb_width || (unsigned)y >= (unsigned)fb_height) return;
    backbuf[y * fb_width + x] = color;
}

uint32_t get_pixel(int x, int y) {
    if ((unsigned)x >= (unsigned)fb_width || (unsigned)y >= (unsigned)fb_height) return 0;
    return backbuf[y * fb_width + x];
}

void put_pixel_alpha(int x, int y, uint32_t color, int alpha) {
    if ((unsigned)x >= (unsigned)fb_width || (unsigned)y >= (unsigned)fb_height) return;
    if (alpha <= 0) return;
    if (alpha >= 255) { put_pixel(x, y, color); return; }
    uint32_t bg = backbuf[y * fb_width + x];
    int br = (bg >> 16) & 0xFF, bgg = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    int r = (cr * alpha + br * (255 - alpha)) / 255;
    int g = (cg * alpha + bgg * (255 - alpha)) / 255;
    int b = (cb * alpha + bb * (255 - alpha)) / 255;
    backbuf[y * fb_width + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel(x + i, y + j, color);
}

void draw_rect_alpha(int x, int y, int w, int h, uint32_t color, int alpha) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel_alpha(x + i, y + j, color, alpha);
}

void clear_screen_gfx(uint32_t color) {
    for (int i = 0; i < fb_width * fb_height; i++) backbuf[i] = color;
    flip_buffer();
}

void draw_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                put_pixel(cx + dx, cy + dy, color);
}

void draw_hline(int x, int y, int w, uint32_t color) {
    for (int i = 0; i < w; i++) put_pixel(x + i, y, color);
}

void draw_rounded_rect(int x, int y, int w, int h, uint32_t color) {
    int r = 6; if (r > w/2) r = w/2; if (r > h/2) r = h/2;
    int r2 = r * r;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int draw = 1;
            if (i < r && j < r) { int dx=r-i-1,dy=r-j-1; if(dx*dx+dy*dy>r2) draw=0; }
            if (i>=w-r && j<r) { int dx=i-(w-r),dy=r-j-1; if(dx*dx+dy*dy>r2) draw=0; }
            if (i<r && j>=h-r) { int dx=r-i-1,dy=j-(h-r); if(dx*dx+dy*dy>r2) draw=0; }
            if (i>=w-r && j>=h-r) { int dx=i-(w-r),dy=j-(h-r); if(dx*dx+dy*dy>r2) draw=0; }
            if (draw) put_pixel(x+i, y+j, color);
        }
    }
}

void draw_rounded_rect_alpha(int x, int y, int w, int h, uint32_t color, int alpha) {
    int r = 8; if (r > w/2) r = w/2; if (r > h/2) r = h/2;
    int r2 = r * r;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int draw = 1;
            if (i < r && j < r) { int dx=r-i-1,dy=r-j-1; if(dx*dx+dy*dy>r2) draw=0; }
            if (i>=w-r && j<r) { int dx=i-(w-r),dy=r-j-1; if(dx*dx+dy*dy>r2) draw=0; }
            if (i<r && j>=h-r) { int dx=r-i-1,dy=j-(h-r); if(dx*dx+dy*dy>r2) draw=0; }
            if (i>=w-r && j>=h-r) { int dx=i-(w-r),dy=j-(h-r); if(dx*dx+dy*dy>r2) draw=0; }
            if (draw) put_pixel_alpha(x+i, y+j, color, alpha);
        }
    }
}

void draw_rounded_rect_border(int x, int y, int w, int h, uint32_t bc, int alpha) {
    int r = 8; if (r>w/2) r=w/2; if (r>h/2) r=h/2;
    int r2o = r*r, r2i = (r-1)*(r-1);
    for (int i = r; i < w-r; i++) { put_pixel_alpha(x+i,y,bc,alpha); put_pixel_alpha(x+i,y+h-1,bc,alpha); }
    for (int j = r; j < h-r; j++) { put_pixel_alpha(x,y+j,bc,alpha); put_pixel_alpha(x+w-1,y+j,bc,alpha); }
    for (int j = 0; j < r; j++) for (int i = 0; i < r; i++) {
        int dx,dy,d2;
        dx=r-i-1; dy=r-j-1; d2=dx*dx+dy*dy; if(d2<=r2o&&d2>=r2i) put_pixel_alpha(x+i,y+j,bc,alpha);
        dx=i; dy=r-j-1; d2=dx*dx+dy*dy; if(d2<=r2o&&d2>=r2i) put_pixel_alpha(x+w-r+i,y+j,bc,alpha);
        dx=r-i-1; dy=j; d2=dx*dx+dy*dy; if(d2<=r2o&&d2>=r2i) put_pixel_alpha(x+i,y+h-r+j,bc,alpha);
        dx=i; dy=j; d2=dx*dx+dy*dy; if(d2<=r2o&&d2>=r2i) put_pixel_alpha(x+w-r+i,y+h-r+j,bc,alpha);
    }
}

void draw_shadow(int x, int y, int w, int h, int layers) {
    for (int l = 1; l <= layers; l++) {
        int a = 45 - (l * 45) / layers; if (a <= 0) continue;
        for (int i = -l; i < w+l; i++) put_pixel_alpha(x+i+2, y+h+l, 0xFF000000, a);
        for (int j = -l; j < h+l; j++) put_pixel_alpha(x+w+l, y+j+2, 0xFF000000, a);
    }
}

// Deep space nebula desktop background
void draw_gradient_bg(void) {
    rng_state = 42;
    for (int y = 0; y < fb_height; y++) {
        int ratio = (y * 255) / fb_height;
        int br = 6 + (ratio * 3) / 255;
        int bg_c = 4 + (ratio * 2) / 255;
        int bb = 18 + (ratio * 10) / 255;
        for (int x = 0; x < fb_width; x++) {
            int r = br, g = bg_c, b = bb;
            // Nebula band 1
            if (y > 80 && y < 320) {
                int cy2 = 200, dist = y - cy2;
                int inten = 100 - (dist*dist)/90; if (inten < 0) inten = 0;
                int wave = ((x*3+y*2)%500)-250; if (wave<0) wave=-wave;
                int wf = (wave<200)?(200-wave):0;
                int c = (inten*wf)/300;
                r += (c*55)/100; g += (c*6)/100; b += (c*70)/100;
            }
            // Nebula band 2
            if (y > 350 && y < 580) {
                int cy2 = 465, dist = y - cy2;
                int inten = 80 - (dist*dist)/110; if (inten < 0) inten = 0;
                int wave = ((x*2+y*3+100)%600)-300; if (wave<0) wave=-wave;
                int wf = (wave<220)?(220-wave):0;
                int c = (inten*wf)/300;
                r += (c*3)/100; g += (c*35)/100; b += (c*50)/100;
            }
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            backbuf[y * fb_width + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
    // Stars
    rng_state = 7777;
    for (int i = 0; i < 300; i++) {
        int sx = (int)(rng() % (uint32_t)fb_width);
        int sy = (int)(rng() % (uint32_t)(fb_height - 80));
        int bright = 100 + (int)(rng() % 156);
        uint32_t sc = 0xFF000000 | (bright<<16) | (bright<<8) | bright;
        put_pixel(sx, sy, sc);
        if (i % 8 == 0 && bright > 190) {
            put_pixel(sx+1,sy,sc); put_pixel(sx,sy+1,sc); put_pixel(sx+1,sy+1,sc);
        }
    }
    rng_state = 3333;
    uint32_t acols[] = {0xFF7799FF,0xFFFFAACC,0xFF99FFDD,0xFFFFEE99};
    for (int i = 0; i < 12; i++) {
        int sx = (int)(rng()%(uint32_t)fb_width);
        int sy = (int)(rng()%(uint32_t)(fb_height-200));
        uint32_t c = acols[i%4];
        put_pixel(sx,sy,c); put_pixel(sx+1,sy,c);
    }
}

// Mouse
void save_mouse_bg(int x, int y) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            mouse_bg[j*CUR_W+i] = get_pixel(x+i, y+j);
    mouse_bg_saved = 1;
}
void restore_mouse_bg(int x, int y) {
    if (!mouse_bg_saved) return;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            put_pixel(x+i, y+j, mouse_bg[j*CUR_W+i]);
}
void render_cursor_graphic(int x, int y) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            uint8_t p = cursor_bmp[j][i];
            if (p == 1) put_pixel(x+i, y+j, 0xFFFFFFFF);       // white outline
            else if (p == 2) put_pixel(x+i, y+j, 0xFF000000);  // black fill
        }
}
