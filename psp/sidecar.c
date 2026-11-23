/* sidecar: the PSP as a Generic USB Display (Linux "gud" DRM driver).
 *
 * A kernel PRX holding one USB function driver: a vendor-class interface
 * with a bulk OUT endpoint, speaking the GUD protocol (include/drm/gud.h in
 * Linux). The host sees a 480x272 RGB565 panel and sends only the damaged
 * rectangles; nothing here polls, the one thread sleeps on an event flag.
 *
 * Two ways onto the bus, picked at start by whether USB is already active:
 *   beside PSPLink  ldstart-ed from pspsh; the bus is restarted once with
 *                   usbhostfs and this driver both on it (the device stays
 *                   054c:01c9, the display is interface 1, class 0xfe so
 *                   that a host rule can name it without matching
 *                   usbhostfs's vendor-class interface 0). modstop takes
 *                   the driver off the bus again.
 *   standalone      loaded by app/EBOOT.PBP; this module starts the
 *                   bus itself, alone on it
 *
 * Nothing is written to flash; the only state outside RAM is the log.
 * Options after the path in module_start's arguments:
 *   gudid   standalone only: present 1d50:614d, the ID Linux's gud binds to
 *   alone   from PSPLink, take usbhostfs off the bus first: the standalone
 *           bus for a test, pspsh is gone until the next power cycle
 *   log     write ms0:/sidecar.log, every bring-up step and every 3 s
 *
 * SELECT on the console opens a small menu over the picture (up/down, then
 * left/right or X to change, O or SELECT to close). The buttons arrive
 * through the controller driver's callback; nothing samples the pad.
 */
#include <pspkernel.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <pspdisplay.h>
#include <pspdisplay_kernel.h>
#include <pspctrl.h>
#include <pspctrl_kernel.h>
#include <psppower.h>
#include <pspsdk.h>
#include <string.h>

#include "edid.h"

PSP_MODULE_INFO("sidecar", PSP_MODULE_KERNEL, 1, 0);

#define DRIVER_NAME "SidecarDriver"
#define PSPLINK_PID 0x1c9

#define W 480
#define H 272
#define STRIDE 512
#define FRAME_BYTES (W * H * 2)
#define MAX_BUFFER FRAME_BYTES

/* GUD protocol, from include/drm/gud.h */
#define GUD_MAGIC 0x1d50614d
#define GUD_REQ_GET_STATUS 0x00
#define GUD_REQ_GET_DESCRIPTOR 0x01
#define GUD_REQ_GET_FORMATS 0x40
#define GUD_REQ_GET_PROPERTIES 0x41
#define GUD_REQ_GET_CONNECTORS 0x50
#define GUD_REQ_GET_CONNECTOR_PROPERTIES 0x51
#define GUD_REQ_SET_CONNECTOR_FORCE_DETECT 0x53
#define GUD_REQ_GET_CONNECTOR_STATUS 0x54
#define GUD_REQ_GET_CONNECTOR_MODES 0x55
#define GUD_REQ_GET_CONNECTOR_EDID 0x56
#define GUD_REQ_SET_BUFFER 0x60
#define GUD_REQ_SET_STATE_CHECK 0x61
#define GUD_REQ_SET_STATE_COMMIT 0x62
#define GUD_REQ_SET_CONTROLLER_ENABLE 0x63
#define GUD_REQ_SET_DISPLAY_ENABLE 0x64
#define GUD_STATUS_OK 0x00
#define GUD_STATUS_REQUEST_NOT_SUPPORTED 0x02
#define GUD_PIXEL_FORMAT_RGB565 0x40
#define GUD_MODE_FLAG_PREFERRED (1 << 10)
/* Not GUD, never sent by the kernel driver: gudtest presses a button
 * (wValue = PSP_CTRL_* bits) to drive the menu on an unattended bench. */
#define TEST_REQ_BUTTONS 0x7e

struct gud_descriptor {
    u32 magic;
    u8 version;
    u32 flags;
    u8 compression;
    u32 max_buffer_size;
    u32 min_width, max_width, min_height, max_height;
} __attribute__((packed));

struct gud_mode {
    u32 clock;
    u16 hdisplay, hsync_start, hsync_end, htotal;
    u16 vdisplay, vsync_start, vsync_end, vtotal;
    u32 flags;
} __attribute__((packed));

struct gud_property {
    u16 prop;
    u64 val;
} __attribute__((packed));

struct gud_set_buffer {
    u32 x, y, width, height, length;
    u8 compression;
    u32 compressed_length;
} __attribute__((packed));

/* ---- log: a RAM buffer, written to sidecar.log when asked for ---- */

static char g_log[8192];
static int g_loglen, g_logging;
static char g_logpath[32];

static void log_str(const char *s)
{
    int k = pspSdkDisableInterrupts();
    while (*s && g_loglen < (int)sizeof g_log - 1)
        g_log[g_loglen++] = *s++;
    pspSdkEnableInterrupts(k);
}

static void log_hex(u32 v)
{
    char b[10];
    int i;
    for (i = 0; i < 8; i++)
        b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[8] = ' ';
    b[9] = 0;
    log_str(b);
}

static void log_kv(const char *k, u32 v)
{
    log_str(k);
    log_str(" ");
    log_hex(v);
    log_str("\n");
}

/* ---- USB descriptors, laid out the way usbhostfs does ---- */

static struct DeviceDescriptor devdesc = {
    .bLength = 18, .bDescriptorType = 1, .bcdUSB = 0x200,
    .bMaxPacketSize = 64, .bcdDevice = 0x100, .bNumConfigurations = 1,
};

static struct ConfigDescriptor confdesc = {
    .bLength = 9, .bDescriptorType = 2, .wTotalLength = 9 + 9 + 2 * 7,
    .bNumInterfaces = 1, .bConfigurationValue = 1, .bmAttributes = 0xC0,
};

static struct InterfaceDescriptor interdesc = {
    .bLength = 9, .bDescriptorType = 4, .bNumEndpoints = 2,
    .bInterfaceClass = 0xFF, .bInterfaceSubClass = 0, .bInterfaceProtocol = 0,
    .iInterface = 0,
};

/* Bulk IN first, bulk OUT second: the order usbhostfs runs with. The IN
 * endpoint is not used by GUD. */
static struct EndpointDescriptor endpdesc[2] = {
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x81, .bmAttributes = 2 },
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x02, .bmAttributes = 2 },
};

static unsigned char strp[] = { 0x8, 0x3, 'P', 0, 'S', 0, 'P', 0 };

static struct UsbEndpoint endp[3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } };
static struct UsbInterface intp = { 0xFFFFFFFF, 0, 1 };
static struct UsbData usbdata[2];

/* ---- state ---- */

#define EV_SETBUF 0x01
#define EV_BULK 0x02
#define EV_ENABLE 0x04
#define EV_DETACH 0x08
#define EV_STOP 0x10
#define EV_BUTTON 0x20

static SceUID g_ev = -1, g_thid = -1, g_stage_blk = -1;
static int g_gudid, g_alone, g_standalone;
static u8 g_status;
static volatile int g_display_on, g_armed;
static volatile struct gud_set_buffer g_next;
static u32 g_stat_rects, g_stat_bytes, g_stat_errors;

static unsigned char g_ctl_in[256] __attribute__((aligned(64)));
static unsigned char g_ctl_out[512] __attribute__((aligned(64)));
static struct DeviceRequest g_ctl_out_req;
static struct UsbdDeviceReq g_ep0_send, g_ep0_recv, g_bulk;
/* Where a rectangle lands before it is drawn. Too big for the kernel
 * partition under PSPLink, so it comes from user memory. */
static unsigned char *g_stage;

/* The menu's box on the screen, and what the picture holds underneath it
 * while the menu is open. */
#define MENU_X 72
#define MENU_Y 96
#define MENU_W 336
#define MENU_H 80
#define MENU_BUTTONS (PSP_CTRL_SELECT | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | \
                      PSP_CTRL_RIGHT | PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)
static u16 *g_under;
static int g_menu_open, g_menu_row;
static volatile u32 g_buttons;
static int g_dim = 1;         /* let the system dim the screen when the picture rests */
static int g_backlight = -1;  /* level to return to after the host switched the display off */
static u32 g_last_tick;

static u16 *vram(void)
{
    return (u16 *)0x44000000;
}

/* ---- control transfers ---- */

static int ep0_send(int len, int wlength)
{
    if (len > wlength)
        len = wlength;
    sceKernelDcacheWritebackRange(g_ctl_in, sizeof g_ctl_in);
    memset(&g_ep0_send, 0, sizeof g_ep0_send);
    g_ep0_send.endp = &endp[0];
    g_ep0_send.data = g_ctl_in;
    g_ep0_send.size = len;
    return sceUsbbdReqSend(&g_ep0_send);
}

static int handle_get(struct DeviceRequest *req)
{
    int len;

    switch (req->bRequest) {
    case GUD_REQ_GET_STATUS:
        g_ctl_in[0] = g_status;
        g_status = GUD_STATUS_OK;
        len = 1;
        break;
    case GUD_REQ_GET_DESCRIPTOR: {
        struct gud_descriptor d = {
            .magic = GUD_MAGIC, .version = 1, .max_buffer_size = MAX_BUFFER,
            .min_width = W, .max_width = W, .min_height = H, .max_height = H,
        };
        memcpy(g_ctl_in, &d, len = sizeof d);
        break;
    }
    case GUD_REQ_GET_FORMATS:
        g_ctl_in[0] = GUD_PIXEL_FORMAT_RGB565;
        len = 1;
        break;
    case GUD_REQ_GET_PROPERTIES:
    case GUD_REQ_GET_CONNECTOR_PROPERTIES: {
        /* One property the host does not know and skips: an empty list
         * would need a zero-length data stage. */
        struct gud_property p = { .prop = 0x7fff, .val = 0 };
        memcpy(g_ctl_in, &p, len = sizeof p);
        break;
    }
    case GUD_REQ_GET_CONNECTORS:
        memset(g_ctl_in, 0, len = 5); /* one panel, no flags */
        break;
    case GUD_REQ_GET_CONNECTOR_STATUS:
        g_ctl_in[0] = 1; /* connected */
        len = 1;
        break;
    case GUD_REQ_GET_CONNECTOR_MODES: {
        struct gud_mode m = {
            .clock = 9000,
            .hdisplay = 480, .hsync_start = 482, .hsync_end = 523, .htotal = 525,
            .vdisplay = 272, .vsync_start = 274, .vsync_end = 284, .vtotal = 286,
            .flags = GUD_MODE_FLAG_PREFERRED,
        };
        memcpy(g_ctl_in, &m, len = sizeof m);
        break;
    }
    case GUD_REQ_GET_CONNECTOR_EDID:
        memcpy(g_ctl_in, gud_edid, len = sizeof gud_edid);
        break;
    default:
        g_status = GUD_STATUS_REQUEST_NOT_SUPPORTED;
        return -1;
    }
    return ep0_send(len, req->wLength) < 0 ? -1 : 0;
}

/* The data stage of a SET request has arrived. Interrupt context. */
static int ctl_out_done(struct UsbdDeviceReq *r, int arg2, int arg3)
{
    switch (g_ctl_out_req.bRequest) {
    case GUD_REQ_SET_BUFFER:
        memcpy((void *)&g_next, g_ctl_out, sizeof g_next);
        sceKernelSetEventFlag(g_ev, EV_SETBUF);
        break;
    case GUD_REQ_SET_DISPLAY_ENABLE:
        g_display_on = g_ctl_out[0];
        sceKernelSetEventFlag(g_ev, EV_ENABLE);
        break;
    }
    return 0;
}

static int handle_set(struct DeviceRequest *req)
{
    switch (req->bRequest) {
    case GUD_REQ_SET_CONNECTOR_FORCE_DETECT:
    case GUD_REQ_SET_STATE_COMMIT:
    case GUD_REQ_SET_BUFFER:
    case GUD_REQ_SET_STATE_CHECK:
    case GUD_REQ_SET_CONTROLLER_ENABLE:
    case GUD_REQ_SET_DISPLAY_ENABLE:
        break;
    case TEST_REQ_BUTTONS:
        g_buttons |= req->wValue & MENU_BUTTONS;
        sceKernelSetEventFlag(g_ev, EV_BUTTON);
        break;
    default:
        g_status = GUD_STATUS_REQUEST_NOT_SUPPORTED;
        return -1;
    }
    if (req->wLength == 0)
        return 0;
    if (req->wLength > sizeof g_ctl_out)
        return -1;
    g_ctl_out_req = *req;
    sceKernelDcacheInvalidateRange(g_ctl_out, sizeof g_ctl_out);
    memset(&g_ep0_recv, 0, sizeof g_ep0_recv);
    g_ep0_recv.endp = &endp[0];
    g_ep0_recv.data = g_ctl_out;
    g_ep0_recv.size = req->wLength;
    g_ep0_recv.func = ctl_out_done;
    return sceUsbbdReqRecv(&g_ep0_recv) < 0 ? -1 : 0;
}

static int usb_request(int arg1, int arg2, struct DeviceRequest *req)
{
    if (g_logging) {
        log_str("ctl ");
        log_hex(arg1);
        log_hex(arg2);
        log_hex(req->bmRequestType << 8 | req->bRequest);
        log_hex(req->wValue);
        log_hex(req->wIndex);
        log_hex(req->wLength);
        log_str("\n");
    }
    if (arg2 < 0)
        return -1;
    if ((req->bmRequestType & 0x7f) != 0x41) /* vendor, to an interface */
        return -1;
    return req->bmRequestType & 0x80 ? handle_get(req) : handle_set(req);
}

static int usb_func28(int arg1, int arg2, int arg3)
{
    return 0;
}

static int usb_attach(int speed, void *arg2, void *arg3)
{
    if (g_logging)
        log_kv("attach speed", speed);
    return 0;
}

static int usb_detach(int arg1, int arg2, int arg3)
{
    if (g_ev >= 0)
        sceKernelSetEventFlag(g_ev, EV_DETACH);
    return 0;
}

/* ---- pixels ---- */

static int bulk_done(struct UsbdDeviceReq *r, int arg2, int arg3)
{
    sceKernelSetEventFlag(g_ev, EV_BULK);
    return 0;
}

static void arm_bulk(int len)
{
    sceKernelDcacheInvalidateRange(g_stage, (len + 63) & ~63);
    memset(&g_bulk, 0, sizeof g_bulk);
    g_bulk.endp = &endp[2];
    g_bulk.data = g_stage;
    g_bulk.size = len;
    g_bulk.func = bulk_done;
    g_armed = 1;
    if (sceUsbbdReqRecv(&g_bulk) < 0) {
        g_armed = 0;
        g_stat_errors++;
    }
}

/* DRM RGB565 has red in bits 11-15, the PSP's 5650 has it in bits 0-4. */
static void convert(u16 *dst, const u16 *src, int n)
{
    while (n-- > 0) {
        u32 p = *src++;
        *dst++ = (u16)(p >> 11 | (p & 0x07e0) | p << 11);
    }
}

/* With the menu open, what falls under its box goes to g_under instead, so
 * the picture is current when the menu closes. */
static void blit(const struct gud_set_buffer *b)
{
    const u16 *src = (const u16 *)g_stage;
    int x0 = b->x, x1 = b->x + b->width, y;

    for (y = b->y; y < (int)(b->y + b->height); y++, src += b->width) {
        u16 *row = vram() + y * STRIDE;
        if (g_menu_open && y >= MENU_Y && y < MENU_Y + MENU_H && x1 > MENU_X &&
            x0 < MENU_X + MENU_W) {
            int a = x0 < MENU_X ? MENU_X : x0;
            int e = x1 > MENU_X + MENU_W ? MENU_X + MENU_W : x1;
            convert(row + x0, src, a - x0);
            convert(g_under + (y - MENU_Y) * MENU_W + (a - MENU_X), src + (a - x0), e - a);
            convert(row + e, src + (e - x0), x1 - e);
        } else {
            convert(row + x0, src, b->width);
        }
    }
}

static void show(void)
{
    sceDisplaySetMode(0, W, H);
    sceDisplaySetFrameBuf(vram(), STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
}

/* ---- the menu ---- */

extern u8 msx[]; /* the SDK's 8x8 font, from libpspdebug */

#define COL_TEXT 0xffff
#define COL_PICK 0x07ff /* yellow */
#define COL_BACK 0x18a2
#define COL_EDGE 0x8410

static const int brightness_steps[] = { 20, 40, 60, 80, 100 };
#define N_STEPS ((int)(sizeof brightness_steps / sizeof brightness_steps[0]))

static int brightness_step(void)
{
    int level = 0, unk = 0, i, best = 0;

    sceDisplayGetBrightness(&level, &unk);
    for (i = 1; i < N_STEPS; i++)
        if (level > (brightness_steps[i - 1] + brightness_steps[i]) / 2)
            best = i;
    return best;
}

static const char *dim_value(void)
{
    return g_dim ? "yes" : "no";
}

static void dim_change(int dir)
{
    g_dim = !g_dim;
}

static const char *brightness_value(void)
{
    static char text[4];
    int v = brightness_steps[brightness_step()], n = 0;

    if (v >= 100)
        text[n++] = '1';
    if (v >= 10)
        text[n++] = '0' + v / 10 % 10;
    text[n++] = '0' + v % 10;
    text[n] = 0;
    return text;
}

static void brightness_change(int dir)
{
    int i = brightness_step() + dir;

    if (i < 0)
        i = 0;
    if (i >= N_STEPS)
        i = N_STEPS - 1;
    sceDisplaySetBrightness(brightness_steps[i], 0);
}

/* One line per option: a further option is a further entry. A setting
 * lives until the module is unloaded. */
static const struct menu_item {
    const char *label;
    const char *(*value)(void);
    void (*change)(int dir); /* -1 left, +1 right or X */
} menu[] = {
    { "Dim when idle", dim_value, dim_change },
    { "Brightness", brightness_value, brightness_change },
};
#define N_ITEMS ((int)(sizeof menu / sizeof menu[0]))

static void fill(int x, int y, int w, int h, u16 c)
{
    int i, j;
    for (j = 0; j < h; j++) {
        u16 *p = vram() + (y + j) * STRIDE + x;
        for (i = 0; i < w; i++)
            *p++ = c;
    }
}

/* 8x8 glyphs drawn twice the size */
static void text(int x, int y, const char *s, u16 c)
{
    for (; *s; s++, x += 16) {
        const u8 *g = msx + (u8)*s * 8;
        int r, b;
        for (r = 0; r < 8; r++)
            for (b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b))
                    fill(x + 2 * b, y + 2 * r, 2, 2, c);
    }
}

static void menu_draw(void)
{
    int i;

    fill(MENU_X, MENU_Y, MENU_W, MENU_H, COL_EDGE);
    fill(MENU_X + 2, MENU_Y + 2, MENU_W - 4, MENU_H - 4, COL_BACK);
    text(MENU_X + 12, MENU_Y + 10, "PSP display", COL_EDGE);
    for (i = 0; i < N_ITEMS; i++) {
        u16 c = i == g_menu_row ? COL_PICK : COL_TEXT;
        int y = MENU_Y + 34 + 20 * i;
        text(MENU_X + 12, y, menu[i].label, c);
        text(MENU_X + MENU_W - 12 - 16 * 3, y, menu[i].value(), c);
    }
}

static void menu_show(int open)
{
    int y;

    if (open == g_menu_open)
        return;
    for (y = 0; y < MENU_H; y++) {
        u16 *scr = vram() + (MENU_Y + y) * STRIDE + MENU_X;
        if (open)
            memcpy(g_under + y * MENU_W, scr, MENU_W * 2);
        else
            memcpy(scr, g_under + y * MENU_W, MENU_W * 2);
    }
    g_menu_open = open;
    if (open)
        menu_draw();
}

static void menu_buttons(u32 down)
{
    if (!g_menu_open) {
        if (down & PSP_CTRL_SELECT) {
            show(); /* the menu also comes up before the host's first picture */
            menu_show(1);
        }
        return;
    }
    if (down & (PSP_CTRL_SELECT | PSP_CTRL_CIRCLE)) {
        menu_show(0);
        return;
    }
    if (down & PSP_CTRL_UP)
        g_menu_row = (g_menu_row + N_ITEMS - 1) % N_ITEMS;
    if (down & PSP_CTRL_DOWN)
        g_menu_row = (g_menu_row + 1) % N_ITEMS;
    if (down & PSP_CTRL_LEFT)
        menu[g_menu_row].change(-1);
    if (down & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS))
        menu[g_menu_row].change(1);
    menu_draw();
}

/* From the controller driver, when one of MENU_BUTTONS changes. */
static void button_callback(u32 cur, u32 last, void *arg)
{
    u32 down = cur & ~last & MENU_BUTTONS;

    if (down && g_ev >= 0) {
        g_buttons |= down;
        sceKernelSetEventFlag(g_ev, EV_BUTTON);
    }
}

/* The host switched the output off or on (DPMS): the backlight follows. */
static void backlight(int on)
{
    int level = 0, unk = 0;

    if (!on && g_backlight < 0) {
        sceDisplayGetBrightness(&level, &unk);
        g_backlight = level;
        sceDisplaySetBrightness(0, 0);
    } else if (on && g_backlight >= 0) {
        sceDisplaySetBrightness(g_backlight, 0);
        g_backlight = -1;
    }
}

/* ---- the bus ---- */

static void fill_descriptors(void)
{
    int i;

    memset(usbdata, 0, sizeof usbdata);
    for (i = 0; i < 2; i++) { /* 0 hi-speed, 1 full-speed */
        int mps = i == 0 ? 512 : 64;
        endpdesc[0].wMaxPacketSize = mps;
        endpdesc[1].wMaxPacketSize = mps;
        memcpy(usbdata[i].devdesc, &devdesc, sizeof devdesc);
        usbdata[i].config.pconfdesc = &usbdata[i].confdesc;
        usbdata[i].config.pinterfaces = &usbdata[i].interfaces;
        usbdata[i].config.pinterdesc = &usbdata[i].interdesc;
        usbdata[i].config.pendp = usbdata[i].endp;
        memcpy(usbdata[i].confdesc.desc, &confdesc, sizeof confdesc);
        usbdata[i].confdesc.pinterfaces = &usbdata[i].interfaces;
        usbdata[i].interfaces.pinterdesc[0] = &usbdata[i].interdesc;
        usbdata[i].interfaces.intcount = 1;
        memcpy(usbdata[i].interdesc.desc, &interdesc, sizeof interdesc);
        usbdata[i].interdesc.pendp = usbdata[i].endp;
        memcpy(usbdata[i].endp[0].desc, &endpdesc[0], sizeof endpdesc[0]);
        memcpy(usbdata[i].endp[1].desc, &endpdesc[1], sizeof endpdesc[1]);
    }
}

static int usb_start(int size, void *p)
{
    extern struct UsbDriver g_driver;

    fill_descriptors();
    g_driver.devp_hi = usbdata[0].devdesc;
    g_driver.confp_hi = &usbdata[0].config;
    g_driver.devp = usbdata[1].devdesc;
    g_driver.confp = &usbdata[1].config;
    return 0;
}

static int usb_stop(int size, void *p)
{
    return 0;
}

struct UsbDriver g_driver = {
    DRIVER_NAME, 3, endp, &intp,
    NULL, NULL, NULL, NULL,
    (struct StringDescriptor *)strp,
    usb_request, usb_func28, usb_attach, usb_detach,
    0, usb_start, usb_stop, NULL,
};

static void write_log(void)
{
    SceUID fd = sceIoOpen(g_logpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, g_log, g_loglen);
        sceIoClose(fd);
    }
}

/* A step of the bring-up: logged, and the log on the Memory Stick before
 * the next one, so a hang leaves a trail. */
static void step(const char *what, u32 v)
{
    log_kv(what, v);
    if (g_logging)
        write_log();
}

static int wait_connected(int seconds)
{
    int i;
    for (i = 0; i < seconds * 10; i++) {
        if (sceUsbGetState() & PSP_USB_CONNECTION_ESTABLISHED)
            return 1;
        sceKernelDelayThread(100 * 1000);
    }
    return 0;
}

/* A device descriptor as the bus driver holds it once activated: Sony's
 * vendor ID and the product ID given to sceUsbActivate. */
static int is_bus_devdesc(const u8 *d)
{
    return d[0] == 18 && d[1] == 1 && d[8] == 0x4c && d[9] == 0x05 &&
           d[10] == (PSPLINK_PID & 0xff) && d[11] == (PSPLINK_PID >> 8) && d[17] == 1;
}

/* The bus driver answers the device descriptor itself, from its own copy
 * (RAM, hi-speed and full-speed descriptor 20 bytes apart). The host's gud
 * driver binds by itself only to a GUD ID, so that copy is rewritten.
 * No offsets: the bus driver's own memory is searched for a pointer into
 * kernel RAM at which two such descriptors stand. Exactly one distinct
 * place must turn up; otherwise nothing is touched and the console stays
 * 054c:01c9. */
static void set_ids(void)
{
    SceModule *m = sceKernelFindModuleByName("sceUSB_Driver");
    u32 size, a;
    u8 *d = NULL;
    int found = 0, i;

    if (!g_gudid || !m)
        return;
    size = m->text_size + m->data_size + m->bss_size;
    if (size < 0x1000 || size > 0x40000) {
        step("bus driver size not plausible", size);
        return;
    }
    for (a = m->text_addr; a + 4 <= m->text_addr + size; a += 4) {
        u32 p = *(u32 *)a;
        if ((p & 3) || p < 0x88000000 || p > 0x88400000 - 40)
            continue;
        if (!is_bus_devdesc((u8 *)p) || !is_bus_devdesc((u8 *)p + 20))
            continue;
        if (d != (u8 *)p)
            found++;
        d = (u8 *)p;
    }
    if (found != 1) {
        step("bus descriptors not found, places", found);
        return;
    }
    step("bus descriptors at", (u32)d);
    for (i = 0; i < 2; i++) {
        u8 *v = d + 20 * i;
        v[8] = 0x50;
        v[9] = 0x1d;
        v[10] = 0x4d;
        v[11] = 0x61;
    }
    sceKernelDcacheWritebackRange((void *)((u32)d & ~63), 128);
    step("ids set", 0x1d50614d);
}

static int g_beside;     /* on the bus next to usbhostfs */
static int g_registered; /* known to the bus driver */
static int g_started;    /* started on the bus */

static void bus_up(void)
{
    int state = sceUsbGetState();

    step("state before", state);
    g_standalone = !(state & PSP_USB_ACTIVATED);
    /* Vendor class where the device carries a GUD ID (the host's gud driver
     * asks for it); beside PSPLink a class of its own, see the header. */
    interdesc.bInterfaceClass = g_standalone || g_alone ? 0xFF : 0xFE;
    if (g_standalone) {
        step("start bus", sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0));
        step("start gud", sceUsbStart(DRIVER_NAME, 0, 0));
        step("activate", sceUsbActivate(PSPLINK_PID));
        set_ids();
    } else if (g_alone) {
        /* The standalone bus, reached from PSPLink for a test: usbhostfs
         * leaves the bus, so pspsh is gone until the next power cycle. */
        sceKernelDelayThread(500 * 1000);
        step("deactivate", sceUsbDeactivate(PSPLINK_PID));
        step("stop hostfs", sceUsbStop("USBHostFSDriver", 0, 0));
        step("start gud", sceUsbStart(DRIVER_NAME, 0, 0));
        step("activate", sceUsbActivate(PSPLINK_PID));
        set_ids();
    } else {
        /* Let pspsh get its answer to ldstart before the cable drops. */
        sceKernelDelayThread(500 * 1000);
        step("deactivate", sceUsbDeactivate(PSPLINK_PID));
        step("start gud", sceUsbStart(DRIVER_NAME, 0, 0));
        step("activate", sceUsbActivate(PSPLINK_PID));
        g_beside = 1;
        if (!wait_connected(15)) {
            /* The host did not take the two drivers together: give the
             * bus back to PSPLink alone. */
            step("no connection, state", sceUsbGetState());
            step("deactivate", sceUsbDeactivate(PSPLINK_PID));
            step("stop gud", sceUsbStop(DRIVER_NAME, 0, 0));
            step("activate", sceUsbActivate(PSPLINK_PID));
            g_beside = 0;
        }
    }
    g_started = g_standalone || g_alone || g_beside;
    step("state after", sceUsbGetState());
    step("ep1 bus", endp[1].unk2);
    step("ep2 bus", endp[2].unk2);
}

static int gud_thread(SceSize size, void *argp)
{
    struct gud_set_buffer cur = { 0 };
    SceUInt timeout;
    u32 bits;

    g_registered = sceUsbbdRegister(&g_driver) >= 0;
    step("registered", g_registered);
    bus_up();
    step("buttons", sceCtrlRegisterButtonCallback(3, MENU_BUTTONS, button_callback, NULL));

    for (;;) {
        /* Without the log the only wake-up besides USB and a button is a
         * power tick every 30 s, so the console does not fall asleep as a
         * monitor. With "Dim when idle" that tick leaves the display's
         * own idle timer alone, and the firmware dims as it always does. */
        timeout = g_logging ? 3 * 1000 * 1000 : 30 * 1000 * 1000;
        bits = 0;
        if (sceKernelWaitEventFlag(g_ev, 0xff, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
                                   &bits, &timeout) < 0) {
            if (g_display_on)
                scePowerTick(g_dim ? PSP_POWER_TICK_SUSPEND : PSP_POWER_TICK_ALL);
            if (g_logging) {
                log_kv("rects", g_stat_rects);
                log_kv("bytes", g_stat_bytes);
                log_kv("errors", g_stat_errors);
                write_log();
            }
            continue;
        }
        if (bits & EV_STOP)
            break;
        if (bits & EV_DETACH) {
            if (g_armed) {
                sceUsbbdReqCancelAll(&endp[2]);
                g_armed = 0;
            }
            bits &= ~EV_BULK;
        }
        if (bits & EV_ENABLE) {
            if (g_display_on)
                show();
            backlight(g_display_on);
        }
        if (bits & EV_BUTTON) {
            int k = pspSdkDisableInterrupts();
            u32 down = g_buttons;
            g_buttons = 0;
            pspSdkEnableInterrupts(k);
            menu_buttons(down);
        }
        if (bits & EV_BULK) {
            g_armed = 0;
            if (g_bulk.retcode == 0 && g_bulk.recvsize == (int)cur.length) {
                blit(&cur);
                /* A picture that moves counts as activity, at most once a
                 * second: the screen comes back from the system's dimming,
                 * and a stream that never lets the wait above time out
                 * still keeps the console awake. */
                if (sceKernelGetSystemTimeLow() - g_last_tick > 1000000) {
                    g_last_tick = sceKernelGetSystemTimeLow();
                    scePowerTick(PSP_POWER_TICK_ALL);
                }
                g_stat_rects++;
                g_stat_bytes += cur.length;
            } else {
                g_stat_errors++;
            }
        }
        if (bits & EV_SETBUF) {
            int k = pspSdkDisableInterrupts();
            memcpy(&cur, (void *)&g_next, sizeof cur);
            pspSdkEnableInterrupts(k);
            if (g_armed) { /* the host gave up on the last transfer */
                sceUsbbdReqCancelAll(&endp[2]);
                g_armed = 0;
            }
            /* Each value on its own first: the sums and the product of
             * unchecked 32-bit values from the host would wrap. */
            if (cur.compression == 0 && cur.x < W && cur.y < H && cur.width >= 1 &&
                cur.height >= 1 && cur.width <= W - cur.x && cur.height <= H - cur.y &&
                cur.length == cur.width * cur.height * 2)
                arm_bulk(cur.length);
            else
                g_stat_errors++;
        }
    }

    if (g_armed)
        sceUsbbdReqCancelAll(&endp[2]);
    sceCtrlRegisterButtonCallback(3, 0, NULL, NULL);
    menu_show(0);
    backlight(1);
    return 0;
}

int module_start(SceSize args, void *argp)
{
    const char *p = argp, *end = p + args;

    /* argv[0] is the path, the rest are the options */
    strcpy(g_logpath, "ms0:/sidecar.log");
    for (p += strlen(p) + 1; p < end; p += strlen(p) + 1) {
        if (!strcmp(p, "gudid"))
            g_gudid = 1;
        else if (!strcmp(p, "alone"))
            g_alone = 1;
        else if (!strcmp(p, "log"))
            g_logging = 1;
    }

    {
        g_stage_blk = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "sidecar",
                                                    PSP_SMEM_High,
                                                    MAX_BUFFER + 64 + MENU_W * MENU_H * 2, NULL);
        if (g_stage_blk < 0)
            return 1;
        g_stage = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(g_stage_blk) + 63) & ~63);
        g_under = (u16 *)(g_stage + MAX_BUFFER);
    }
    memset(vram(), 0, STRIDE * H * 2);
    g_ev = sceKernelCreateEventFlag("sidecar", 0x200, 0, NULL);
    g_thid = sceKernelCreateThread("sidecar", gud_thread, 16, 0x4000, 0, NULL);
    if (g_ev < 0 || g_thid < 0)
        return 1;
    sceKernelStartThread(g_thid, 0, NULL);
    return 0;
}

/* Beside PSPLink: ends the thread and gives the bus back to usbhostfs
 * alone, all before returning (a thread left to do it later did not survive
 * the stop on 6.60). The cable drops once in here, so pspsh may not get the
 * answer to its modstop; it answers again a few seconds later, and the
 * module can then be unloaded. */
int module_stop(SceSize args, void *argp)
{
    SceUInt timeout = 2 * 1000 * 1000;

    sceKernelSetEventFlag(g_ev, EV_STOP);
    sceKernelWaitThreadEnd(g_thid, &timeout);
    sceKernelTerminateDeleteThread(g_thid);
    /* The bus must not keep descriptors and callbacks of a module that is
     * about to go: off the bus and unregistered on every path. Only beside
     * PSPLink is there someone to hand the bus back to. */
    if (g_started) {
        step("deactivate", sceUsbDeactivate(PSPLINK_PID));
        step("stop gud", sceUsbStop(DRIVER_NAME, 0, 0));
        if (g_beside)
            step("activate", sceUsbActivate(PSPLINK_PID));
    }
    if (g_registered)
        step("unregister", sceUsbbdUnregister(&g_driver));
    sceKernelDeleteEventFlag(g_ev);
    sceKernelFreePartitionMemory(g_stage_blk);
    return 0;
}
