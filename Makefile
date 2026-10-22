# 3DS 중복 게임 정리기 - devkitPro(devkitARM + libctru + citro2d/3d) 필요
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
TARGET    := DupCleaner3DS

CC     := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ -std=gnu11 \
          -I$(DEVKITPRO)/libctru/include -I$(DEVKITPRO)/portlibs/3ds/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(DEVKITPRO)/libctru/lib -L$(DEVKITPRO)/portlibs/3ds/lib
LIBS   := -lcitro2d -lcitro3d -lctru -lm

all: $(TARGET).3dsx

$(TARGET).elf: source/main.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

$(TARGET).smdh: icon.png
	smdhtool --create "중복 게임 정리기" "Duplicate game cleaner" "Claude" icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh

clean:
	rm -f *.elf *.smdh *.3dsx
