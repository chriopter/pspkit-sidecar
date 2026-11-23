TARGET = sidecar_app
OBJS = main.o

BUILD_PRX = 1
CFLAGS = -O2 -G0 -Wall
LIBS = -lpspkubridge

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Sidecar

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
