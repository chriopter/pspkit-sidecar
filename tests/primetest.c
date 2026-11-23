/* primetest: what the patched aquamarine asks of the kernel for a gud
 * display, without a compositor. Allocates a linear XRGB8888 buffer on the
 * render GPU with GBM, imports it into the gud DRM device (PRIME) and makes
 * a framebuffer of it. If this process is DRM master on the gud device
 * (no compositor holds it) it also sets the mode and flushes N frames.
 *
 *   primetest [N]     N frames if master, default 60
 *
 * Exits 0 if import and framebuffer creation worked, 1 otherwise; prints
 * whether the modeset part ran.
 *
 *   cc -O2 -o primetest tests/primetest.c $(pkg-config --cflags --libs libdrm gbm)
 */
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define W 480
#define H 272

static void paint(struct gbm_bo *bo, int shift)
{
    uint32_t stride;
    void *map_data = NULL;
    uint32_t *px = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                      0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
    if (!px) {
        perror("gbm_bo_map");
        exit(1);
    }
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)((char *)px + y * stride);
        for (int x = 0; x < W; x++)
            row[x] = bars[((x + shift) % W) * 8 / W];
    }
    gbm_bo_unmap(bo, map_data);
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 60, card = -1, render;
    char path[32], gudpath[32] = "";

    for (int i = 0; i < 16 && card < 0; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmVersion *v = drmGetVersion(fd);
        if (v && !strcmp(v->name, "gud")) {
            card = fd;
            strcpy(gudpath, path);
        } else
            close(fd);
        if (v)
            drmFreeVersion(v);
    }
    if (card < 0) {
        fprintf(stderr, "no gud DRM device that this user can open\n");
        return 1;
    }
    printf("gud device %s\n", gudpath);

    render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = render >= 0 ? gbm_create_device(render) : NULL;
    if (!gbm) {
        fprintf(stderr, "no GBM device on renderD128\n");
        return 1;
    }
    printf("render GPU via %s\n", gbm_device_get_backend_name(gbm));

    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, W, H, GBM_FORMAT_XRGB8888, &linear, 1,
                                                      GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!bo) {
        printf("allocation with scanout flag failed, trying rendering+linear\n");
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!bo) {
        fprintf(stderr, "no linear XRGB8888 buffer from the render GPU\n");
        return 1;
    }
    printf("buffer: modifier 0x%llx stride %u\n", (unsigned long long)gbm_bo_get_modifier(bo),
           gbm_bo_get_stride(bo));
    paint(bo, 0);

    int dmabuf = gbm_bo_get_fd(bo);
    uint32_t handle = 0, fb = 0;
    if (drmPrimeFDToHandle(card, dmabuf, &handle)) {
        perror("PRIME import into gud");
        return 1;
    }
    printf("PRIME import into gud: ok, handle %u\n", handle);

    uint32_t handles[4] = { handle }, pitches[4] = { gbm_bo_get_stride(bo) }, offsets[4] = { 0 };
    uint64_t mods[4] = { DRM_FORMAT_MOD_LINEAR };
    if (drmModeAddFB2WithModifiers(card, W, H, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                                   mods, &fb, DRM_MODE_FB_MODIFIERS)) {
        perror("framebuffer with modifier");
        if (drmModeAddFB2(card, W, H, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0)) {
            perror("framebuffer");
            return 1;
        }
    }
    printf("framebuffer on gud: ok, id %u\n", fb);

    if (!drmIsMaster(card)) {
        printf("not DRM master on %s (a compositor holds it): modeset and flush not tested\n", gudpath);
        return 0;
    }

    drmModeRes *res = drmModeGetResources(card);
    drmModeConnector *conn = NULL;
    for (int i = 0; res && i < res->count_connectors && !conn; i++) {
        conn = drmModeGetConnector(card, res->connectors[i]);
        if (conn && (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes)) {
            drmModeFreeConnector(conn);
            conn = NULL;
        }
    }
    if (!conn || !res->count_crtcs) {
        fprintf(stderr, "no connected connector\n");
        return 1;
    }
    if (drmModeSetCrtc(card, res->crtcs[0], fb, 0, 0, &conn->connector_id, 1, &conn->modes[0])) {
        perror("modeset");
        return 1;
    }
    printf("modeset %dx%d: ok\n", conn->modes[0].hdisplay, conn->modes[0].vdisplay);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 1; i <= frames; i++) {
        paint(bo, i * 4);
        if (drmModeDirtyFB(card, fb, NULL, 0)) {
            perror("flush");
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d full-frame flushes queued in %.2f s\n", frames, s);
    sleep(2);
    return 0;
}
