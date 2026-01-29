# Adjust these if needed
CROSS_COMPILE ?= arm-poky-linux-gnueabi-
CC := $(CROSS_COMPILE)gcc
SYSROOT ?= /opt/fsl-imx-fb/6.6-scarthgap/sysroots/cortexa7t2hf-neon-poky-linux-gnueabi

# Architecture flags: ensure hard-float ABI so correct glibc stubs are used
ARCH_FLAGS := -march=armv7-a -mfloat-abi=hard -mfpu=neon-vfpv4
ARCH_DEFS  := -D__ARM_PCS_VFP

# Explicit include and library paths (avoid host pkg-config)
INCLUDE_DIRS := $(SYSROOT)/usr/include \
               $(SYSROOT)/usr/include/paho-mqtt3c \
LIB_DIRS     := $(SYSROOT)/lib $(SYSROOT)/usr/lib

CFLAGS := -Wall -Wextra -O2 \
          --sysroot=$(SYSROOT) \
          $(ARCH_FLAGS) $(ARCH_DEFS) \
          $(foreach dir,$(INCLUDE_DIRS),-I$(dir))

LDFLAGS := --sysroot=$(SYSROOT) \
           $(foreach dir,$(LIB_DIRS),-L$(dir)) \
           -Wl,-rpath-link,$(SYSROOT)/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib \
           -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 \
           -lpaho-mqtt3c

INCLUDES := -Isrc

TARGET := ibutton-tester

SRCS := src/ibutton_tester.c
OBJS := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
