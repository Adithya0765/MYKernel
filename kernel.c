// kernel.c - Alteo OS v5.0 Desktop Environment
#include "graphics.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"
#include "idt.h"
#include "irq.h"
#include "isr.h"
#include "pmm.h"
#include "heap.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"
#include "vfs.h"
#include "e1000.h"
#include "ethernet.h"
#include "ip.h"
#include "tcp.h"
#include "socket.h"
#include "ac97.h"
#include "gdt.h"
#include "vmm.h"
#include "elf.h"
#include "pe.h"
#include "ata.h"
#include "fat32.h"
#include "pci.h"
#include "acpi.h"
#include "apic.h"
#include "usb.h"
#include "usb_hid.h"
#include "xhci.h"
#include "blkdev.h"
#include "pipe.h"
#include "signal.h"
#include "shm.h"
#include "devfs.h"
#include "procfs.h"
#include "ext2.h"
#include "gpu.h"
#include "nv_display.h"
#include "nv_2d.h"
#include "nv_fifo.h"
#include "nv_3d.h"
#include "nv_mem.h"
#include "nv_power.h"
#include "opengl.h"
#include "compositor.h"

#define SCR_W 1024
#define SCR_H 768
#define TASKBAR_H 40
#define TASKBAR_Y (SCR_H - TASKBAR_H)
#define START_BTN_W 90
#define MENU_W 300
#define MENU_H 520
#define MENU_X 0
#define MENU_Y (TASKBAR_Y - MENU_H)
#define MENU_ITEM_H 36
#define TITLE_H 32
#define MAX_WINDOWS 12

#define APP_TERMINAL   0
#define APP_ABOUT      1
#define APP_SYSINFO    2
#define APP_CALCULATOR 3
#define APP_FILES      4
#define APP_SETTINGS   5
#define APP_SCRIBE     6
#define APP_PAINT      7
#define APP_CLOCK      8
#define APP_BROWSER    9
#define NUM_APPS       10

typedef struct {
    int active, minimized, maximized;
    int x, y, w, h;
    int ox, oy, ow, oh;
    int app_type;
    char title[32];
} window_t;

static window_t windows[MAX_WINDOWS];
static int window_count = 0;
static int focused_win = -1;
static int start_menu_open = 0;
static int dragging = 0, drag_win = -1;
static int drag_ox, drag_oy;
static int mx = 512, my = 384, old_mx = 512, old_my = 384;
static int mouse_left = 0, mouse_left_prev = 0;
static int mouse_right = 0, mouse_right_prev = 0;
static int ctx_open = 0, ctx_x, ctx_y;

// Terminal
#define TERM_LINES 30
#define TERM_COLS 70
static char term_buf[TERM_LINES][TERM_COLS+1];
static int term_row = 0, term_col = 0;
static char term_input[128];
static int term_input_len = 0;

// Calculator
static char calc_display[32] = "0";
static int calc_val1 = 0, calc_val2 = 0, calc_op = 0, calc_new = 1;

// Scribe (text editor)
static char scribe_buf[4096];
static int scribe_len = 0;

// Paint
static uint32_t paint_canvas[200*150];
static int paint_inited = 0;
static uint32_t paint_color = 0xFF000000;

static uint32_t tick_count = 0;
static int sys_hour = 12, sys_min = 0, sys_sec = 0;

static const char* app_names[NUM_APPS] = {
    "Terminal", "About Alteo", "System Info", "Calculator",
    "Files", "Settings", "Scribe", "Paint", "Clock", "Surf"
};

// Port I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Helpers
static int my_strlen(const char* s) { int l=0; while(s[l]) l++; return l; }
static void my_strcpy(char* d, const char* s) { while((*d++=*s++)); }
static int my_strcmp(const char* a, const char* b) {
    while(*a && *a==*b) { a++; b++; } return *(unsigned char*)a - *(unsigned char*)b;
}
static int my_strncmp(const char* a, const char* b, int n) {
    for(int i=0;i<n;i++) { if(a[i]!=b[i]) return a[i]-b[i]; if(!a[i]) return 0; } return 0;
}
static void my_memset(void* d, int v, int n) {
    unsigned char* p = (unsigned char*)d; for(int i=0;i<n;i++) p[i]=(unsigned char)v;
}
static void int_to_str(int val, char* buf) {
    if (val < 0) { *buf++ = '-'; val = -val; }
    if (val == 0) { buf[0]='0'; buf[1]=0; return; }
    char tmp[16]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i-1-j];
    buf[i] = 0;
}
static void two_digit(int v, char* b) { b[0]='0'+v/10; b[1]='0'+v%10; }

// ---- Terminal ----
static void term_clear(void) {
    for (int i = 0; i < TERM_LINES; i++) my_memset(term_buf[i], 0, TERM_COLS+1);
    term_row = 0; term_col = 0;
}
static void term_putchar(char c) {
    if (c == '\n') {
        term_col = 0; term_row++;
        if (term_row >= TERM_LINES) {
            for (int i = 0; i < TERM_LINES-1; i++) my_strcpy(term_buf[i], term_buf[i+1]);
            my_memset(term_buf[TERM_LINES-1], 0, TERM_COLS+1);
            term_row = TERM_LINES - 1;
        }
        return;
    }
    if (c == '\b') { if (term_col > 0) { term_col--; term_buf[term_row][term_col] = 0; } return; }
    if (term_col < TERM_COLS) { term_buf[term_row][term_col] = c; term_col++; }
}
static void term_print(const char* s) { while (*s) term_putchar(*s++); }
static void term_prompt(void) { term_print("alteo> "); }

static void term_exec(void) {
    term_input[term_input_len] = 0;
    term_putchar('\n');
    if (my_strcmp(term_input, "help") == 0) {
        term_print("Commands: help, clear, about, uname, uptime,\n");
        term_print("  echo <text>, date, whoami, ls, cat <file>,\n");
        term_print("  cd <dir>, pwd, mkdir <dir>, touch <file>,\n");
        term_print("  rm <file>, mem, cpu, ps, neofetch, exit\n");
    } else if (my_strcmp(term_input, "clear") == 0) {
        term_clear();
    } else if (my_strcmp(term_input, "about") == 0) {
        term_print("Alteo OS v5.0 - 64-bit Graphical OS\n");
        term_print("Built from scratch in C and Assembly\n");
    } else if (my_strcmp(term_input, "uname") == 0) {
        term_print("Alteo x86_64 v5.0 (bare-metal)\n");
    } else if (my_strcmp(term_input, "whoami") == 0) {
        term_print("root\n");
    } else if (my_strcmp(term_input, "date") == 0) {
        char tb[16];
        two_digit(sys_hour, tb); tb[2]=':';
        two_digit(sys_min, tb+3); tb[5]=':';
        two_digit(sys_sec, tb+6); tb[8]=0;
        term_print(tb); term_print(" UTC 2025\n");
    } else if (my_strcmp(term_input, "uptime") == 0) {
        char tb[16]; int secs = (int)(tick_count / 100);
        int_to_str(secs, tb);
        term_print(tb); term_print(" seconds\n");
    } else if (my_strcmp(term_input, "ls") == 0) {
        vfs_dirent_t ls_entries[32];
        const char* ls_dir = vfs_getcwd();
        int ls_count = vfs_readdir(ls_dir, ls_entries, 32);
        for (int li = 0; li < ls_count; li++) {
            term_print(ls_entries[li].name);
            if (ls_entries[li].type == VFS_DIRECTORY) term_print("/");
            term_print("  ");
            if ((li + 1) % 4 == 0) term_print("\n");
        }
        if (ls_count % 4 != 0) term_print("\n");
    } else if (my_strncmp(term_input, "cat ", 4) == 0) {
        int cat_fd = vfs_open(term_input + 4, VFS_O_RDONLY);
        if (cat_fd >= 0) {
            char cat_buf[512];
            int cat_n = vfs_read(cat_fd, cat_buf, 511);
            if (cat_n > 0) { cat_buf[cat_n] = 0; term_print(cat_buf); }
            if (cat_n >= 0 && cat_buf[cat_n > 0 ? cat_n - 1 : 0] != '\n') term_print("\n");
            vfs_close(cat_fd);
        } else { term_print("cat: file not found\n"); }
    } else if (my_strncmp(term_input, "cd ", 3) == 0) {
        if (vfs_chdir(term_input + 3) < 0) term_print("cd: no such directory\n");
    } else if (my_strcmp(term_input, "pwd") == 0) {
        term_print(vfs_getcwd()); term_print("\n");
    } else if (my_strncmp(term_input, "mkdir ", 6) == 0) {
        if (vfs_mkdir(term_input + 6) < 0) term_print("mkdir: failed\n");
    } else if (my_strncmp(term_input, "touch ", 6) == 0) {
        if (vfs_create(term_input + 6, VFS_FILE, VFS_PERM_READ | VFS_PERM_WRITE) < 0) term_print("touch: failed\n");
    } else if (my_strncmp(term_input, "rm ", 3) == 0) {
        if (vfs_delete(term_input + 3) < 0) term_print("rm: failed\n");
    } else if (my_strncmp(term_input, "echo ", 5) == 0) {
        term_print(term_input + 5); term_putchar('\n');
    } else if (my_strcmp(term_input, "mem") == 0) {
        term_print("Total: 512 MB\nUsed:  24 MB\nFree:  488 MB\n");
    } else if (my_strcmp(term_input, "cpu") == 0) {
        term_print("CPU: x86_64 Long Mode\nCores: 1\nFreq: ~2 GHz\n");
    } else if (my_strcmp(term_input, "neofetch") == 0) {
        term_print("     /\\\\\\\\\n");
        term_print("    /  \\\\\\\\\n");
        term_print("   / /\\ \\\\\\\n");
        term_print("  / /__\\ \\\\\n");
        term_print(" /________\\\\\n");
        term_print("  ALTEO OS\n");
        term_print("\n");
        term_print("  OS: Alteo v5.0 x86_64\n");
        term_print("  Kernel: Custom bare-metal\n");
        term_print("  Shell: AlteoTerm\n");
        term_print("  Resolution: 1024x768\n");
        term_print("  Theme: Space Nebula\n");
        term_print("  Processes: ");
        { char tb2[16]; int_to_str(process_count(), tb2); term_print(tb2); }
        term_print(" running\n");
    } else if (my_strcmp(term_input, "ps") == 0) {
        term_print("PID  STATE    PRIO NAME\n");
        term_print("---  -------  ---- --------\n");
        process_t* pt = process_get_table();
        int mx2 = process_get_max();
        for (int pi = 0; pi < mx2; pi++) {
            if (pt[pi].state == PROC_STATE_UNUSED) continue;
            char pb[8]; int_to_str(pt[pi].pid, pb);
            term_print(pb);
            // Padding
            int pl = my_strlen(pb);
            for (int sp = 0; sp < 5 - pl; sp++) term_print(" ");
            term_print(process_state_name(pt[pi].state));
            int sl = my_strlen(process_state_name(pt[pi].state));
            for (int sp = 0; sp < 9 - sl; sp++) term_print(" ");
            char prb[8]; int_to_str(pt[pi].priority, prb);
            term_print(prb);
            int prl = my_strlen(prb);
            for (int sp = 0; sp < 5 - prl; sp++) term_print(" ");
            term_print(pt[pi].name);
            term_print("\n");
        }
    } else if (my_strcmp(term_input, "exit") == 0) {
        for (int i = 0; i < window_count; i++)
            if (windows[i].active && windows[i].app_type == APP_TERMINAL) { windows[i].active = 0; break; }
    } else if (term_input_len > 0) {
        term_print(term_input); term_print(": command not found\n");
    }
    term_input_len = 0;
    term_prompt();
}

// ---- Window management ----
static int create_window(int app_type, const char* title, int w, int h) {
    int idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) { if (!windows[i].active) { idx = i; break; } }
    if (idx < 0) return -1;
    // Single-instance check
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].app_type == app_type) {
            windows[i].minimized = 0; focused_win = i; return i;
        }
    }
    window_t* win = &windows[idx];
    win->active = 1; win->minimized = 0; win->maximized = 0; win->app_type = app_type;
    win->w = w; win->h = h;
    win->x = (SCR_W - w) / 2 + (idx * 25) % 100;
    win->y = 40 + (idx * 30) % 100;
    if (win->x + w > SCR_W - 10) win->x = 20;
    if (win->y + h > TASKBAR_Y - 10) win->y = 20;
    win->ox = win->x; win->oy = win->y; win->ow = w; win->oh = h;
    int tl = my_strlen(title);
    if (tl > 30) tl = 30;
    for (int i = 0; i < tl; i++) win->title[i] = title[i];
    win->title[tl] = 0;
    if (app_type == APP_TERMINAL) { term_clear(); term_print("Alteo Terminal v5.0\n"); term_print("Type 'help' for commands.\n\n"); term_prompt(); }
    if (app_type == APP_SCRIBE) { scribe_len = 0; my_memset(scribe_buf, 0, 4096); }
    if (app_type == APP_CALCULATOR) { my_strcpy(calc_display, "0"); calc_val1=0; calc_val2=0; calc_op=0; calc_new=1; }
    if (app_type == APP_PAINT && !paint_inited) { for (int i=0;i<200*150;i++) paint_canvas[i]=0xFFFFFFFF; paint_inited=1; }
    focused_win = idx;
    if (idx >= window_count) window_count = idx + 1;
    return idx;
}

static void close_window(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    windows[idx].active = 0;
    if (focused_win == idx) {
        focused_win = -1;
        for (int i = MAX_WINDOWS-1; i >= 0; i--)
            if (windows[i].active && !windows[i].minimized) { focused_win = i; break; }
    }
}

static void toggle_maximize(int idx) {
    window_t* w = &windows[idx];
    if (w->maximized) { w->x=w->ox; w->y=w->oy; w->w=w->ow; w->h=w->oh; w->maximized=0; }
    else { w->ox=w->x; w->oy=w->y; w->ow=w->w; w->oh=w->h; w->x=0; w->y=0; w->w=SCR_W; w->h=TASKBAR_Y; w->maximized=1; }
}

// ---- Icons (24x24 detailed) ----
static void draw_vline(int x, int y, int h, uint32_t c) {
    for (int i = 0; i < h; i++) put_pixel(x, y+i, c);
}

// Terminal icon - dark screen with green prompt
static void draw_icon_terminal(int x, int y, int sz) {
    int w = sz, h = sz * 3 / 4;
    // Outer bezel
    draw_rounded_rect(x, y+(sz-h)/2, w, h, 0xFF333350);
    // Screen
    draw_rect(x+2, y+(sz-h)/2+2, w-4, h-4, 0xFF0A0A18);
    // Prompt
    int ty = y + (sz-h)/2 + 4;
    draw_string(x+4, ty, ">", 0xFF00DD66);
    draw_rect(x+12, ty+10, 6, 2, 0xFF00DD66); // underscore
    // Title bar dots
    put_pixel(x+3, y+(sz-h)/2+1, 0xFFFF5555);
    put_pixel(x+6, y+(sz-h)/2+1, 0xFFFFBB33);
    put_pixel(x+9, y+(sz-h)/2+1, 0xFF55CC55);
}

// Folder icon - classic yellow folder
static void draw_icon_folder(int x, int y, int sz) {
    int w = sz, h = sz * 3 / 4;
    int fy = y + (sz - h) / 2;
    // Tab
    draw_rect(x+1, fy, w/3, 4, 0xFFD4A030);
    // Body back
    draw_rounded_rect(x, fy+3, w, h-3, 0xFFE8B84D);
    // Body front
    draw_rect(x+1, fy+6, w-2, h-8, 0xFFF5D060);
    // Highlight
    draw_hline(x+2, fy+4, w-4, 0xFFFAE088);
}

// Gear/Settings icon
static void draw_icon_gear(int x, int y, int sz) {
    int cx = x + sz/2, cy = y + sz/2;
    int r1 = sz/2 - 1, r2 = sz/3;
    // Outer ring with teeth
    draw_circle(cx, cy, r1, 0xFF8899AA);
    // Teeth (N, S, E, W, NE, NW, SE, SW)
    int t = sz/6;
    draw_rect(cx-t/2, cy-r1-1, t, 3, 0xFF8899AA);
    draw_rect(cx-t/2, cy+r1-1, t, 3, 0xFF8899AA);
    draw_rect(cx-r1-1, cy-t/2, 3, t, 0xFF8899AA);
    draw_rect(cx+r1-1, cy-t/2, 3, t, 0xFF8899AA);
    // Inner circle
    draw_circle(cx, cy, r2, 0xFF556677);
    draw_circle(cx, cy, r2-2, 0xFF8899AA);
}

// Calculator icon
static void draw_icon_calc(int x, int y, int sz) {
    draw_rounded_rect(x+1, y+1, sz-2, sz-2, 0xFF3377BB);
    // Display
    draw_rect(x+3, y+3, sz-6, sz/3-1, 0xFFDDEEFF);
    // Button grid
    int by = y + sz/3 + 2;
    int bw = (sz-8)/3, bh = (sz - sz/3 - 6) / 3;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            draw_rect(x+3+c*(bw+1), by+r*(bh+1), bw, bh, 0xFFFFFFFF);
}

// Scribe icon - quill pen on paper
static void draw_icon_scribe(int x, int y, int sz) {
    // Paper background
    draw_rect(x+2, y+3, sz-5, sz-4, 0xFFF8F0E0);
    // Paper fold
    draw_rect(x+sz-6, y+3, 3, 3, 0xFFE0D8B0);
    // Lines on paper
    for (int i = 0; i < 3; i++)
        draw_hline(x+5, y+sz/2+i*3, sz-12, 0xFFCCBB88);
    // Quill pen (diagonal line)
    for (int i = 0; i < sz/2; i++) {
        draw_rect(x+sz-4-i, y+1+i, 2, 2, 0xFF3366CC);
    }
    // Pen tip
    draw_rect(x+sz/2+1, y+sz/2-1, 2, 2, 0xFF222222);
}

// Paint icon - palette with colors
static void draw_icon_paint(int x, int y, int sz) {
    draw_rounded_rect(x+1, y+1, sz-2, sz-2, 0xFFFFFFFF);
    // Border
    draw_hline(x+1, y+1, sz-2, 0xFFCCCCCC);
    draw_hline(x+1, y+sz-2, sz-2, 0xFFCCCCCC);
    draw_vline(x+1, y+1, sz-2, 0xFFCCCCCC);
    draw_vline(x+sz-2, y+1, sz-2, 0xFFCCCCCC);
    // Color spots
    int cs = sz / 5;
    draw_circle(x+sz/4, y+sz/3, cs, 0xFFFF3333);
    draw_circle(x+sz*3/4, y+sz/3, cs, 0xFF3333FF);
    draw_circle(x+sz/3, y+sz*2/3, cs, 0xFF33CC33);
    draw_circle(x+sz*2/3, y+sz*2/3, cs, 0xFFFFCC00);
}

// Clock icon
static void draw_icon_clock_small(int x, int y, int sz) {
    int cx = x + sz/2, cy = y + sz/2;
    int r = sz/2 - 1;
    draw_circle(cx, cy, r, 0xFFDDDDFF);
    draw_circle(cx, cy, r-2, 0xFF1A1A33);
    // 12, 3, 6, 9 marks
    draw_rect(cx-1, cy-r+3, 2, 3, 0xFFFFFFFF);
    draw_rect(cx+r-5, cy-1, 3, 2, 0xFFFFFFFF);
    draw_rect(cx-1, cy+r-5, 2, 3, 0xFFFFFFFF);
    draw_rect(cx-r+3, cy-1, 3, 2, 0xFFFFFFFF);
    // Hour hand (pointing ~2)
    for (int i = 0; i < r/2; i++) put_pixel(cx, cy-i, 0xFFFFFFFF);
    // Minute hand (pointing ~10)
    for (int i = 0; i < r*2/3; i++) put_pixel(cx+i*2/3, cy-i, 0xFF88AAFF);
    // Center dot
    put_pixel(cx, cy, 0xFFFF4444);
}

// Info icon
static void draw_icon_info(int x, int y, int sz) {
    int cx = x + sz/2, cy = y + sz/2;
    draw_circle(cx, cy, sz/2-1, 0xFF2277DD);
    // "i" letter
    draw_rect(cx-1, cy-sz/4, 2, 2, 0xFFFFFFFF); // dot
    draw_rect(cx-1, cy-sz/4+4, 2, sz/3, 0xFFFFFFFF); // stem
}

// About/Alteo icon - diamond logo
static void draw_icon_about(int x, int y, int sz) {
    int cx = x + sz/2, cy = y + sz/2;
    draw_circle(cx, cy, sz/2-1, 0xFF5533AA);
    // Diamond shape inside
    int d = sz/4;
    for (int i = 0; i <= d; i++) {
        draw_hline(cx-i, cy-d+i, i*2+1, 0xFFDDBBFF);
        draw_hline(cx-i, cy+d-i, i*2+1, 0xFFDDBBFF);
    }
}

// Browser state
static char browser_url[256] = "alteo://home";
static int browser_url_len = 12;
static int browser_url_editing = 0;

// Browser icon - globe with meridians
static void draw_icon_browser(int x, int y, int sz) {
    int cx = x + sz/2, cy = y + sz/2;
    int r = sz/2 - 1;
    // Globe circle
    draw_circle(cx, cy, r, 0xFF2288DD);
    draw_circle(cx, cy, r-1, 0xFF33AAEE);
    // Horizontal lines (latitudes)
    draw_hline(cx - r + 2, cy, r*2 - 4, 0xFF1166AA);
    draw_hline(cx - r + 4, cy - r/3, r*2 - 8, 0xFF1166AA);
    draw_hline(cx - r + 4, cy + r/3, r*2 - 8, 0xFF1166AA);
    // Vertical meridian
    draw_vline(cx, cy - r + 2, r*2 - 4, 0xFF1166AA);
    // Ellipse-like meridians
    for (int i = -r+3; i <= r-3; i++) {
        int off = (i*i*r)/(r*r*3);
        if (cx - r/3 + off >= x && cx - r/3 + off < x + sz)
            draw_rect(cx - r/3 + off, cy + i, 1, 1, 0xFF1166AA);
        if (cx + r/3 - off >= x && cx + r/3 - off < x + sz)
            draw_rect(cx + r/3 - off, cy + i, 1, 1, 0xFF1166AA);
    }
}

// Dispatch icon drawing with size parameter
static void draw_app_icon(int app_type, int x, int y, int sz) {
    switch (app_type) {
        case APP_TERMINAL: draw_icon_terminal(x, y, sz); break;
        case APP_ABOUT: draw_icon_about(x, y, sz); break;
        case APP_SYSINFO: draw_icon_info(x, y, sz); break;
        case APP_CALCULATOR: draw_icon_calc(x, y, sz); break;
        case APP_FILES: draw_icon_folder(x, y, sz); break;
        case APP_SETTINGS: draw_icon_gear(x, y, sz); break;
        case APP_SCRIBE: draw_icon_scribe(x, y, sz); break;
        case APP_PAINT: draw_icon_paint(x, y, sz); break;
        case APP_CLOCK: draw_icon_clock_small(x, y, sz); break;
        case APP_BROWSER: draw_icon_browser(x, y, sz); break;
    }
}

// ---- ALTEO Logo (draws the Alteo diamond brand mark) ----
static void draw_alteo_logo(int x, int y, int size, uint32_t color) {
    // Diamond/chevron shape
    int half = size / 2;
    for (int i = 0; i <= half; i++) {
        draw_hline(x + half - i, y + i, i * 2 + 1, color);
    }
    for (int i = 0; i < half; i++) {
        draw_hline(x + half - (half-1-i), y + half + 1 + i, (half-1-i)*2+1, color);
    }
}

// ---- Window chrome ----
static void draw_window_frame(window_t* w) {
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    int is_focused = (focused_win >= 0 && &windows[focused_win] == w);

    // Drop shadow (deeper for focused windows)
    draw_shadow(x, y, ww, wh, is_focused ? 12 : 6);

    // Window body (slightly transparent glass)
    draw_rounded_rect_alpha(x, y, ww, wh, 0xFF1C1C2C, 250);

    // Title bar with gradient
    for (int ty = 0; ty < TITLE_H; ty++) {
        int r, g, b;
        if (is_focused) {
            r = 38 + (ty * 8) / TITLE_H;
            g = 38 + (ty * 8) / TITLE_H;
            b = 68 + (ty * 12) / TITLE_H;
        } else {
            r = 30 + (ty * 5) / TITLE_H;
            g = 30 + (ty * 5) / TITLE_H;
            b = 45 + (ty * 5) / TITLE_H;
        }
        uint32_t tc = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int tx2 = 1; tx2 < ww - 1; tx2++)
            put_pixel_alpha(x + tx2, y + 1 + ty, tc, 252);
    }
    // Title bar bottom separator with subtle glow
    for (int tx2 = 1; tx2 < ww - 1; tx2++) {
        put_pixel_alpha(x + tx2, y + TITLE_H, 0xFF445588, is_focused ? 160 : 80);
        put_pixel_alpha(x + tx2, y + TITLE_H + 1, 0xFF222244, 60);
    }

    // Title icon (16px in title bar)
    draw_app_icon(w->app_type, x + 10, y + 8, 16);

    // Title text with subtle shadow
    if (is_focused) {
        draw_string(x + 32, y + 10, w->title, 0xFF111122);  // shadow
    }
    uint32_t title_col = is_focused ? 0xFFE8E8FF : 0xFF888899;
    draw_string(x + 31, y + 9, w->title, title_col);

    // Window control buttons (macOS style traffic lights, refined)
    int bx = x + ww - 28, by = y + 10;
    // Close (red) with smooth circle
    draw_circle(bx + 6, by + 6, 7, is_focused ? 0xFFEE4455 : 0xFF553333);
    if (is_focused) {
        draw_circle(bx + 6, by + 6, 6, 0xFFFF5566);
        // X mark
        for (int d = 0; d < 5; d++) {
            put_pixel(bx + 3 + d, by + 3 + d, 0xAA000000);
            put_pixel(bx + 7 - d, by + 3 + d, 0xAA000000);
        }
    }
    // Maximize (yellow)
    bx -= 20;
    draw_circle(bx + 6, by + 6, 7, is_focused ? 0xFFCC9922 : 0xFF444433);
    if (is_focused) {
        draw_circle(bx + 6, by + 6, 6, 0xFFDDAA33);
        draw_rect(bx + 3, by + 3, 6, 6, 0x88553300);
    }
    // Minimize (green)
    bx -= 20;
    draw_circle(bx + 6, by + 6, 7, is_focused ? 0xFF33AA44 : 0xFF334433);
    if (is_focused) {
        draw_circle(bx + 6, by + 6, 6, 0xFF44BB55);
        draw_hline(bx + 3, by + 6, 6, 0x88004400);
    }

    // Focused window glow on border
    if (is_focused) {
        draw_rounded_rect_border(x, y, ww, wh, 0xFF4466AA, 60);
    }
}

// ---- App renderers ----
static void render_terminal(window_t* w) {
    int cx = w->x + 8, cy = w->y + TITLE_H + 6;
    int max_vis = (w->h - TITLE_H - 12) / 18;
    if (max_vis > TERM_LINES) max_vis = TERM_LINES;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF0A0A14);
    for (int i = 0; i < max_vis && i <= term_row; i++)
        draw_string(cx, cy + i*18, term_buf[i], 0xFF00DD66);
    if ((tick_count / 30) % 2 == 0)
        draw_rect(cx + term_col*8, cy + term_row*18, 8, 16, 0xFF00DD66);
}

static void render_about(window_t* w) {
    int cx = w->x + 20, cy = w->y + TITLE_H + 20;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF1A1A2E);

    // ALTEO Logo - large centered diamond
    draw_alteo_logo(cx + 40, cy, 40, 0xFF5588DD);
    draw_alteo_logo(cx + 43, cy + 6, 34, 0xFF3366BB);
    // "A" in center of logo
    draw_string(cx + 56, cy + 12, "A", 0xFFFFFFFF);

    cy += 50;
    draw_string(cx, cy, "ALTEO OS", 0xFFFFFFFF); cy += 24;
    draw_string(cx, cy, "Version 5.0", 0xFF88AAFF); cy += 24;
    draw_string(cx, cy, "64-bit Graphical Operating System", 0xFFAAAAAA); cy += 20;
    draw_string(cx, cy, "Built from scratch in C & ASM", 0xFFAAAAAA); cy += 28;
    draw_hline(cx, cy, w->w - 40, 0xFF333355); cy += 12;
    draw_string(cx, cy, "Features:", 0xFFDDDDFF); cy += 20;
    draw_string(cx+8, cy, "- x86_64 Long Mode Kernel", 0xFF999999); cy += 18;
    draw_string(cx+8, cy, "- PS/2 Keyboard & Mouse Input", 0xFF999999); cy += 18;
    draw_string(cx+8, cy, "- Window Manager & Desktop", 0xFF999999); cy += 18;
    draw_string(cx+8, cy, "- 1024x768 Double-Buffered VESA", 0xFF999999); cy += 18;
    draw_string(cx+8, cy, "- 9 Built-in Applications", 0xFF999999); cy += 18;
}

static void render_sysinfo(window_t* w) {
    int cx = w->x + 20, cy = w->y + TITLE_H + 16;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF1A1A2E);
    draw_string(cx, cy, "System Information", 0xFFFFFFFF); cy += 26;
    draw_hline(cx, cy, w->w-40, 0xFF333355); cy += 10;
    draw_string(cx, cy, "OS:          Alteo v5.0", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Kernel:      Custom x86_64", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Arch:        x86_64 Long Mode", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Display:     1024x768 32bpp", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Memory:      512 MB", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Keyboard:    PS/2 Scancode Set 1", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Mouse:       PS/2 3-byte Protocol", 0xFFCCCCCC); cy += 20;
    // Uptime
    char tb[32]; int secs = (int)(tick_count / 100);
    draw_string(cx, cy, "Uptime:      ", 0xFFCCCCCC);
    int_to_str(secs, tb);
    draw_string(cx + 13*8, cy, tb, 0xFF88CCFF);
    draw_string(cx + 13*8 + my_strlen(tb)*8, cy, "s", 0xFF88CCFF); cy += 28;
    // RAM bar
    draw_string(cx, cy, "RAM Usage:", 0xFFCCCCCC); cy += 20;
    draw_rounded_rect(cx, cy, 200, 18, 0xFF333355);
    draw_rounded_rect(cx, cy, 40, 18, 0xFF44AA66);
    draw_string(cx+210, cy+1, "24 / 512 MB", 0xFF888888); cy += 28;
    // Process info
    draw_string(cx, cy, "Processes:   ", 0xFFCCCCCC);
    { char pb[16]; int_to_str(process_count(), pb);
      draw_string(cx + 13*8, cy, pb, 0xFF88CCFF); }
    cy += 20;
    draw_string(cx, cy, "Scheduler:   Priority-based", 0xFFCCCCCC); cy += 20;
    draw_string(cx, cy, "Context SW:  ", 0xFFCCCCCC);
    { char sb[16]; scheduler_stats_t ss2 = scheduler_get_stats();
      int_to_str((int)ss2.total_switches, sb);
      draw_string(cx + 13*8, cy, sb, 0xFF88CCFF); }
}

static void render_calculator(window_t* w) {
    int cx = w->x + 12, cy = w->y + TITLE_H + 12;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF1E1E30);
    // Display
    draw_rounded_rect(cx, cy, w->w-24, 44, 0xFF0D0D1A);
    int dl = my_strlen(calc_display);
    draw_string(cx+w->w-32-dl*8, cy+14, calc_display, 0xFF44FFAA);
    cy += 54;
    // Buttons
    const char* btns[] = {"C","(",")","/","7","8","9","*","4","5","6","-","1","2","3","+","0",".","<","="};
    int bw = (w->w - 24 - 12) / 4, bh = 38;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = cx + c*(bw+4), by = cy + r*(bh+4), idx2 = r*4+c;
            uint32_t bg = 0xFF2A2A44;
            if (idx2 < 4) bg = 0xFF3A3A55;
            if (idx2==3||idx2==7||idx2==11||idx2==15) bg = 0xFF4466AA;
            if (idx2 == 19) bg = 0xFF44AA66;
            if (idx2 == 0) bg = 0xFFAA4444;
            // Hover
            if (mx>=bx && mx<bx+bw && my>=by && my<by+bh) {
                int hr = ((bg>>16)&0xFF)*5/4; if(hr>255) hr=255;
                int hg = ((bg>>8)&0xFF)*5/4; if(hg>255) hg=255;
                int hb = (bg&0xFF)*5/4; if(hb>255) hb=255;
                bg = 0xFF000000|(hr<<16)|(hg<<8)|hb;
            }
            draw_rounded_rect(bx, by, bw, bh, bg);
            int tl2 = my_strlen(btns[idx2]);
            draw_string(bx+(bw-tl2*8)/2, by+(bh-16)/2, btns[idx2], 0xFFFFFFFF);
        }
    }
}

static char files_cwd[256] = "/home/user";
static int files_scroll = 0;
static int files_selected = -1;
static int files_view_mode = 0; // 0=list, 1=grid

static void render_files(window_t* w) {
    int cx = w->x + 1, cy = w->y + TITLE_H + 1;
    int cw = w->w - 2, ch = w->h - TITLE_H - 2;
    draw_rect(cx, cy, cw, ch, 0xFF161624);

    // ---- Toolbar ----
    int tb_y = cy + 2;
    draw_rounded_rect_alpha(cx + 4, tb_y, cw - 8, 32, 0xFF1E1E30, 220);
    draw_rounded_rect_border(cx + 4, tb_y, cw - 8, 32, 0xFF2A2A44, 100);

    // Back button
    int back_enabled = (my_strcmp(files_cwd, "/") != 0);
    {
        int bx = cx + 10, by = tb_y + 4;
        int bh = (mx>=bx && mx<bx+28 && my>=by && my<by+24 && back_enabled);
        draw_rounded_rect(bx, by, 28, 24, bh ? 0xFF334466 : 0xFF252540);
        draw_string(bx + 8, by + 4, "<", back_enabled ? (bh ? 0xFFFFFFFF : 0xFF99AACC) : 0xFF444455);
    }

    // Home button
    {
        int bx = cx + 44, by = tb_y + 4;
        int bh = (mx>=bx && mx<bx+28 && my>=by && my<by+24);
        draw_rounded_rect(bx, by, 28, 24, bh ? 0xFF334466 : 0xFF252540);
        draw_string(bx + 6, by + 4, "~", bh ? 0xFFFFFFFF : 0xFF99AACC);
    }

    // Separator
    draw_vline(cx + 80, tb_y + 6, 20, 0xFF333355);

    // Path bar
    int pb_x = cx + 88;
    int pb_w = cw - 196;
    draw_rounded_rect(pb_x, tb_y + 4, pb_w, 24, 0xFF0E0E1C);
    draw_rounded_rect_border(pb_x, tb_y + 4, pb_w, 24, 0xFF2A2A44, 80);
    // Draw path with folder breadcrumbs
    {
        int px = pb_x + 8;
        char seg[64]; int si = 0;
        for (int i = 0; files_cwd[i]; i++) {
            if (files_cwd[i] == '/' && si > 0) {
                seg[si] = 0;
                draw_string(px, tb_y + 8, seg, 0xFF88AADD);
                px += si * 8;
                draw_string(px, tb_y + 8, " > ", 0xFF556677);
                px += 24;
                si = 0;
            } else if (files_cwd[i] != '/') {
                if (si < 60) seg[si++] = files_cwd[i];
            }
        }
        if (si > 0) { seg[si] = 0; draw_string(px, tb_y + 8, seg, 0xFF88AADD); }
        else if (files_cwd[0] == '/' && files_cwd[1] == 0)
            draw_string(pb_x + 8, tb_y + 8, "/", 0xFF88AADD);
    }

    // View toggle button
    {
        int vx = cx + cw - 100, vy = tb_y + 4;
        int vh = (mx>=vx && mx<vx+28 && my>=vy && my<vy+24);
        draw_rounded_rect(vx, vy, 28, 24, vh ? 0xFF334466 : 0xFF252540);
        draw_string(vx + 5, vy + 4, files_view_mode ? "=" : "#", vh ? 0xFFFFFFFF : 0xFF99AACC);
    }

    // New folder button
    {
        int nx = cx + cw - 66, ny = tb_y + 4;
        int nh = (mx>=nx && mx<nx+56 && my>=ny && my<ny+24);
        draw_rounded_rect(nx, ny, 56, 24, nh ? 0xFF334466 : 0xFF252540);
        draw_string(nx + 5, ny + 4, "+ New", nh ? 0xFFFFFFFF : 0xFF99AACC);
    }

    // ---- Sidebar ----
    int sidebar_w = 130;
    int content_y = cy + 40;
    int content_h = ch - 68; // leave room for status bar
    draw_rect(cx + 4, content_y, sidebar_w, content_h, 0xFF131320);
    draw_rounded_rect_border(cx + 4, content_y, sidebar_w, content_h, 0xFF222240, 60);

    // Sidebar sections
    int sy = content_y + 8;
    draw_string(cx + 14, sy, "Favorites", 0xFF667788); sy += 22;
    struct { const char* name; const char* path; uint32_t col; } fav[] = {
        {"Home", "/home/user", 0xFF4488CC},
        {"Documents", "/home/user/Documents", 0xFF44AA88},
        {"Downloads", "/home/user/Downloads", 0xFF8866CC},
        {"Pictures", "/home/user/Pictures", 0xFFCC8844},
        {"Music", "/home/user/Music", 0xFFCC4466},
    };
    for (int i = 0; i < 5; i++) {
        int fav_hover = (mx >= cx+10 && mx < cx+sidebar_w && my >= sy && my < sy+22);
        if (fav_hover)
            draw_rect_alpha(cx + 8, sy, sidebar_w - 8, 22, 0xFF3355AA, 40);
        // Small colored dot
        draw_rect(cx + 14, sy + 6, 8, 8, fav[i].col);
        draw_string(cx + 28, sy + 3, fav[i].name, fav_hover ? 0xFFFFFFFF : 0xFF99AABB);
        sy += 26;
    }

    sy += 10;
    draw_hline(cx + 14, sy, sidebar_w - 20, 0xFF222240); sy += 10;
    draw_string(cx + 14, sy, "System", 0xFF667788); sy += 22;
    const char* sys_items[] = {"Root /", "Dev", "Tmp", "Boot"};
    for (int i = 0; i < 4; i++) {
        int s_hover = (mx >= cx+10 && mx < cx+sidebar_w && my >= sy && my < sy+22);
        if (s_hover)
            draw_rect_alpha(cx + 8, sy, sidebar_w - 8, 22, 0xFF3355AA, 40);
        draw_rect(cx + 14, sy + 6, 8, 8, 0xFF556688);
        draw_string(cx + 28, sy + 3, sys_items[i], s_hover ? 0xFFFFFFFF : 0xFF778899);
        sy += 26;
    }

    // ---- Main content area ----
    int mc_x = cx + sidebar_w + 10;
    int mc_w = cw - sidebar_w - 18;
    draw_rect(mc_x, content_y, mc_w, content_h, 0xFF141422);

    // Column headers
    {
        int hy = content_y + 2;
        draw_rect(mc_x, hy, mc_w, 22, 0xFF1A1A30);
        draw_string(mc_x + 34, hy + 3, "Name", 0xFF667788);
        draw_string(mc_x + mc_w - 150, hy + 3, "Size", 0xFF667788);
        draw_string(mc_x + mc_w - 70, hy + 3, "Type", 0xFF667788);
        draw_hline(mc_x, hy + 22, mc_w, 0xFF222240);
    }

    // Read directory entries
    vfs_dirent_t entries[32];
    int count = vfs_readdir(files_cwd, entries, 32);
    if (count < 0) count = 0;

    int item_h = 28;
    int list_y = content_y + 26;
    int max_vis = (content_h - 28) / item_h;
    if (files_scroll > count - max_vis) files_scroll = count - max_vis;
    if (files_scroll < 0) files_scroll = 0;

    for (int i = files_scroll; i < count && (i - files_scroll) < max_vis; i++) {
        int fy = list_y + (i - files_scroll) * item_h;
        int is_d = (entries[i].type == VFS_DIRECTORY);
        int item_hover = (mx >= mc_x && mx < mc_x + mc_w && my >= fy && my < fy + item_h);
        int item_sel = (i == files_selected);

        // Selection/hover highlight
        if (item_sel)
            draw_rect_alpha(mc_x + 2, fy, mc_w - 4, item_h - 2, 0xFF2244AA, 80);
        else if (item_hover)
            draw_rect_alpha(mc_x + 2, fy, mc_w - 4, item_h - 2, 0xFF334466, 50);

        // Alternating row tint
        if ((i % 2) == 0 && !item_sel && !item_hover)
            draw_rect_alpha(mc_x + 2, fy, mc_w - 4, item_h - 2, 0xFF1A1A2A, 40);

        // Icon
        if (is_d) {
            draw_icon_folder(mc_x + 6, fy + 3, 20);
        } else {
            // File type icon based on extension
            const char* nm = entries[i].name;
            int nl = my_strlen(nm);
            int is_code = (nl > 2 && nm[nl-2] == '.' && (nm[nl-1] == 'c' || nm[nl-1] == 'h'));
            int is_text = (nl > 4 && nm[nl-4] == '.' && nm[nl-3] == 't' && nm[nl-2] == 'x' && nm[nl-1] == 't');
            int is_conf = (nl > 4 && nm[nl-4] == '.' && nm[nl-3] == 'c' && nm[nl-2] == 'f' && nm[nl-1] == 'g');
            uint32_t fcol = is_code ? 0xFF66AAFF : (is_text ? 0xFFDDCCAA : (is_conf ? 0xFF88CC66 : 0xFFCCCCDD));
            draw_rect(mc_x + 8, fy + 4, 14, 18, fcol);
            draw_rect(mc_x + 8, fy + 4, 14, 3, 0xFFAAAABB);
            // File type indicator dot
            if (is_code) draw_rect(mc_x + 12, fy + 11, 6, 4, 0xFF2244AA);
            else if (is_text) draw_hline(mc_x + 10, fy + 11, 10, 0xFFAA9966);
        }

        // Name
        draw_string(mc_x + 30, fy + 6, entries[i].name, is_d ? 0xFF88CCFF : 0xFFCCCCCC);

        // Size for files
        if (!is_d) {
            if (entries[i].size > 0) {
                char sz_buf[16]; int_to_str((int)entries[i].size, sz_buf);
                int sl = my_strlen(sz_buf);
                sz_buf[sl] = ' '; sz_buf[sl+1] = 'B'; sz_buf[sl+2] = 0;
                draw_string(mc_x + mc_w - 150, fy + 6, sz_buf, 0xFF667788);
            } else {
                draw_string(mc_x + mc_w - 150, fy + 6, "0 B", 0xFF556677);
            }
        } else {
            draw_string(mc_x + mc_w - 150, fy + 6, "--", 0xFF445566);
        }

        // Type column
        const char* type_str = is_d ? "Folder" : "File";
        draw_string(mc_x + mc_w - 70, fy + 6, type_str, 0xFF667788);
    }

    // Scrollbar if needed
    if (count > max_vis) {
        int sb_x = mc_x + mc_w - 8;
        int sb_h = content_h - 28;
        draw_rect(sb_x, list_y, 6, sb_h, 0xFF1A1A2A);
        int thumb_h = (max_vis * sb_h) / count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = list_y + (files_scroll * (sb_h - thumb_h)) / (count - max_vis);
        draw_rounded_rect(sb_x, thumb_y, 6, thumb_h, 0xFF445566);
    }

    // ---- Status bar ----
    int sb_y = cy + ch - 24;
    draw_rounded_rect_alpha(cx + 4, sb_y, cw - 8, 22, 0xFF181828, 220);
    draw_rounded_rect_border(cx + 4, sb_y, cw - 8, 22, 0xFF222240, 60);
    // Item count
    {
        char cb[24]; int_to_str(count, cb);
        int cl = my_strlen(cb);
        cb[cl] = ' '; cb[cl+1] = 'i'; cb[cl+2] = 't'; cb[cl+3] = 'e'; cb[cl+4] = 'm'; cb[cl+5] = 's'; cb[cl+6] = 0;
        draw_string(cx + 14, sb_y + 3, cb, 0xFF667788);
    }
    // Current path
    draw_string(cx + cw - 180, sb_y + 3, files_cwd, 0xFF556677);
}

static void render_settings(window_t* w) {
    int cx = w->x + 20, cy = w->y + TITLE_H + 16;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF1A1A2E);
    draw_string(cx, cy, "Settings", 0xFFFFFFFF); cy += 28;
    draw_hline(cx, cy, w->w-40, 0xFF333355); cy += 12;

    draw_string(cx, cy, "Theme", 0xFFCCCCCC);
    draw_rounded_rect(cx+180, cy-2, 130, 22, 0xFF2A2A44);
    draw_string(cx+188, cy, "Space Nebula", 0xFF88AAFF); cy+=38;

    draw_string(cx, cy, "Resolution", 0xFFCCCCCC);
    draw_rounded_rect(cx+180, cy-2, 130, 22, 0xFF2A2A44);
    draw_string(cx+188, cy, "1024 x 768", 0xFF88AAFF); cy+=38;

    draw_string(cx, cy, "Font Size", 0xFFCCCCCC);
    draw_rounded_rect(cx+180, cy-2, 130, 22, 0xFF2A2A44);
    draw_string(cx+188, cy, "8x16 VGA", 0xFF88AAFF); cy+=38;

    draw_string(cx, cy, "Mouse Speed", 0xFFCCCCCC);
    draw_rect(cx+180, cy+6, 130, 4, 0xFF333355);
    draw_circle(cx+250, cy+8, 7, 0xFF4488CC); cy+=38;

    draw_string(cx, cy, "Sound", 0xFFCCCCCC);
    draw_rounded_rect(cx+180, cy-2, 55, 22, 0xFF44AA66);
    draw_string(cx+192, cy, "On", 0xFFFFFFFF);
}

static void render_scribe(window_t* w) {
    int cx = w->x + 1, cy = w->y + TITLE_H + 1;
    int cw = w->w - 2, ch = w->h - TITLE_H - 2;
    // Background
    draw_rect(cx, cy, cw, ch, 0xFF1A1A28);
    // Toolbar
    int ty_bar = cy + 2;
    draw_rounded_rect_alpha(cx + 4, ty_bar, cw - 8, 28, 0xFF222238, 200);
    draw_rounded_rect_border(cx + 4, ty_bar, cw - 8, 28, 0xFF334466, 80);
    // File info
    draw_string(cx + 12, ty_bar + 7, "Untitled.scrb", 0xFF88AACC);
    // Word count
    {
        int wc = 0; int in_word = 0;
        for (int i = 0; i < scribe_len; i++) {
            if (scribe_buf[i] == ' ' || scribe_buf[i] == '\n') in_word = 0;
            else if (!in_word) { wc++; in_word = 1; }
        }
        char wcb[16]; int_to_str(wc, wcb);
        int wl = my_strlen(wcb);
        wcb[wl] = ' '; wcb[wl+1] = 'w'; wcb[wl+2] = 'o'; wcb[wl+3] = 'r'; wcb[wl+4] = 'd'; wcb[wl+5] = 's'; wcb[wl+6] = 0;
        draw_string(cx + cw - 100, ty_bar + 7, wcb, 0xFF667788);
    }
    // Chars
    {
        char cb[16]; int_to_str(scribe_len, cb);
        int cl = my_strlen(cb);
        cb[cl] = ' '; cb[cl+1] = 'c'; cb[cl+2] = 'h'; cb[cl+3] = 0;
        draw_string(cx + cw - 180, ty_bar + 7, cb, 0xFF667788);
    }
    // Separator line
    draw_hline(cx + 4, cy + 34, cw - 8, 0xFF334455);
    // Line numbers and text area
    int line_area_x = cx + 44;
    int text_y = cy + 40;
    int tx = line_area_x, ty_t = text_y;
    int line_no = 1;
    // Draw first line number
    {
        char lb[8]; int_to_str(line_no, lb);
        draw_string(cx + 8, ty_t, lb, 0xFF445566);
    }
    // Line number separator
    draw_vline(cx + 40, cy + 36, ch - 40, 0xFF2A2A3A);
    for (int i = 0; i < scribe_len && ty_t < w->y + w->h - 24; i++) {
        if (scribe_buf[i] == '\n') {
            tx = line_area_x; ty_t += 18;
            line_no++;
            char lb[8]; int_to_str(line_no, lb);
            draw_string(cx + 8, ty_t, lb, 0xFF445566);
            continue;
        }
        char s[2] = { scribe_buf[i], 0 };
        draw_string(tx, ty_t, s, 0xFFDDDDCC);
        tx += 8;
        if (tx > w->x + w->w - 16) { tx = line_area_x; ty_t += 18; line_no++; char lb[8]; int_to_str(line_no, lb); draw_string(cx + 8, ty_t, lb, 0xFF445566); }
    }
    // Blinking cursor
    if ((tick_count / 30) % 2 == 0)
        draw_rect(tx, ty_t, 2, 16, 0xFF88AAFF);
    // Status bar at bottom
    draw_rounded_rect_alpha(cx + 4, w->y + w->h - 24, cw - 8, 20, 0xFF181828, 220);
    draw_string(cx + 12, w->y + w->h - 20, "Scribe v1.0", 0xFF556677);
    {
        char lnb[16];
        lnb[0] = 'L'; lnb[1] = 'n'; lnb[2] = ' ';
        int_to_str(line_no, lnb + 3);
        draw_string(cx + cw - 80, w->y + w->h - 20, lnb, 0xFF556677);
    }
}

static void render_paint(window_t* w) {
    int cx = w->x + 8, cy = w->y + TITLE_H + 32;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF222233);
    // Palette
    uint32_t pal[] = {0xFF000000,0xFFFF0000,0xFF00CC00,0xFF0000FF,0xFFFFFF00,0xFFFF00FF,0xFF00FFFF,0xFFFFFFFF,0xFF884400,0xFF888888};
    for (int i = 0; i < 10; i++) {
        int px = w->x+8+i*26, py = w->y+TITLE_H+4;
        draw_rect(px, py, 22, 22, pal[i]);
        if (paint_color == pal[i])
            draw_rounded_rect_border(px-1, py-1, 24, 24, 0xFFFFFFFF, 255);
    }
    // Canvas 200x150 scaled 2x
    for (int y = 0; y < 150; y++)
        for (int x = 0; x < 200; x++) {
            uint32_t c = paint_canvas[y*200+x];
            int dx = cx+x*2, dy = cy+y*2;
            put_pixel(dx, dy, c); put_pixel(dx+1, dy, c);
            put_pixel(dx, dy+1, c); put_pixel(dx+1, dy+1, c);
        }
}

static void render_clock(window_t* w) {
    int ccx = w->x + w->w/2, ccy = w->y + TITLE_H + (w->h - TITLE_H)/2 - 10;
    draw_rect(w->x+1, w->y+TITLE_H+1, w->w-2, w->h-TITLE_H-2, 0xFF1A1A2E);
    int rad = 80;
    // Face
    draw_circle(ccx, ccy, rad, 0xFF222244);
    draw_circle(ccx, ccy, rad-3, 0xFF1A1A33);
    // Hour markers
    draw_rect(ccx-2, ccy-rad+5, 4, 10, 0xFFFFFFFF);
    draw_rect(ccx+rad-15, ccy-2, 10, 4, 0xFFFFFFFF);
    draw_rect(ccx-2, ccy+rad-15, 4, 10, 0xFFFFFFFF);
    draw_rect(ccx-rad+5, ccy-2, 10, 4, 0xFFFFFFFF);
    // Brand text
    draw_string(ccx-20, ccy-rad/2, "ALTEO", 0xFF555577);
    // Digital time
    char tb[12];
    two_digit(sys_hour, tb); tb[2]=':';
    two_digit(sys_min, tb+3); tb[5]=':';
    two_digit(sys_sec, tb+6); tb[8]=0;
    int tl = my_strlen(tb);
    draw_string(ccx-tl*4, ccy+rad+16, tb, 0xFF88CCFF);
    // Center dot
    draw_circle(ccx, ccy, 3, 0xFFFF4444);
}

static void render_browser(window_t* w) {
    int cx = w->x + 1, cy = w->y + TITLE_H + 1;
    int cw = w->w - 2, ch = w->h - TITLE_H - 2;
    draw_rect(cx, cy, cw, ch, 0xFF1C1C2C);

    // ---- Navigation Bar ----
    int nb_y = cy + 2;
    draw_rounded_rect_alpha(cx + 4, nb_y, cw - 8, 36, 0xFF222238, 220);
    draw_rounded_rect_border(cx + 4, nb_y, cw - 8, 36, 0xFF334466, 80);

    // Back button
    {
        int bx = cx + 10, by = nb_y + 6;
        int bh = (mx>=bx && mx<bx+24 && my>=by && my<by+24);
        draw_rounded_rect(bx, by, 24, 24, bh ? 0xFF334466 : 0xFF252540);
        draw_string(bx + 7, by + 4, "<", bh ? 0xFFFFFFFF : 0xFF778899);
    }
    // Forward button
    {
        int bx = cx + 38, by = nb_y + 6;
        int bh = (mx>=bx && mx<bx+24 && my>=by && my<by+24);
        draw_rounded_rect(bx, by, 24, 24, bh ? 0xFF334466 : 0xFF252540);
        draw_string(bx + 7, by + 4, ">", bh ? 0xFFFFFFFF : 0xFF778899);
    }
    // Refresh button
    {
        int bx = cx + 66, by = nb_y + 6;
        int bh = (mx>=bx && mx<bx+24 && my>=by && my<by+24);
        draw_rounded_rect(bx, by, 24, 24, bh ? 0xFF334466 : 0xFF252540);
        draw_string(bx + 5, by + 4, "O", bh ? 0xFFFFFFFF : 0xFF778899);
    }
    // Home button
    {
        int bx = cx + 94, by = nb_y + 6;
        int bh = (mx>=bx && mx<bx+24 && my>=by && my<by+24);
        draw_rounded_rect(bx, by, 24, 24, bh ? 0xFF334466 : 0xFF252540);
        draw_string(bx + 5, by + 4, "H", bh ? 0xFFFFFFFF : 0xFF778899);
    }

    // URL bar
    int url_x = cx + 126;
    int url_w = cw - 144;
    draw_rounded_rect(url_x, nb_y + 6, url_w, 24, 0xFF0E0E1C);
    draw_rounded_rect_border(url_x, nb_y + 6, url_w, 24, browser_url_editing ? 0xFF4477CC : 0xFF2A2A44, 100);
    // Security icon (lock)
    if (browser_url[0] == 'h' && browser_url[4] == 's') {
        draw_rect(url_x + 6, nb_y + 12, 8, 8, 0xFF44AA66);
    }
    draw_string(url_x + 18, nb_y + 10, browser_url, browser_url_editing ? 0xFFFFFFFF : 0xFF88AACC);
    // Cursor when editing
    if (browser_url_editing && (tick_count / 30) % 2 == 0) {
        int cur_x = url_x + 18 + browser_url_len * 8;
        draw_rect(cur_x, nb_y + 10, 2, 14, 0xFF88AAFF);
    }

    // ---- Tab bar ----
    int tab_y = cy + 42;
    draw_rect(cx + 4, tab_y, cw - 8, 26, 0xFF1A1A2C);
    // Active tab
    draw_rounded_rect(cx + 6, tab_y + 2, 180, 22, 0xFF252540);
    draw_rounded_rect_border(cx + 6, tab_y + 2, 180, 22, 0xFF334466, 60);
    draw_icon_browser(cx + 12, tab_y + 5, 14);
    draw_string(cx + 30, tab_y + 6, "Surf - Home", 0xFFCCCCDD);
    // Close tab button
    draw_string(cx + 168, tab_y + 6, "x", 0xFF778899);
    // New tab button
    {
        int ntx = cx + 192, nty = tab_y + 2;
        int nth = (mx>=ntx && mx<ntx+22 && my>=nty && my<nty+22);
        draw_rounded_rect(ntx, nty, 22, 22, nth ? 0xFF334466 : 0xFF222238);
        draw_string(ntx + 5, nty + 3, "+", nth ? 0xFFFFFFFF : 0xFF778899);
    }

    // ---- Content area ----
    int content_y = tab_y + 30;
    int content_h = ch - 76;
    draw_rect(cx + 4, content_y, cw - 8, content_h, 0xFFF0F0F6);

    // Display homepage
    if (my_strncmp(browser_url, "alteo://home", 12) == 0) {
        // Surf Browser Homepage
        int pc_x = cx + cw/2;
        int pc_y = content_y + 40;

        // Surf logo (larger globe)
        draw_circle(pc_x, pc_y + 20, 28, 0xFF2288DD);
        draw_circle(pc_x, pc_y + 20, 26, 0xFF33AAEE);
        draw_circle(pc_x, pc_y + 20, 20, 0xFF44BBFF);
        draw_hline(pc_x - 24, pc_y + 20, 48, 0xFF1166AA);
        draw_hline(pc_x - 20, pc_y + 10, 40, 0xFF1166AA);
        draw_hline(pc_x - 20, pc_y + 30, 40, 0xFF1166AA);
        draw_vline(pc_x, pc_y - 4, 48, 0xFF1166AA);

        // Title
        draw_string(pc_x - 24, pc_y + 56, "Surf", 0xFF2244AA);

        // Search bar
        int sb_x = pc_x - 150, sb_y = pc_y + 86;
        draw_rounded_rect(sb_x, sb_y, 300, 36, 0xFFFFFFFF);
        draw_rounded_rect_border(sb_x, sb_y, 300, 36, 0xFFCCCCDD, 120);
        draw_string(sb_x + 12, sb_y + 10, "Search with Alteo...", 0xFFAAAABB);
        // Search icon
        draw_circle(sb_x + 278, sb_y + 18, 8, 0xFF4488CC);
        draw_string(sb_x + 274, sb_y + 12, "?", 0xFFFFFFFF);

        // Quick links
        int ql_y = pc_y + 140;
        draw_string(pc_x - 60, ql_y, "Quick Links", 0xFF556688);
        ql_y += 24;

        struct { const char* name; const char* url; uint32_t col; } links[] = {
            {"Alteo Docs", "alteo://docs", 0xFF4488CC},
            {"System", "alteo://sysinfo", 0xFF44AA66},
            {"Settings", "alteo://settings", 0xFFCC8844},
            {"Network", "alteo://network", 0xFF8866CC},
        };
        for (int i = 0; i < 4; i++) {
            int lx = pc_x - 140 + i * 72;
            int ly = ql_y;
            int lh = (mx >= lx && mx < lx + 64 && my >= ly && my < ly + 64);
            draw_rounded_rect(lx, ly, 64, 64, lh ? 0xFFE8E8F0 : 0xFFFFFFFF);
            draw_rounded_rect_border(lx, ly, 64, 64, 0xFFDDDDEE, 80);
            draw_circle(lx + 32, ly + 24, 14, links[i].col);
            int nl = my_strlen(links[i].name);
            draw_string(lx + 32 - nl * 4, ly + 46, links[i].name, 0xFF556688);
        }
    } else if (my_strncmp(browser_url, "alteo://docs", 12) == 0) {
        draw_string(cx + 24, content_y + 20, "Alteo OS Documentation", 0xFF2244AA);
        draw_hline(cx + 24, content_y + 40, 200, 0xFFCCCCDD);
        draw_string(cx + 24, content_y + 52, "Welcome to Alteo OS v5.0", 0xFF444466);
        draw_string(cx + 24, content_y + 72, "Alteo is a custom operating system built", 0xFF556677);
        draw_string(cx + 24, content_y + 90, "from scratch featuring a modern desktop", 0xFF556677);
        draw_string(cx + 24, content_y + 108, "environment with window management,", 0xFF556677);
        draw_string(cx + 24, content_y + 126, "networking, and audio support.", 0xFF556677);
        draw_string(cx + 24, content_y + 160, "Features:", 0xFF334466);
        draw_string(cx + 24, content_y + 180, "- Graphical Desktop (1024x768)", 0xFF556677);
        draw_string(cx + 24, content_y + 198, "- Window Manager with Drag & Drop", 0xFF556677);
        draw_string(cx + 24, content_y + 216, "- File System (VFS/FAT32)", 0xFF556677);
        draw_string(cx + 24, content_y + 234, "- Networking Stack (TCP/IP)", 0xFF556677);
        draw_string(cx + 24, content_y + 252, "- Audio (AC97)", 0xFF556677);
        draw_string(cx + 24, content_y + 270, "- Built-in Applications", 0xFF556677);
    } else if (my_strncmp(browser_url, "alteo://network", 15) == 0) {
        draw_string(cx + 24, content_y + 20, "Network Status", 0xFF2244AA);
        draw_hline(cx + 24, content_y + 40, 200, 0xFFCCCCDD);
        draw_string(cx + 24, content_y + 52, "Interface: E1000 (Intel)", 0xFF556677);
        draw_string(cx + 24, content_y + 72, "IP: 10.0.2.15", 0xFF556677);
        draw_string(cx + 24, content_y + 92, "Subnet: 255.255.255.0", 0xFF556677);
        draw_string(cx + 24, content_y + 112, "Gateway: 10.0.2.2", 0xFF556677);
        draw_string(cx + 24, content_y + 132, "DNS: 10.0.2.3", 0xFF556677);
        draw_string(cx + 24, content_y + 162, "Status: Connected", 0xFF44AA66);
    } else {
        // Generic page
        draw_string(cx + 24, content_y + 30, "Page not found", 0xFF884444);
        draw_string(cx + 24, content_y + 56, "The requested page could not", 0xFF556677);
        draw_string(cx + 24, content_y + 74, "be loaded. Check the URL and", 0xFF556677);
        draw_string(cx + 24, content_y + 92, "try again.", 0xFF556677);
    }

    // ---- Status bar ----
    int sb_y = cy + ch - 22;
    draw_rect(cx + 4, sb_y, cw - 8, 20, 0xFF1C1C2C);
    draw_string(cx + 12, sb_y + 3, "Surf Browser v1.0 - Alteo OS", 0xFF556677);
}

static void render_window_content(window_t* w) {
    switch(w->app_type) {
        case APP_TERMINAL: render_terminal(w); break;
        case APP_ABOUT: render_about(w); break;
        case APP_SYSINFO: render_sysinfo(w); break;
        case APP_CALCULATOR: render_calculator(w); break;
        case APP_FILES: render_files(w); break;
        case APP_SETTINGS: render_settings(w); break;
        case APP_SCRIBE: render_scribe(w); break;
        case APP_PAINT: render_paint(w); break;
        case APP_CLOCK: render_clock(w); break;
        case APP_BROWSER: render_browser(w); break;
    }
}

// ---- Taskbar ----
static void draw_taskbar(void) {
    // Glassmorphism base: dark panel with subtle gradient
    for (int y = TASKBAR_Y; y < SCR_H; y++) {
        int gy = y - TASKBAR_Y;
        // Gradient from slightly lighter top to darker bottom
        int r = 12 - (gy * 4) / TASKBAR_H;
        int g = 14 - (gy * 5) / TASKBAR_H;
        int b = 28 - (gy * 6) / TASKBAR_H;
        if (r < 6) r = 6; if (g < 6) g = 6; if (b < 16) b = 16;
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int x = 0; x < SCR_W; x++)
            put_pixel_alpha(x, y, col, 235);
    }
    // Top highlight line (frosted glass edge)
    for (int x = 0; x < SCR_W; x++)
        put_pixel_alpha(x, TASKBAR_Y, 0xFF6688BB, 90);
    // Subtle second line
    for (int x = 0; x < SCR_W; x++)
        put_pixel_alpha(x, TASKBAR_Y + 1, 0xFF445577, 40);

    // Start button with hover glow
    uint32_t sbg = 0xFF1A1A33;
    int start_hover = (mx >= 0 && mx < START_BTN_W && my >= TASKBAR_Y);
    if (start_hover) sbg = 0xFF263866;
    if (start_menu_open) sbg = 0xFF2244AA;
    // Rounded start button area
    draw_rounded_rect_alpha(4, TASKBAR_Y + 4, START_BTN_W - 8, TASKBAR_H - 8, sbg, 220);
    if (start_menu_open || start_hover) {
        // Glow border on hover/open
        draw_rounded_rect_border(4, TASKBAR_Y + 4, START_BTN_W - 8, TASKBAR_H - 8, 0xFF5588DD, 120);
    }

    // Alteo diamond logo in start button (animated glow when open)
    uint32_t logo_c1 = start_menu_open ? 0xFF77AAFF : 0xFF5588DD;
    uint32_t logo_c2 = start_menu_open ? 0xFF5588EE : 0xFF3366BB;
    draw_alteo_logo(14, TASKBAR_Y + 10, 20, logo_c1);
    draw_alteo_logo(16, TASKBAR_Y + 14, 16, logo_c2);
    draw_string(38, TASKBAR_Y + 13, "ALTEO", 0xFFDDDDFF);

    // Vertical separator after start button
    for (int y = TASKBAR_Y + 8; y < SCR_H - 8; y++) {
        put_pixel_alpha(START_BTN_W, y, 0xFF556688, 100);
        put_pixel_alpha(START_BTN_W + 1, y, 0xFF223344, 60);
    }

    // Running app buttons with modern pill style
    int tx = START_BTN_W + 8;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) continue;
        int bw = 155;
        int is_focused_btn = (i == focused_win);
        int btn_hover = (mx >= tx && mx < tx + bw && my >= TASKBAR_Y);

        // Button background with state-dependent styling
        uint32_t bg = 0xFF151522;
        int bg_alpha = 180;
        if (is_focused_btn) { bg = 0xFF1E2E55; bg_alpha = 220; }
        if (windows[i].minimized) { bg = 0xFF111118; bg_alpha = 140; }
        if (btn_hover) { bg = 0xFF253555; bg_alpha = 220; }

        draw_rounded_rect_alpha(tx, TASKBAR_Y + 5, bw, TASKBAR_H - 10, bg, bg_alpha);

        // Focus indicator - glowing bottom line
        if (is_focused_btn) {
            for (int gx = 0; gx < bw; gx++) {
                // Tapered glow: brighter in center, fading at edges
                int dist = gx < bw / 2 ? gx : bw - gx;
                int bright = (dist * 255) / (bw / 2);
                if (bright > 255) bright = 255;
                put_pixel_alpha(tx + gx, TASKBAR_Y + TASKBAR_H - 4, 0xFF5599EE, bright);
                put_pixel_alpha(tx + gx, TASKBAR_Y + TASKBAR_H - 3, 0xFF77BBFF, bright * 3 / 4);
            }
        } else if (windows[i].minimized) {
            // Dim dot indicator for minimized
            for (int dot = bw / 2 - 8; dot < bw / 2 + 8; dot++)
                put_pixel_alpha(tx + dot, TASKBAR_Y + TASKBAR_H - 4, 0xFF445566, 120);
        }

        // Hover highlight border
        if (btn_hover)
            draw_rounded_rect_border(tx, TASKBAR_Y + 5, bw, TASKBAR_H - 10, 0xFF5588BB, 80);

        draw_app_icon(windows[i].app_type, tx + 8, TASKBAR_Y + 12, 18);
        draw_string(tx + 30, TASKBAR_Y + 13, windows[i].title, is_focused_btn ? 0xFFEEEEFF : 0xFF999AAA);
        tx += bw + 4;
    }

    // System tray area (right side) with separator
    int tray_x = SCR_W - 150;
    for (int y = TASKBAR_Y + 8; y < SCR_H - 8; y++) {
        put_pixel_alpha(tray_x, y, 0xFF556688, 80);
    }

    // Tray icons - network dot, volume bars
    int icon_x = tray_x + 10;
    // Network indicator (green dot)
    draw_circle(icon_x + 4, TASKBAR_Y + 20, 3, 0xFF44CC66);
    // Volume bars
    icon_x += 18;
    for (int vb = 0; vb < 4; vb++) {
        int vh = 4 + vb * 3;
        draw_rect(icon_x + vb * 4, TASKBAR_Y + 26 - vh, 2, vh, 0xFF8899BB);
    }

    // Clock (larger, cleaner)
    char tb[12];
    two_digit(sys_hour, tb); tb[2] = ':';
    two_digit(sys_min, tb + 3); tb[5] = 0;
    draw_string(SCR_W - 52, TASKBAR_Y + 7, tb, 0xFFEEEEFF);
    // Date below clock
    draw_string(SCR_W - 76, TASKBAR_Y + 23, "Feb 2026", 0xFF667788);
}

// ---- Start Menu ----
static void draw_start_menu(void) {
    if (!start_menu_open) return;

    // Drop shadow behind menu
    draw_shadow(MENU_X, MENU_Y, MENU_W, MENU_H, 12);

    // Background with gradient
    for (int y = 0; y < MENU_H; y++) {
        int r = 16 + (y * 2) / MENU_H;
        int g = 16 + (y * 2) / MENU_H;
        int b = 36 + (y * 6) / MENU_H;
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int x = 0; x < MENU_W; x++) {
            int draw = 1;
            // Round corners (radius 10)
            int cr = 10;
            if (x < cr && y < cr) { int dx = cr-x-1, dy = cr-y-1; if (dx*dx+dy*dy > cr*cr) draw = 0; }
            if (x >= MENU_W-cr && y < cr) { int dx = x-(MENU_W-cr), dy = cr-y-1; if (dx*dx+dy*dy > cr*cr) draw = 0; }
            if (draw) put_pixel_alpha(MENU_X + x, MENU_Y + y, col, 245);
        }
    }
    // Border glow
    draw_rounded_rect_border(MENU_X, MENU_Y, MENU_W, MENU_H, 0xFF4466BB, 140);

    // Header area with gradient accent bar
    for (int x = 12; x < MENU_W - 12; x++) {
        int grad = (x * 255) / MENU_W;
        int r2 = 40 + (grad * 50) / 255;
        int g2 = 60 + (grad * 20) / 255;
        int b2 = 160 + (grad * 60) / 255;
        put_pixel(MENU_X + x, MENU_Y + 42, 0xFF000000 | (r2 << 16) | (g2 << 8) | b2);
    }

    // Circle background for logo
    draw_circle(MENU_X + 30, MENU_Y + 22, 17, 0xFF1E3888);
    draw_circle(MENU_X + 30, MENU_Y + 22, 15, 0xFF152E77);
    // Glowing ring
    for (int a = 0; a < 360; a++) {
        // Approximate circle ring pixels
        int cxp = MENU_X + 30, cyp = MENU_Y + 22;
        int rr = 16;
        // Use simple angle approximation for glow dots
        int dx = 0, dy = 0;
        if (a < 90) { dx = a * rr / 90; dy = -(rr - a * rr / 90); }
        else if (a < 180) { dx = rr - (a-90) * rr / 90; dy = (a-90) * rr / 90; }
        else if (a < 270) { dx = -((a-180) * rr / 90); dy = rr - (a-180) * rr / 90; }
        else { dx = -(rr - (a-270) * rr / 90); dy = -((a-270) * rr / 90); }
        put_pixel_alpha(cxp + dx, cyp + dy, 0xFF6699FF, 60);
    }
    // Diamond mark inside circle
    draw_alteo_logo(MENU_X + 22, MENU_Y + 12, 16, 0xFF88BBFF);
    draw_alteo_logo(MENU_X + 24, MENU_Y + 15, 12, 0xFFAADDFF);
    draw_string(MENU_X + 26, MENU_Y + 16, "A", 0xFFFFFFFF);

    // Branding text with better typography feel
    draw_string(MENU_X + 54, MENU_Y + 10, "ALTEO OS", 0xFFFFFFFF);
    draw_string(MENU_X + 54, MENU_Y + 26, "v5.0", 0xFF7788AA);

    // App list with improved layout
    int app_list_y = MENU_Y + 48;
    for (int i = 0; i < NUM_APPS; i++) {
        int iy = app_list_y + i * MENU_ITEM_H;
        int item_hover = (mx >= MENU_X + 6 && mx < MENU_X + MENU_W - 6 && my >= iy && my < iy + MENU_ITEM_H);

        // Hover effect with rounded highlight
        if (item_hover) {
            draw_rounded_rect_alpha(MENU_X + 6, iy, MENU_W - 12, MENU_ITEM_H - 2, 0xFF3355AA, 70);
            draw_rounded_rect_border(MENU_X + 6, iy, MENU_W - 12, MENU_ITEM_H - 2, 0xFF5577CC, 50);
        }

        draw_app_icon(i, MENU_X + 18, iy + 7, 22);
        draw_string(MENU_X + 48, iy + 10, app_names[i], item_hover ? 0xFFFFFFFF : 0xFFBBBBCC);
    }

    // Footer with gradient separator
    int fy = app_list_y + NUM_APPS * MENU_ITEM_H + 6;
    for (int x = 12; x < MENU_W - 12; x++) {
        int grad = (x * 255) / MENU_W;
        put_pixel_alpha(MENU_X + x, fy, 0xFF5577AA, 40 + (grad * 40) / 255);
    }

    // User info
    // User avatar circle
    draw_circle(MENU_X + 26, fy + 18, 12, 0xFF2A3A5A);
    draw_circle(MENU_X + 26, fy + 18, 10, 0xFF3A4A6A);
    // Simple person silhouette
    draw_circle(MENU_X + 26, fy + 14, 4, 0xFF99AACC); // head
    draw_circle(MENU_X + 26, fy + 26, 6, 0xFF99AACC); // body (clipped by avatar circle)
    draw_string(MENU_X + 44, fy + 8, "root", 0xFFDDDDEE);
    draw_string(MENU_X + 44, fy + 22, "Alteo OS v5.0", 0xFF556677);

    // Power buttons row
    int py = fy + 44;
    // Second separator
    for (int x = 12; x < MENU_W - 12; x++)
        put_pixel_alpha(MENU_X + x, py - 4, 0xFF445577, 50);

    // Power button layout: Shutdown, Restart, Sleep, Lock
    typedef struct { const char* label; uint32_t color; uint32_t hover_bg; } power_btn_t;
    power_btn_t pbtns[] = {
        {"Shut Down", 0xFFEE4455, 0xFF442222},
        {"Restart",   0xFFFF8833, 0xFF443322},
        {"Sleep",     0xFF44AADD, 0xFF223344},
        {"Lock",      0xFF66BB66, 0xFF224422}
    };
    int pbtn_w = (MENU_W - 24 - 12) / 4; // 4 buttons with 4px gaps
    for (int i = 0; i < 4; i++) {
        int bx = MENU_X + 8 + i * (pbtn_w + 4);
        int bh_btn = 34;
        int btn_hover = (mx >= bx && mx < bx + pbtn_w && my >= py && my < py + bh_btn);

        // Button background
        if (btn_hover) {
            draw_rounded_rect_alpha(bx, py, pbtn_w, bh_btn, pbtns[i].hover_bg, 180);
            draw_rounded_rect_border(bx, py, pbtn_w, bh_btn, pbtns[i].color, 80);
        } else {
            draw_rounded_rect_alpha(bx, py, pbtn_w, bh_btn, 0xFF151525, 160);
        }

        // Icon for each button
        int icx = bx + pbtn_w / 2 - 4, icy = py + 4;
        uint32_t ic = btn_hover ? pbtns[i].color : 0xFF667788;
        if (i == 0) { // Shutdown - power icon
            draw_circle(icx + 4, icy + 6, 5, ic);
            draw_circle(icx + 4, icy + 6, 3, btn_hover ? pbtns[i].hover_bg : 0xFF151525);
            draw_rect(icx + 3, icy, 2, 6, ic);
        } else if (i == 1) { // Restart - circular arrow
            draw_circle(icx + 4, icy + 5, 5, ic);
            draw_circle(icx + 4, icy + 5, 3, btn_hover ? pbtns[i].hover_bg : 0xFF151525);
            draw_rect(icx + 7, icy + 2, 3, 2, ic); // arrow head
            draw_rect(icx + 8, icy + 4, 2, 2, ic);
        } else if (i == 2) { // Sleep - moon
            draw_circle(icx + 4, icy + 5, 5, ic);
            draw_circle(icx + 6, icy + 3, 5, btn_hover ? pbtns[i].hover_bg : 0xFF151525);
        } else { // Lock - padlock
            draw_rect(icx + 1, icy + 5, 6, 6, ic); // lock body
            // Shackle (arch)
            draw_rect(icx + 2, icy + 1, 1, 5, ic);
            draw_rect(icx + 5, icy + 1, 1, 5, ic);
            draw_hline(icx + 2, icy + 1, 4, ic);
            // Keyhole
            put_pixel(icx + 4, icy + 8, btn_hover ? pbtns[i].hover_bg : 0xFF151525);
        }

        // Label below icon
        int lbl_len = my_strlen(pbtns[i].label);
        int lx = bx + (pbtn_w - lbl_len * 8) / 2;
        draw_string(lx, py + 20, pbtns[i].label, btn_hover ? 0xFFFFFFFF : 0xFF778899);
    }
}

// ---- Context menu ----
static void draw_context_menu(void) {
    if (!ctx_open) return;
    int cw = 210, ch = 140;
    draw_shadow(ctx_x, ctx_y, cw, ch, 8);
    // Background gradient
    for (int y = 0; y < ch; y++) {
        int r = 20 + y / 10, g = 20 + y / 10, b = 38 + y / 6;
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        int cr = 8;
        for (int x = 0; x < cw; x++) {
            int draw = 1;
            if (x < cr && y < cr) { int dx2 = cr-x-1, dy2 = cr-y-1; if (dx2*dx2+dy2*dy2 > cr*cr) draw = 0; }
            if (x >= cw-cr && y < cr) { int dx2 = x-(cw-cr), dy2 = cr-y-1; if (dx2*dx2+dy2*dy2 > cr*cr) draw = 0; }
            if (x < cr && y >= ch-cr) { int dx2 = cr-x-1, dy2 = y-(ch-cr); if (dx2*dx2+dy2*dy2 > cr*cr) draw = 0; }
            if (x >= cw-cr && y >= ch-cr) { int dx2 = x-(cw-cr), dy2 = y-(ch-cr); if (dx2*dx2+dy2*dy2 > cr*cr) draw = 0; }
            if (draw) put_pixel_alpha(ctx_x + x, ctx_y + y, col, 242);
        }
    }
    draw_rounded_rect_border(ctx_x, ctx_y, cw, ch, 0xFF4466AA, 120);

    const char* items[] = {"New Window", "Terminal", "Settings", "About Alteo"};
    for (int i = 0; i < 4; i++) {
        int iy = ctx_y + 10 + i * 30;
        int item_hover = (mx >= ctx_x + 6 && mx < ctx_x + cw - 6 && my >= iy && my < iy + 28);
        if (item_hover) {
            draw_rounded_rect_alpha(ctx_x + 6, iy, cw - 12, 28, 0xFF3355AA, 70);
        }
        // Mini icon hint
        int icon_types[] = {APP_FILES, APP_TERMINAL, APP_SETTINGS, APP_ABOUT};
        draw_app_icon(icon_types[i], ctx_x + 14, iy + 4, 16);
        draw_string(ctx_x + 38, iy + 6, items[i], item_hover ? 0xFFFFFFFF : 0xFFBBBBCC);
    }
}

// ---- Desktop icons ----
static void draw_desktop_icons(void) {
    int dx = 24, dy = 20;
    int icons[] = {APP_TERMINAL, APP_FILES, APP_SCRIBE, APP_BROWSER, APP_PAINT};
    const char* names[] = {"Terminal", "Files", "Scribe", "Surf", "Paint"};
    for (int i = 0; i < 5; i++) {
        int ix = dx, iy = dy + i * 90;
        int icon_hover = (mx >= ix - 6 && mx < ix + 60 && my >= iy - 6 && my < iy + 74);
        // Hover highlight with glow
        if (icon_hover) {
            draw_rounded_rect_alpha(ix - 8, iy - 8, 70, 84, 0xFF3355AA, 35);
            draw_rounded_rect_border(ix - 8, iy - 8, 70, 84, 0xFF5577CC, 40);
        }
        // Icon (32x32 on desktop)
        draw_app_icon(icons[i], ix + 8, iy, 32);
        // Label with shadow for readability
        int nl = my_strlen(names[i]);
        draw_string(ix + 25 - nl * 4, iy + 40, names[i], 0xFF111122);  // shadow
        draw_string(ix + 24 - nl * 4, iy + 39, names[i], icon_hover ? 0xFFFFFFFF : 0xFFDDDDEE);
    }
}

// ---- Desktop ALTEO watermark + widgets ----
static void draw_desktop_branding(void) {
    // Centered ALTEO logo on desktop (subtle watermark)
    int cx = SCR_W / 2, cy = (TASKBAR_Y) / 2 - 20;

    // Large diamond - outer glow (very subtle)
    draw_alteo_logo(cx - 36, cy - 48, 72, 0xFF141428);
    // Main diamond layers
    draw_alteo_logo(cx - 30, cy - 40, 60, 0xFF1A1A38);
    draw_alteo_logo(cx - 22, cy - 30, 44, 0xFF202048);
    draw_alteo_logo(cx - 14, cy - 20, 28, 0xFF282860);

    // "A" letter inside the diamond
    draw_string(cx - 4, cy - 12, "A", 0xFF303070);

    // ALTEO text below logo
    draw_string(cx - 20, cy + 28, "ALTEO", 0xFF1E1E44);
    // Tagline
    draw_string(cx - 56, cy + 48, "Operating System", 0xFF161630);

    // ===== WIDGETS (right side of desktop) =====
    int wx = SCR_W - 220, wy = 20;

    // ---- Clock Widget ----
    draw_shadow(wx, wy, 200, 90, 6);
    draw_rounded_rect_alpha(wx, wy, 200, 90, 0xFF121222, 200);
    draw_rounded_rect_border(wx, wy, 200, 90, 0xFF334466, 60);

    // Large digital time
    char big_time[12];
    two_digit(sys_hour, big_time); big_time[2] = ':';
    two_digit(sys_min, big_time + 3); big_time[5] = ':';
    two_digit(sys_sec, big_time + 6); big_time[8] = 0;
    // Draw time larger by repeating offset
    for (int dx = 0; dx < 2; dx++)
        for (int dy = 0; dy < 2; dy++)
            draw_string(wx + 30 + dx, wy + 14 + dy, big_time, 0xFF88BBFF);
    // Blink colon
    if ((tick_count / 50) % 2 == 0) {
        draw_rect(wx + 30 + 16, wy + 14, 8, 16, 0xFF121222); // hide first colon briefly
    }
    // Date
    draw_string(wx + 36, wy + 42, "Wednesday", 0xFF667799);
    draw_string(wx + 36, wy + 60, "Feb 5, 2026", 0xFF556688);
    // Clock icon
    draw_icon_clock_small(wx + 8, wy + 10, 16);

    wy += 100;

    // ---- System Stats Widget ----
    draw_shadow(wx, wy, 200, 110, 6);
    draw_rounded_rect_alpha(wx, wy, 200, 110, 0xFF121222, 200);
    draw_rounded_rect_border(wx, wy, 200, 110, 0xFF334466, 60);
    draw_string(wx + 12, wy + 8, "System Monitor", 0xFFAABBDD);
    draw_hline(wx + 12, wy + 26, 176, 0xFF223344);

    // CPU usage bar
    draw_string(wx + 12, wy + 32, "CPU", 0xFF889AAA);
    draw_rounded_rect(wx + 50, wy + 34, 136, 10, 0xFF1A1A2A);
    // Pseudo-random CPU usage simulation
    static int cpu_pct_smooth = 18;
    if ((tick_count % 25) == 0) {
        uint32_t rng = tick_count * 1103515245u + 12345u;
        rng = (rng >> 16) & 0x7FFF;
        int delta = (int)(rng % 11) - 5; // -5 to +5
        cpu_pct_smooth += delta;
        // Bias toward 15-35% idle range
        if (cpu_pct_smooth < 5) cpu_pct_smooth = 5 + (int)(rng % 8);
        if (cpu_pct_smooth > 65) cpu_pct_smooth = 60 - (int)(rng % 10);
    }
    int cpu_pct = cpu_pct_smooth;
    int cpu_bar = (cpu_pct * 130) / 100;
    uint32_t cpu_col = cpu_pct > 80 ? 0xFFEE4455 : (cpu_pct > 50 ? 0xFFDDAA33 : 0xFF44BB66);
    draw_rect(wx + 53, wy + 36, cpu_bar, 6, cpu_col);
    char pct_buf[8]; int_to_str(cpu_pct, pct_buf);
    int pl = my_strlen(pct_buf); pct_buf[pl] = '%'; pct_buf[pl+1] = 0;
    draw_string(wx + 160, wy + 32, pct_buf, cpu_col);

    // RAM usage bar
    draw_string(wx + 12, wy + 50, "RAM", 0xFF889AAA);
    draw_rounded_rect(wx + 50, wy + 52, 136, 10, 0xFF1A1A2A);
    int ram_pct = 12; // ~12% of 512MB used
    int ram_bar = (ram_pct * 130) / 100;
    draw_rect(wx + 53, wy + 54, ram_bar, 6, 0xFF4488CC);
    draw_string(wx + 148, wy + 50, "24M", 0xFF4488CC);

    // Processes
    draw_string(wx + 12, wy + 70, "Tasks", 0xFF889AAA);
    {
        char tb[8]; int_to_str(process_count(), tb);
        draw_string(wx + 60, wy + 70, tb, 0xFF88AAFF);
        draw_string(wx + 60 + my_strlen(tb) * 8, wy + 70, " running", 0xFF667788);
    }
    // Uptime
    draw_string(wx + 12, wy + 88, "Up", 0xFF889AAA);
    {
        char ub[24]; int secs = (int)(tick_count / 100);
        int mins = secs / 60; secs %= 60;
        int hrs = mins / 60; mins %= 60;
        int pos = 0;
        if (hrs > 0) { int_to_str(hrs, ub + pos); pos += my_strlen(ub + pos); ub[pos++] = 'h'; ub[pos++] = ' '; }
        int_to_str(mins, ub + pos); pos += my_strlen(ub + pos); ub[pos++] = 'm'; ub[pos++] = ' ';
        int_to_str(secs, ub + pos); pos += my_strlen(ub + pos); ub[pos++] = 's'; ub[pos] = 0;
        draw_string(wx + 40, wy + 88, ub, 0xFF88AAFF);
    }

    wy += 120;

    // ---- Quick Launch Widget ----
    draw_shadow(wx, wy, 200, 68, 6);
    draw_rounded_rect_alpha(wx, wy, 200, 68, 0xFF121222, 200);
    draw_rounded_rect_border(wx, wy, 200, 68, 0xFF334466, 60);
    draw_string(wx + 12, wy + 6, "Quick Launch", 0xFFAABBDD);
    draw_hline(wx + 12, wy + 24, 176, 0xFF223344);

    // Row of 5 quick launch icons
    int ql_icons[] = {APP_TERMINAL, APP_FILES, APP_SCRIBE, APP_CALCULATOR, APP_SETTINGS};
    for (int i = 0; i < 5; i++) {
        int qx = wx + 12 + i * 38;
        int qy = wy + 30;
        int ql_hover = (mx >= qx - 2 && mx < qx + 32 && my >= qy - 2 && my < qy + 32);
        if (ql_hover) {
            draw_rounded_rect_alpha(qx - 4, qy - 4, 36, 36, 0xFF2244AA, 80);
        }
        draw_app_icon(ql_icons[i], qx, qy, 28);
    }

    // Bottom right version watermark (moved down since widgets took the right side)
    int wxv = SCR_W - 160, wyv = TASKBAR_Y - 42;
    draw_string(wxv, wyv, "ALTEO OS", 0xFF1A1A30);
    draw_string(wxv + 16, wyv + 18, "v5.0", 0xFF151525);
}

// ---- Boot splash screen ----
static void draw_boot_splash(void) {
    // Dark background
    for (int i = 0; i < SCR_W * SCR_H; i++)
        get_backbuf()[i] = 0xFF0A0A14;

    int cx = SCR_W / 2, cy = SCR_H / 2 - 40;

    // Large ALTEO diamond logo
    draw_alteo_logo(cx - 40, cy - 50, 80, 0xFF5588DD);
    draw_alteo_logo(cx - 30, cy - 30, 60, 0xFF3366BB);
    draw_alteo_logo(cx - 18, cy - 10, 36, 0xFF2255AA);

    // ALTEO text
    draw_string(cx - 20, cy + 50, "ALTEO", 0xFFDDDDFF);

    // Subtitle
    draw_string(cx - 72, cy + 80, "Operating System v5.0", 0xFF666688);

    // Loading bar
    int bw = 200, bx = cx - bw/2, by = cy + 110;
    draw_rounded_rect(bx, by, bw, 8, 0xFF222244);

    flip_buffer();

    // Animate loading bar
    for (int p = 0; p < bw - 4; p += 3) {
        draw_rounded_rect(bx+2, by+2, p, 4, 0xFF5588DD);
        flip_buffer();
        for (volatile int d = 0; d < 100000; d++);
    }

    // Brief pause at full
    for (volatile int d = 0; d < 5000000; d++);
}

// ---- Full desktop render ----
static void render_desktop(void) {
    draw_gradient_bg();
    draw_desktop_branding();
    draw_desktop_icons();

    // Windows (unfocused first, focused on top)
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active || windows[i].minimized || i == focused_win) continue;
        draw_window_frame(&windows[i]);
        render_window_content(&windows[i]);
    }
    if (focused_win >= 0 && windows[focused_win].active && !windows[focused_win].minimized) {
        draw_window_frame(&windows[focused_win]);
        render_window_content(&windows[focused_win]);
    }

    draw_taskbar();
    draw_start_menu();
    draw_context_menu();

    // Cursor
    save_mouse_bg(mx, my);
    render_cursor_graphic(mx, my);

    flip_buffer();
}

// ---- Calculator logic ----
static void calc_press(const char* btn) {
    if (my_strcmp(btn, "C") == 0) { calc_val1=0; calc_val2=0; calc_op=0; calc_new=1; my_strcpy(calc_display,"0"); return; }
    if (my_strcmp(btn, "<") == 0) {
        int l=my_strlen(calc_display);
        if(l>1) calc_display[l-1]=0; else my_strcpy(calc_display,"0");
        return;
    }
    if (btn[0]>='0' && btn[0]<='9') {
        if (calc_new) { calc_display[0]=btn[0]; calc_display[1]=0; calc_new=0; }
        else { int l=my_strlen(calc_display); if(l<14) { calc_display[l]=btn[0]; calc_display[l+1]=0; } }
        return;
    }
    if (my_strcmp(btn, ".") == 0) {
        for (int i=0; calc_display[i]; i++) if (calc_display[i]=='.') return;
        int l=my_strlen(calc_display); if(l<14) { calc_display[l]='.'; calc_display[l+1]=0; }
        return;
    }
    if (my_strcmp(btn, "=") == 0) {
        calc_val2=0; int neg=0, i=0;
        if (calc_display[0]=='-') { neg=1; i=1; }
        for (; calc_display[i]; i++) if (calc_display[i]>='0' && calc_display[i]<='9') calc_val2=calc_val2*10+(calc_display[i]-'0');
        if (neg) calc_val2=-calc_val2;
        int result = 0;
        if (calc_op=='+') result=calc_val1+calc_val2;
        else if (calc_op=='-') result=calc_val1-calc_val2;
        else if (calc_op=='*') result=calc_val1*calc_val2;
        else if (calc_op=='/') result=(calc_val2!=0)?calc_val1/calc_val2:0;
        else result=calc_val2;
        int_to_str(result, calc_display); calc_val1=result; calc_op=0; calc_new=1;
        return;
    }
    if (btn[0]=='+'||btn[0]=='-'||btn[0]=='*'||btn[0]=='/') {
        calc_val1=0; int neg=0, i=0;
        if (calc_display[0]=='-') { neg=1; i=1; }
        for (; calc_display[i]; i++) if (calc_display[i]>='0' && calc_display[i]<='9') calc_val1=calc_val1*10+(calc_display[i]-'0');
        if (neg) calc_val1=-calc_val1;
        calc_op=btn[0]; calc_new=1;
    }
}

// ---- Mouse click handling ----
static void handle_click(int x, int y) {
    // Start menu
    if (start_menu_open) {
        // App list clicks
        if (x>=MENU_X+4 && x<MENU_X+MENU_W-4 && y>=MENU_Y+48 && y<MENU_Y+48+NUM_APPS*MENU_ITEM_H) {
            int idx = (y-MENU_Y-48)/MENU_ITEM_H;
            if (idx>=0 && idx<NUM_APPS) {
                int ws[]={600,400,440,300,420,400,460,440,300};
                int hs[]={420,380,360,360,380,360,380,390,320};
                create_window(idx, app_names[idx], ws[idx], hs[idx]);
                start_menu_open=0;
            }
            return;
        }
        // Power buttons at bottom of menu
        {
            int fy = MENU_Y + 48 + NUM_APPS * MENU_ITEM_H + 6;
            int py = fy + 44;
            int pbtn_w = (MENU_W - 24 - 12) / 4;
            for (int i = 0; i < 4; i++) {
                int bx2 = MENU_X + 8 + i * (pbtn_w + 4);
                if (x >= bx2 && x < bx2 + pbtn_w && y >= py && y < py + 34) {
                    start_menu_open = 0;
                    if (i == 0) { // Shutdown
                        // Draw shutdown screen
                        for (int si = 0; si < SCR_W * SCR_H; si++) get_backbuf()[si] = 0xFF000000;
                        draw_string(SCR_W/2 - 60, SCR_H/2 - 8, "Shutting down...", 0xFFFFFFFF);
                        flip_buffer();
                        for (volatile int d = 0; d < 50000000; d++);
                        // Halt CPU
                        __asm__ __volatile__("cli; hlt");
                    } else if (i == 1) { // Restart
                        for (int si = 0; si < SCR_W * SCR_H; si++) get_backbuf()[si] = 0xFF000000;
                        draw_string(SCR_W/2 - 56, SCR_H/2 - 8, "Restarting...", 0xFFFFFFFF);
                        flip_buffer();
                        for (volatile int d = 0; d < 30000000; d++);
                        // Triple fault to reboot
                        __asm__ __volatile__("lidt (%%rax)" : : "a"(0));
                        __asm__ __volatile__("int $3");
                    } else if (i == 2) { // Sleep - blank screen
                        for (int si = 0; si < SCR_W * SCR_H; si++) get_backbuf()[si] = 0xFF000000;
                        draw_string(SCR_W/2 - 100, SCR_H/2, "Press any key to wake...", 0xFF334455);
                        flip_buffer();
                        // Wait for keypress
                        while (!(inb(0x64) & 0x01));
                        inb(0x60); // consume the key
                    } else { // Lock - show lock screen
                        for (int si = 0; si < SCR_W * SCR_H; si++) get_backbuf()[si] = 0xFF0A0A18;
                        draw_alteo_logo(SCR_W/2 - 20, SCR_H/2 - 60, 40, 0xFF3366BB);
                        draw_string(SCR_W/2 - 20, SCR_H/2 - 8, "Locked", 0xFFDDDDFF);
                        draw_string(SCR_W/2 - 100, SCR_H/2 + 16, "Press any key to unlock", 0xFF556688);
                        flip_buffer();
                        while (!(inb(0x64) & 0x01));
                        inb(0x60);
                    }
                    return;
                }
            }
        }
        if (x<MENU_X||x>MENU_X+MENU_W||y<MENU_Y||y>TASKBAR_Y+TASKBAR_H) { start_menu_open=0; return; }
    }

    // Start button
    if (x>=0 && x<START_BTN_W && y>=TASKBAR_Y) {
        start_menu_open = start_menu_open ? 0 : 1;
        ctx_open=0; return;
    }

    // Taskbar app buttons
    if (y >= TASKBAR_Y) {
        int tx = START_BTN_W + 8;
        for (int i=0; i<MAX_WINDOWS; i++) {
            if (!windows[i].active) continue;
            int bw = 150;
            if (x>=tx && x<tx+bw) {
                if (i==focused_win && !windows[i].minimized) {
                    windows[i].minimized=1; focused_win=-1;
                    for (int j=MAX_WINDOWS-1;j>=0;j--)
                        if (windows[j].active && !windows[j].minimized) { focused_win=j; break; }
                } else { windows[i].minimized=0; focused_win=i; }
                start_menu_open=0; return;
            }
            tx += bw + 4;
        }
        return;
    }

    // Context menu
    if (ctx_open) {
        int cw = 200;
        if (x>=ctx_x && x<ctx_x+cw) {
            int ci = (y-ctx_y-10)/28;
            if (ci==0) create_window(APP_FILES,"Files",620,460);
            else if (ci==1) create_window(APP_TERMINAL,"Terminal",600,420);
            else if (ci==2) create_window(APP_SETTINGS,"Settings",400,360);
            else if (ci==3) create_window(APP_ABOUT,"About Alteo",400,380);
        }
        ctx_open=0; return;
    }

    // Desktop icons
    if (y < TASKBAR_Y) {
        // Quick Launch widget clicks (right side)
        {
            int qlx = SCR_W - 220, qly = 20 + 100 + 120; // same as widget position
            int ql_icons[] = {APP_TERMINAL, APP_FILES, APP_SCRIBE, APP_CALCULATOR, APP_SETTINGS};
            const char* ql_names[] = {"Terminal", "Files", "Notes", "Calculator", "Settings"};
            int ql_ws[] = {600, 420, 460, 300, 400};
            int ql_hs[] = {420, 380, 380, 360, 360};
            for (int i = 0; i < 5; i++) {
                int qx = qlx + 12 + i * 38;
                int qy = qly + 30;
                if (x >= qx - 2 && x < qx + 32 && y >= qy - 2 && y < qy + 32) {
                    create_window(ql_icons[i], ql_names[i], ql_ws[i], ql_hs[i]);
                    start_menu_open = 0; return;
                }
            }
        }

        int dx2=24, dy2=20;
        int icons[]={APP_TERMINAL,APP_FILES,APP_SCRIBE,APP_BROWSER,APP_PAINT};
        const char* dn[]={"Terminal","Files","Scribe","Surf","Paint"};
        int dw[]={600,620,520,700,440}, dh[]={420,460,420,500,390};
        for (int i=0;i<5;i++) {
            int ix=dx2, iy=dy2+i*90;
            if (x>=ix-6 && x<ix+60 && y>=iy-6 && y<iy+74) {
                create_window(icons[i],dn[i],dw[i],dh[i]); start_menu_open=0; return;
            }
        }
    }

    start_menu_open = 0;

    // Window interactions
    for (int pass=0; pass<2; pass++) {
        for (int i=MAX_WINDOWS-1; i>=0; i--) {
            if (!windows[i].active || windows[i].minimized) continue;
            if (pass==0 && i!=focused_win) continue;
            if (pass==1 && i==focused_win) continue;
            window_t* w = &windows[i];
            if (x<w->x||x>=w->x+w->w||y<w->y||y>=w->y+w->h) continue;

            focused_win = i;
            // Close button (must match draw_window_frame offsets: bx = x+ww-28, step 20, circle center bx+6,by+6 r=7)
            int bx = w->x+w->w-28, by = w->y+10;
            if (x>=bx && x<bx+14 && y>=by && y<by+14) { close_window(i); return; }
            // Maximize
            bx -= 20;
            if (x>=bx && x<bx+14 && y>=by && y<by+14) { toggle_maximize(i); return; }
            // Minimize
            bx -= 20;
            if (x>=bx && x<bx+14 && y>=by && y<by+14) {
                w->minimized=1; focused_win=-1;
                for (int j=MAX_WINDOWS-1;j>=0;j--) if (windows[j].active && !windows[j].minimized) { focused_win=j; break; }
                return;
            }
            // Title bar drag
            if (y>=w->y && y<w->y+TITLE_H && !(w->maximized)) {
                dragging=1; drag_win=i; drag_ox=x-w->x; drag_oy=y-w->y; return;
            }
            // Calculator buttons
            if (w->app_type == APP_CALCULATOR) {
                int ccx2=w->x+12, ccy2=w->y+TITLE_H+66, bw2=(w->w-24-12)/4;
                const char* btns[]={"C","(",")","/","7","8","9","*","4","5","6","-","1","2","3","+","0",".","<","="};
                for (int r=0;r<5;r++) for (int c=0;c<4;c++) {
                    int bxx=ccx2+c*(bw2+4), byy=ccy2+r*42;
                    if (x>=bxx && x<bxx+bw2 && y>=byy && y<byy+38) { calc_press(btns[r*4+c]); return; }
                }
            }
            // Paint palette
            if (w->app_type == APP_PAINT) {
                uint32_t pal[]={0xFF000000,0xFFFF0000,0xFF00CC00,0xFF0000FF,0xFFFFFF00,0xFFFF00FF,0xFF00FFFF,0xFFFFFFFF,0xFF884400,0xFF888888};
                for (int pi=0;pi<10;pi++) {
                    int px=w->x+8+pi*26, py=w->y+TITLE_H+4;
                    if (x>=px && x<px+22 && y>=py && y<py+22) { paint_color=pal[pi]; return; }
                }
            }
            // Files app - directory navigation
            if (w->app_type == APP_FILES) {
                int fcx = w->x + 1, fcy = w->y + TITLE_H + 1;
                int fcw = w->w - 2;
                int tb_y2 = fcy + 2;
                int sidebar_w2 = 130;
                int content_y2 = fcy + 40;
                int content_h2 = w->h - TITLE_H - 2 - 68;
                int mc_x2 = fcx + sidebar_w2 + 10;
                int mc_w2 = fcw - sidebar_w2 - 18;

                // Back button click
                {
                    int bx = fcx + 10, by = tb_y2 + 4;
                    if (x >= bx && x < bx+28 && y >= by && y < by+24 && my_strcmp(files_cwd, "/") != 0) {
                        int len = my_strlen(files_cwd);
                        while (len > 1 && files_cwd[len - 1] == '/') len--;
                        while (len > 1 && files_cwd[len - 1] != '/') len--;
                        if (len <= 1) { files_cwd[0] = '/'; files_cwd[1] = 0; }
                        else { files_cwd[len - 1] = 0; }
                        files_selected = -1; files_scroll = 0;
                        return;
                    }
                }
                // Home button click
                {
                    int bx = fcx + 44, by = tb_y2 + 4;
                    if (x >= bx && x < bx+28 && y >= by && y < by+24) {
                        my_strcpy(files_cwd, "/home/user");
                        files_selected = -1; files_scroll = 0;
                        return;
                    }
                }
                // View toggle click
                {
                    int vx = fcx + fcw - 100, vy = tb_y2 + 4;
                    if (x >= vx && x < vx+28 && y >= vy && y < vy+24) {
                        files_view_mode = !files_view_mode;
                        return;
                    }
                }

                // Sidebar favorites click
                {
                    int sy = content_y2 + 8 + 22; // after "Favorites" label
                    const char* fav_paths[] = {"/home/user", "/home/user/Documents", "/home/user/Downloads", "/home/user/Pictures", "/home/user/Music"};
                    for (int fi = 0; fi < 5; fi++) {
                        if (x >= fcx+10 && x < fcx+sidebar_w2 && y >= sy && y < sy+22) {
                            my_strcpy(files_cwd, fav_paths[fi]);
                            files_selected = -1; files_scroll = 0;
                            return;
                        }
                        sy += 26;
                    }
                    // System section
                    sy += 32; // spacing + header
                    const char* sys_paths[] = {"/", "/dev", "/tmp", "/boot"};
                    for (int fi = 0; fi < 4; fi++) {
                        if (x >= fcx+10 && x < fcx+sidebar_w2 && y >= sy && y < sy+22) {
                            my_strcpy(files_cwd, sys_paths[fi]);
                            files_selected = -1; files_scroll = 0;
                            return;
                        }
                        sy += 26;
                    }
                }

                // Main content area clicks
                {
                    int item_h2 = 28;
                    int list_y2 = content_y2 + 26;
                    vfs_dirent_t fentries[32];
                    int fcount = vfs_readdir(files_cwd, fentries, 32);
                    if (fcount < 0) fcount = 0;
                    int max_vis2 = content_h2 / item_h2;

                    for (int fi = files_scroll; fi < fcount && (fi - files_scroll) < max_vis2; fi++) {
                        int ffy = list_y2 + (fi - files_scroll) * item_h2;
                        if (x >= mc_x2 && x < mc_x2 + mc_w2 && y >= ffy && y < ffy + item_h2) {
                            if (fentries[fi].type == VFS_DIRECTORY) {
                                // Navigate into directory
                                int clen = my_strlen(files_cwd);
                                if (clen == 1 && files_cwd[0] == '/') {
                                    files_cwd[1] = 0;
                                    my_strcpy(files_cwd + 1, fentries[fi].name);
                                } else {
                                    files_cwd[clen] = '/';
                                    my_strcpy(files_cwd + clen + 1, fentries[fi].name);
                                }
                                files_selected = -1; files_scroll = 0;
                            } else {
                                files_selected = fi;
                            }
                            return;
                        }
                    }
                }
            }
            return;
        }
    }
}

static void paint_check_draw(void) {
    if (!mouse_left) return;
    for (int i=0; i<MAX_WINDOWS; i++) {
        if (!windows[i].active || windows[i].app_type != APP_PAINT) continue;
        window_t* w = &windows[i];
        int cx=w->x+8, cy=w->y+TITLE_H+32;
        int px=(mx-cx)/2, py=(my-cy)/2;
        if (px>=0 && px<200 && py>=0 && py<150) {
            for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
                int nx=px+dx, ny=py+dy;
                if (nx>=0 && nx<200 && ny>=0 && ny<150) paint_canvas[ny*200+nx]=paint_color;
            }
        }
    }
}

// ---- Key handling ----
void process_key(char c) {
    if (focused_win<0 || !windows[focused_win].active) return;
    window_t* w = &windows[focused_win];
    if (w->app_type == APP_TERMINAL) {
        if (c=='\n') term_exec();
        else if (c=='\b') { if (term_input_len>0) { term_input_len--; term_putchar('\b'); } }
        else if (c=='\t') { }
        else { if (term_input_len<120) { term_input[term_input_len++]=c; term_putchar(c); } }
    } else if (w->app_type == APP_SCRIBE) {
        if (c=='\b') { if (scribe_len>0) scribe_len--; }
        else if (c=='\n') { if (scribe_len<4090) scribe_buf[scribe_len++]='\n'; }
        else { if (scribe_len<4090) scribe_buf[scribe_len++]=c; }
    } else if (w->app_type == APP_CALCULATOR) {
        if (c>='0' && c<='9') { char s[2]={c,0}; calc_press(s); }
        else if (c=='+') calc_press("+");
        else if (c=='-') calc_press("-");
        else if (c=='*') calc_press("*");
        else if (c=='/') calc_press("/");
        else if (c=='\n') calc_press("=");
        else if (c=='\b') calc_press("<");
    } else if (w->app_type == APP_BROWSER) {
        if (browser_url_editing) {
            if (c=='\n') { browser_url_editing = 0; }
            else if (c=='\b') { if (browser_url_len>0) { browser_url_len--; browser_url[browser_url_len]=0; } }
            else { if (browser_url_len<250) { browser_url[browser_url_len++]=c; browser_url[browser_url_len]=0; } }
        } else {
            // Press any key to start editing URL bar
            if (c=='\n' || c==' ') { browser_url_editing = 1; }
        }
    }
}

void update_status_line(void) { }

// ---- IRQ Handlers (CRITICAL for input to work) ----
// MUST check port 0x64 status bit 5 to know if data is from keyboard or mouse.
// Without this check, mouse bytes leak into keyboard handler (wrong keys)
// and keyboard bytes corrupt mouse state (jumpy/misaligned cursor).
static void keyboard_irq_handler(registers_t* regs) {
    (void)regs;
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;      // No data ready
    if (status & 0x20) {               // Bit 5 set = data from mouse, NOT keyboard
        inb(0x60);                     // Read & discard to clear buffer
        return;
    }
    uint8_t scancode = inb(0x60);
    keyboard_handle_byte(scancode);
}

static void mouse_irq_handler(registers_t* regs) {
    (void)regs;
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;      // No data ready
    if (!(status & 0x20)) {            // Bit 5 clear = data from keyboard, NOT mouse
        inb(0x60);                     // Read & discard to clear buffer
        return;
    }
    uint8_t data = inb(0x60);
    mouse_handle_byte(data);
}

// ---- Main ----
void kernel_main(uint64_t multiboot_addr) {
    init_graphics(multiboot_addr);

    // Phase 1: Set up GDT with kernel/user segments and TSS
    gdt_init();

    idt_init();

    // Phase 1: Install CPU exception handlers (ISR 0-31) including page fault
    isr_init();

    irq_init();
    // NOTE: Do NOT sti here! Install handlers first.

    // Enable SSE/SSE2 instructions (needed by GPU driver code compiled with SSE)
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1UL << 2);  // Clear CR0.EM (disable x87 emulation)
        cr0 |= (1UL << 1);   // Set CR0.MP (monitor coprocessor)
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1UL << 9);   // Set CR4.OSFXSR (enable FXSAVE/FXRSTOR)
        cr4 |= (1UL << 10);  // Set CR4.OSXMMEXCPT (enable SSE exceptions)
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    // Show boot splash with ALTEO branding (no interrupts needed)
    draw_boot_splash();

    // Phase 1: Initialize Physical Memory Manager (needed before VMM)
    pmm_init(multiboot_addr);

    // Initialize memory subsystems (heap needed before process management)
    heap_init();

    // Phase 1: Initialize Virtual Memory Manager
    // Creates proper 4-level page tables and replaces boot.asm's 1GB huge pages
    vmm_init();

    // Initialize Virtual File System (in-memory)
    vfs_init();

    // Phase 3: Initialize IPC and signal subsystems
    pipe_init();
    signal_init();
    shm_init();

    // Phase 3: Mount pseudo-filesystems (/dev, /proc)
    devfs_init();
    procfs_init();

    // Phase 2: Initialize PCI bus enumerator (must come before all PCI device drivers)
    pci_init();

    // Phase 2: Parse ACPI tables (MADT, FADT - needed for APIC and power management)
    acpi_init();

    // Phase 2: Initialize APIC (replaces legacy 8259 PIC if available)
    // Falls back to PIC if APIC not present
    apic_init();

    // Initialize ATA driver and block device layer
    ata_init();

    // Phase 2: Initialize block device layer (wraps ATA with cache)
    blkdev_init();

    // Attempt FAT32 mount
    if (ata_get_drive_count() > 0) {
        if (fat32_init(0) == 0) {
            // FAT32 filesystem detected - mount it at /mnt/disk
            // TODO: Implement FAT32 VFS operations adapter and call vfs_mount()
        }
    }

    // Phase 3: Attempt ext2 filesystem mount on first ATA drive
    ext2_init(0);

    // Initialize networking stack (e1000 now uses central PCI layer)
    e1000_init();      // NIC driver (uses pci_find_device)
    eth_init();        // Ethernet layer
    arp_init();        // ARP cache
    ip_init();         // IPv4 layer (QEMU default: 10.0.2.15)
    tcp_init();        // TCP transport
    socket_init();     // Socket API

    // Initialize audio (ac97 now uses central PCI layer)
    ac97_init();       // AC97 audio codec

    // Phase 4: Initialize NVIDIA GPU driver stack
    gpu_init();                 // PCI discovery, BAR mapping, chip ID
    nv_mem_init();              // GPU memory management (VRAM allocator, VM)
    nv_power_init();            // Power/thermal management, clock control
    nv_fifo_init();             // PFIFO command submission engine
    nv_display_init();          // Display engine, mode setting
    nv_2d_init();               // 2D acceleration engine
    nv_3d_init();               // 3D graphics engine
    gl_init();                  // OpenGL 1.x subset API
    compositor_init();          // Window compositor

    // Phase 2: Initialize USB subsystem (xHCI + device enumeration)
    usb_init();
    usb_hid_init();

    // Initialize process management subsystem
    process_init();
    scheduler_init();

    // Phase 1: Initialize syscall interface (sets up SYSCALL/SYSRET MSRs)
    syscall_init();

    // Create system daemon processes
    process_create("desktop", (void(*)(void))0, PRIORITY_HIGH);
    process_create("input", (void(*)(void))0, PRIORITY_HIGH);
    process_create("display", (void(*)(void))0, PRIORITY_NORMAL);

    // --- PS/2 Initialization (all done with interrupts disabled) ---
    keyboard_init();
    mouse_init();

    // Flush any stale bytes left over from mouse_init
    { int flush_t = 2048; while (flush_t-- > 0 && (inb(0x64) & 1)) inb(0x60); }

    // Enable PS/2 auxiliary device (mouse)
    outb(0x64, 0xA8);
    { volatile int d; for (d = 0; d < 10000; d++); } // Small delay

    // Flush again after aux enable
    { int flush_t = 2048; while (flush_t-- > 0 && (inb(0x64) & 1)) inb(0x60); }

    // Read PS/2 controller config byte
    outb(0x64, 0x20);
    { int timeout = 100000; while (timeout-- > 0) { if (inb(0x64) & 1) break; } }
    uint8_t ps2_config = inb(0x60);

    // Bit 0 = Enable keyboard interrupt (IRQ1)
    // Bit 1 = Enable auxiliary/mouse interrupt (IRQ12)
    // Bit 6 = Enable scancode translation (set 2 -> set 1)
    ps2_config |= 0x43;   // Set bits 0, 1, 6
    ps2_config &= ~0x30;  // Clear bits 4,5 (enable both clocks)

    // Write config byte back
    outb(0x64, 0x60);
    { int timeout = 100000; while (timeout-- > 0) { if (!(inb(0x64) & 2)) break; } }
    outb(0x60, ps2_config);

    // Final flush before enabling interrupts
    { int flush_t = 2048; while (flush_t-- > 0 && (inb(0x64) & 1)) inb(0x60); }

    // Install IRQ handlers BEFORE enabling interrupts
    irq_install_handler(1, keyboard_irq_handler);
    irq_install_handler(12, mouse_irq_handler);
    irq_enable_all();

    // NOW safe to enable interrupts - handlers are installed
    __asm__ __volatile__("sti");

    // Init windows
    for (int i=0; i<MAX_WINDOWS; i++) windows[i].active=0;
    window_count=0; focused_win=-1;

    // Initial desktop render
    draw_gradient_bg();
    flip_buffer();

    // ---- Main loop ----
    while (1) {
        tick_count++;

        // Update clock
        if (tick_count % 100 == 0) {
            sys_sec++;
            if (sys_sec >= 60) { sys_sec = 0; sys_min++; }
            if (sys_min >= 60) { sys_min = 0; sys_hour++; }
            if (sys_hour >= 24) sys_hour = 0;
        }

        // Poll mouse state
        mouse_state_t ms;
        mouse_get_state(&ms);
        old_mx = mx; old_my = my;
        mx = ms.x; my = ms.y;
        if (mx < 0) mx = 0;
        if (mx >= SCR_W) mx = SCR_W - 1;
        if (my < 0) my = 0;
        if (my >= SCR_H) my = SCR_H - 1;

        mouse_left_prev = mouse_left;
        mouse_right_prev = mouse_right;
        mouse_left = (ms.buttons & 1);
        mouse_right = (ms.buttons & 2);

        // Left click
        if (mouse_left && !mouse_left_prev) {
            handle_click(mx, my);
        }

        // Right click - desktop context menu
        if (mouse_right && !mouse_right_prev) {
            int on_win = 0;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (!windows[i].active || windows[i].minimized) continue;
                if (mx >= windows[i].x && mx < windows[i].x + windows[i].w &&
                    my >= windows[i].y && my < windows[i].y + windows[i].h) { on_win = 1; break; }
            }
            if (!on_win && my < TASKBAR_Y) {
                ctx_open = 1; ctx_x = mx; ctx_y = my;
                start_menu_open = 0;
            }
        }

        // Window dragging
        if (dragging && mouse_left) {
            if (drag_win >= 0 && windows[drag_win].active) {
                windows[drag_win].x = mx - drag_ox;
                windows[drag_win].y = my - drag_oy;
                if (windows[drag_win].y < 0) windows[drag_win].y = 0;
                if (windows[drag_win].y > TASKBAR_Y - TITLE_H)
                    windows[drag_win].y = TASKBAR_Y - TITLE_H;
            }
        }
        if (!mouse_left) { dragging = 0; drag_win = -1; }

        // Paint drawing
        if (focused_win >= 0 && windows[focused_win].active &&
            windows[focused_win].app_type == APP_PAINT)
            paint_check_draw();

        // Poll network (receive packets, TCP timers)
        socket_poll();

        // Render every 2 ticks
        if (tick_count % 2 == 0) render_desktop();

        // Small delay for loop pacing
        for (volatile int d = 0; d < 50000; d++);
    }
}
