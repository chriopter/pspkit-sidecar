TARGET = sidecar
OBJS = sidecar.o exports.o

USE_KERNEL_LIBS = 1
# As PSPLink builds its modules: the CFW resolves the SDK's kernel NIDs
# for a module that declares an old firmware version.
PSP_FW_VERSION = 271

CFLAGS = -O2 -G0 -Wall -fno-builtin-printf
LIBS = -lpspusb_driver -lpspusbbus_driver -lpsppower_driver -lpspdisplay_driver -lpspctrl_driver -lpspdebug

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
