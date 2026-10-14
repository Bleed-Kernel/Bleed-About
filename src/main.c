#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include <sysinfo.h>
#include <display.h>
#include <crime/crime.h>
#include <bleed_uikit.h>
#include <version.h>

#define LOGO_PATH       "/initrd/boot/splash.bgra"
#define KERNEL_PATH     "/initrd/proc/version"

#define LOGO_SRC_PX     256     // the boot splash, square
#define LOGO_PX         (LOGO_SRC_PX / 2)

#define NAME_PX         30
#define MIN_W           300
#define PAD             26
#define NAME_GAP        6       // logo to the name
#define ROWS_GAP        22      // above and below the rows
#define ROW_H           21
#define COL_GAP         10      // between a label and what it says

#define COLOUR_VALUE    0xB4B4BA
#define FOOTER_TEXT     "GPLv3 Licence | bleedkernel.com"

typedef struct {
    const char *label;
    char        value[96];
} row_t;

enum {
    ROW_CPU,
    ROW_MEMORY,
    ROW_DISPLAY,
    ROW_KERNEL,
    ROW_COUNT,
};

static row_t rows[ROW_COUNT] = {
    [ROW_CPU]     = { "Processor", "Unknown" },
    [ROW_MEMORY]  = { "Memory",    "Unknown" },
    [ROW_DISPLAY] = { "Display",   "Unknown" },
    [ROW_KERNEL]  = { "Kernel",    "Unknown" },
};

static ui_font_t  font_name;
static ui_image_t logo;

static int        label_w;
static int        value_w;

static void cpuid(uint32_t leaf, uint32_t regs[4]) {
    asm volatile("cpuid"
        : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
        : "a"(leaf), "c"(0));
}

// intel pads the brand string out with spaces, at the front on some and in the middle on others
static void squeeze(char *out, size_t size, const char *in) {
    size_t len = 0;

    while (*in == ' ')
        in++;
    for (; *in && len + 1 < size; in++) {
        if (*in == ' ' && (in[1] == ' ' || !in[1]))
            continue;
        out[len++] = *in;
    }
    out[len] = '\0';
}

static void read_cpu(void) {
    uint32_t regs[4];
    char     brand[49] = {0};

    cpuid(0x80000000, regs);
    if (regs[0] < 0x80000004)
        return;

    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, regs);
        memcpy(brand + i * 16, regs, 16);
    }
    if (brand[0])
        squeeze(rows[ROW_CPU].value, sizeof(rows[ROW_CPU].value), brand);
}

static void read_memory(void) {
    system_memory_info_t mem;
    memset(&mem, 0, sizeof(mem));
    if (_meminfo(&mem) != 0 || !mem.MEMORY_USABLE)
        return;

    uint64_t mib   = mem.MEMORY_USABLE / (1024 * 1024);
    char    *value = rows[ROW_MEMORY].value;
    size_t   size  = sizeof(rows[ROW_MEMORY].value);

    if (mib < 1024)
        snprintf(value, size, "%llu MB", mib);
    else
        snprintf(value, size, "%llu.%llu GB", mib / 1024, (mib % 1024) * 10 / 1024);
}

static void read_display(void) {
    struct fb_info fb;
    if (get_fb_metadata(&fb) < 0)
        return;

    snprintf(rows[ROW_DISPLAY].value, sizeof(rows[ROW_DISPLAY].value), "%llu x %llu", fb.width, fb.height);
}

static void read_kernel(void) {
    char *value = rows[ROW_KERNEL].value;
    char  file[sizeof(rows[ROW_KERNEL].value)];

    int fd = _open(KERNEL_PATH, O_RDONLY);
    if (fd < 0)
        return;
    long size = _read(fd, file, sizeof(file) - 1);
    _close(fd);

    long len = 0;
    while (len < size && file[len] > ' ')
        len++;

    if (len > 0) {
        memcpy(value, file, (size_t)len);
        value[len] = '\0';
    }
}

// the splash is a glow on black, keyed so the window shows through where it isnt lit
static void logo_load(void) {
    if (ui_image_load_raw(&logo, LOGO_PATH, LOGO_SRC_PX, LOGO_SRC_PX) < 0)
        return;

    ui_image_scale(&logo, LOGO_PX, LOGO_PX);
    ui_image_key_black(&logo);
}

static int content_height(void) {
    return LOGO_PX + NAME_GAP + ui_font_height(&font_name) + ui_font_height(&ui_font_light) +
           ROWS_GAP + ROW_COUNT * ROW_H + ROWS_GAP + ui_font_height(&ui_font_text);
}

// a line of text across the whole window, y is its top
static void centred(ui_canvas_t *c, const ui_font_t *font, int y, const char *text, uint32_t colour) {
    ui_text_in(c, font, (ui_rect_t){0, y, c->width, ui_font_height(font)}, text, colour, UI_ALIGN_CENTRE);
}

static void draw(cs_window_t *win) {
    ui_canvas_t c = ui_canvas(win->pixels, win->width, win->height, win->pitch);

    ui_fill(&c, c.clip, UI_COLOUR_FACE);

    int y = (c.height - content_height()) / 2;
    if (y < 0) y = 0;

    ui_image_draw(&c, &logo, (c.width - LOGO_PX) / 2, y);
    y += LOGO_PX + NAME_GAP;

    centred(&c, &font_name, y, "The Bleed Kernel", UI_COLOUR_TEXT);
    y += ui_font_height(&font_name);
    centred(&c, &ui_font_light, y, BLEED_VERSION, UI_COLOUR_BACK_TEXT);
    y += ui_font_height(&ui_font_light) + ROWS_GAP;

    // labels end and values start either side of the one line, the pair of columns is whats centred
    int split = (c.width - (label_w + COL_GAP + value_w)) / 2 + label_w;
    for (int i = 0; i < ROW_COUNT; i++) {
        ui_text_in(&c, &ui_font_text, (ui_rect_t){0, y, split, ROW_H}, rows[i].label, UI_COLOUR_TEXT, UI_ALIGN_RIGHT);
        ui_text_in(&c, &ui_font_text, (ui_rect_t){split + COL_GAP, y, value_w, ROW_H}, rows[i].value,
                   COLOUR_VALUE, UI_ALIGN_LEFT);
        y += ROW_H;
    }
    y += ROWS_GAP;

    centred(&c, &ui_font_text, y, FOOTER_TEXT, UI_COLOUR_BACK_TEXT);

    cs_window_commit(win, 0, 0, win->width, win->height);
}

int main(void) {
    read_cpu();
    read_memory();
    read_display();
    read_kernel();

    if (cs_connect() < 0) {
        printf("Bleed %s\n", BLEED_VERSION);
        for (int i = 0; i < ROW_COUNT; i++)
            printf("%-10s %s\n", rows[i].label, rows[i].value);
        return 0;
    }

    if (ui_init() < 0) {
        printf("about: no font ending in " UI_FONT_REGULAR " in " UI_FONT_DIR "\n");
        return 1;
    }
    if (ui_font_load(&font_name, UI_FONT_BOLD, NAME_PX) < 0)
        ui_font_load(&font_name, UI_FONT_REGULAR, NAME_PX);
    logo_load();

    for (int i = 0; i < ROW_COUNT; i++) {
        int lw = ui_text_width(&ui_font_text, rows[i].label);
        int vw = ui_text_width(&ui_font_text, rows[i].value);
        if (lw > label_w) label_w = lw;
        if (vw > value_w) value_w = vw;
    }

    // a long processor name is the only thing that makes it any wider
    int width = label_w + COL_GAP + value_w + PAD * 2;
    if (width < MIN_W) width = MIN_W;

    cs_window_t *win = cs_window_open(width, content_height() + PAD * 2, "About Bleed");
    if (!win) {
        printf("about: the desktop wouldnt give us a window\n");
        return 1;
    }
    draw(win);

    cs_event_t ev;
    for (;;) {
        cs_event_wait(&ev, -1);
        if (ev.type == CS_EV_CLOSE_REQUEST)
            break;
        if (ev.type == CS_EV_RESIZE)
            draw(win);
    }

    cs_window_close(win);
    return 0;
}
