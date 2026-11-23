/* gudtest: talk GUD to the PSP from userspace, the way Linux's gud driver
 * does. A test tool for the case where the kernel driver is not bound (the
 * PSP beside PSPLink keeps Sony's USB ID); not needed for normal use.
 *
 *   gudtest bars          colour bars and a white frame around the screen
 *   gudtest bench N       N full frames of a moving pattern, prints frames/s
 *   gudtest rects N       N 64x64 rectangles at changing positions
 *   gudtest button HEX    press PSP buttons (PSP_CTRL_* bits; 1 = SELECT) for the menu
 *
 * Opens 054c:01c9, claims the first interface after usbhostfs's
 * (interface 0) of class 0xfe or 0xff with a bulk OUT endpoint. Exits 0 on
 * success, 1 on any USB error.
 *
 *   cc -O2 -o gudtest tests/gudtest.c $(pkg-config --cflags --libs libusb-1.0)
 */
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 480
#define H 272

static libusb_device_handle *h;
static int ifnum = -1, ep_out;

static void die(const char *what, int r)
{
    fprintf(stderr, "%s: %s\n", what, r < 0 ? libusb_strerror(r) : "failed");
    exit(1);
}

static int get(int req, int value, void *buf, int len)
{
    int r = libusb_control_transfer(h, 0xc1, req, value, ifnum, buf, len, 2000);
    if (r < 0) {
        fprintf(stderr, "get %02x: %s\n", req, libusb_strerror(r));
        exit(1);
    }
    return r;
}

static void set(int req, const void *buf, int len)
{
    int r = libusb_control_transfer(h, 0x41, req, 0, ifnum, (void *)buf, len, 2000);
    if (r != len) {
        fprintf(stderr, "set %02x: %s\n", req, r < 0 ? libusb_strerror(r) : "short");
        exit(1);
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* One rectangle of RGB565 (red in the high bits), as gud_flush_rect sends it. */
static void flush(int x, int y, int w, int hgt, const uint16_t *px)
{
    uint8_t req[25] = { 0 };
    int len = w * hgt * 2, done = 0, r;

    put32(req, x);
    put32(req + 4, y);
    put32(req + 8, w);
    put32(req + 12, hgt);
    put32(req + 16, len);
    set(0x60, req, sizeof req);
    r = libusb_bulk_transfer(h, ep_out, (void *)px, len, &done, 3000);
    if (r < 0 || done != len)
        die("bulk", r);
}

static uint16_t rgb(int r, int g, int b)
{
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    static uint16_t fb[W * H];
    struct libusb_config_descriptor *cfg;
    uint8_t buf[2048];
    int r, i, n, x, y;

    if (argc < 2) {
        fprintf(stderr, "usage: gudtest bars | bench N | rects N\n");
        return 1;
    }
    libusb_init(NULL);
    h = libusb_open_device_with_vid_pid(NULL, 0x054c, 0x01c9);
    if (!h)
        die("open 054c:01c9", -1);
    if ((r = libusb_get_active_config_descriptor(libusb_get_device(h), &cfg)) < 0)
        die("config", r);
    for (i = 1; i < cfg->bNumInterfaces && ifnum < 0; i++) {
        const struct libusb_interface_descriptor *d = &cfg->interface[i].altsetting[0];
        for (n = 0; n < d->bNumEndpoints; n++)
            if (d->bInterfaceClass >= 0xfe && !(d->endpoint[n].bEndpointAddress & 0x80)) {
                ifnum = d->bInterfaceNumber;
                ep_out = d->endpoint[n].bEndpointAddress;
            }
    }
    if (ifnum < 0)
        die("no display interface", -1);
    if ((r = libusb_claim_interface(h, ifnum)) < 0)
        die("claim", r);

    /* What gud_probe asks, in its order. */
    n = get(0x01, 0, buf, 30);
    printf("descriptor %d bytes, magic %02x%02x%02x%02x, max buffer %u\n", n, buf[3], buf[2],
           buf[1], buf[0], buf[10] | buf[11] << 8 | buf[12] << 16 | (unsigned)buf[13] << 24);
    n = get(0x40, 0, buf, 32);
    printf("formats %d: %02x\n", n, buf[0]);
    printf("properties %d bytes\n", get(0x41, 0, buf, 320));
    printf("connectors %d bytes\n", get(0x50, 0, buf, 160));
    printf("connector properties %d bytes\n", get(0x51, 0, buf, 320));
    set(0x53, NULL, 0);
    get(0x54, 0, buf, 1);
    printf("connector status %d\n", buf[0]);
    printf("edid %d bytes\n", get(0x56, 0, buf, 2048));
    n = get(0x55, 0, buf, 3072 > (int)sizeof buf ? (int)sizeof buf : 3072);
    printf("modes %d bytes: %dx%d\n", n, buf[4] | buf[5] << 8, buf[12] | buf[13] << 8);

    /* gud_plane_atomic_check, then gud_crtc_atomic_enable */
    buf[24] = 0x40; /* RGB565 */
    buf[25] = 0;    /* connector 0 */
    set(0x61, buf, 26);
    buf[0] = 1;
    set(0x63, buf, 1);
    set(0x62, NULL, 0);
    buf[0] = 1;
    set(0x64, buf, 1);

    if (!strcmp(argv[1], "bars")) {
        static const int c[8][3] = { { 255, 0, 0 },     { 0, 255, 0 },   { 0, 0, 255 },
                                     { 255, 255, 0 },   { 0, 255, 255 }, { 255, 0, 255 },
                                     { 128, 128, 128 }, { 0, 0, 0 } };
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                const int *k = c[x / 60];
                fb[y * W + x] = x == 0 || y == 0 || x == W - 1 || y == H - 1
                                    ? 0xffff
                                    : rgb(k[0], k[1], k[2]);
            }
        flush(0, 0, W, H, fb);
        printf("bars sent\n");
    } else if (!strcmp(argv[1], "button") && argc > 2) {
        /* sidecar's test request: press buttons (PSP_CTRL_* bits, hex) */
        r = libusb_control_transfer(h, 0x41, 0x7e, strtol(argv[2], NULL, 16), ifnum, NULL, 0, 2000);
        if (r < 0)
            die("button", r);
    } else if (!strcmp(argv[1], "bench") && argc > 2) {
        double t0;
        n = atoi(argv[2]);
        t0 = now();
        for (i = 0; i < n; i++) {
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++)
                    fb[y * W + x] = rgb(x + i * 4, y + i * 2, (x + y) / 2 + i * 8);
            flush(0, 0, W, H, fb);
        }
        t0 = now() - t0;
        printf("%d full frames in %.2f s: %.1f frames/s, %.1f MB/s\n", n, t0, n / t0,
               n * (W * H * 2.0) / t0 / 1e6);
    } else if (!strcmp(argv[1], "rects") && argc > 2) {
        double t0;
        n = atoi(argv[2]);
        t0 = now();
        for (i = 0; i < n; i++) {
            for (x = 0; x < 64 * 64; x++)
                fb[x] = rgb(i * 9, 255 - i * 5, i * 3);
            flush((i * 37) % (W - 64), (i * 23) % (H - 64), 64, 64, fb);
        }
        t0 = now() - t0;
        printf("%d rectangles of 64x64 in %.2f s: %.0f /s\n", n, t0, n / t0);
    } else {
        fprintf(stderr, "unknown test\n");
        return 1;
    }
    libusb_release_interface(h, ifnum);
    libusb_close(h);
    return 0;
}
