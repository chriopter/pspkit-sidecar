/* PSP as a USB display, started from the XMB: loads sidecar.prx from this
 * folder with the "gudid" option, so the console alone on the cable shows
 * up as 1d50:614d and Linux's gud driver binds to it. HOME leaves.
 * Needs custom firmware (a kernel PRX is loaded through kubridge).
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <kubridge.h>
#include <string.h>
#include <stdio.h>

PSP_MODULE_INFO("SidecarApp", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(256);

static int exit_cb(int a, int b, void *c)
{
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize args, void *argp)
{
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_cb, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

int main(int argc, char *argv[])
{
    static const char opts[] = "sidecar\0gudid";
    char path[256];
    const char *slash = strrchr(argv[0], '/');
    int dir = slash ? (int)(slash - argv[0]) + 1 : 0, status = 0;
    SceUID mod, th;

    th = sceKernelCreateThread("cb", cb_thread, 0x11, 0x1000, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);

    pspDebugScreenInit();
    pspDebugScreenPrintf("Sidecar: PSP as a USB display\n\n");
    snprintf(path, sizeof path, "%.*ssidecar.prx", dir, argv[0]);
    mod = kuKernelLoadModule(path, 0, NULL);
    if (mod < 0) {
        pspDebugScreenPrintf("could not load %s: %08x\n", path, (unsigned)mod);
    } else {
        int r = sceKernelStartModule(mod, sizeof opts, (void *)opts, &status, NULL);
        if (r < 0)
            pspDebugScreenPrintf("could not start the driver: %08x\n", (unsigned)r);
        else
            pspDebugScreenPrintf("Connect the USB cable. The picture appears\n"
                                 "when the computer turns the display on.\n\nHOME leaves.\n");
    }
    sceKernelSleepThread();
    return 0;
}
