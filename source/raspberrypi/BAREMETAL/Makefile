CC = arm-none-eabi-gcc
LD = arm-none-eabi-ld
OBJCOPY = arm-none-eabi-objcopy

CFLAGS = -DNDEBUG -Wall -O3 -mfpu=neon -mfloat-abi=hard -march=armv7ve -mtune=cortex-a7 -mno-unaligned-access -fno-tree-loop-vectorize -fno-tree-slp-vectorize
CFLAGS += -nostdlib -nostartfiles -ffreestanding -fno-asynchronous-unwind-tables -fomit-frame-pointer
CFLAGS += -DBAREMETAL
CCFLAGS = $(CFLAGS)
CCFLAGS += -fno-exceptions -fno-rtti

ifdef CONNECT_TYPE
CFLAGS += -DCONNECT_TYPE_$(CONNECT_TYPE)
CCFLAGS += -DCONNECT_TYPE_$(CONNECT_TYPE)
endif

LIB = -L "C:\Program Files (x86)\GNU Tools Arm Embedded\9 2019-q4-major\arm-none-eabi\lib\arm\v5te\hard"
LIB += -L "C:\Program Files (x86)\GNU Tools Arm Embedded\9 2019-q4-major\lib\gcc\arm-none-eabi\9.2.1\arm\v5te\hard"

OBJS = main.o SmartStart32.o rpi-SmartStart.o emb-stdio.o SDEmmc.o windows.o fast_util.o
OBJS +=  cpputil.o ffdiskio.o ffsystem.o ffunicode.o ff.o
OBJS +=  gpiobus.o fileio.o filepath.o disk.o cfilesystem.o rascsi.o

all: kernel7.img

kernel7.elf: $(BOOT) $(OBJS)
	$(LD) -gc-sections --build-id=none -Bdynamic $^ -T rpi32.ld -o $@ $(LIB) -lc -lm -lg -lgcc

.SUFFIXES: .elf .img

.elf.img:
	$(OBJCOPY) -O binary $< $@
.c.o:
	$(CC) $(CFLAGS) -c $< -o $@
.cpp.o:
	$(CC) $(CCFLAGS) -c $< -o $@
.S.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	del *.o kernel7.img *.elf
