CC ?= gcc
CFLAGS := -g -mcmodel=small -isystem include -nostdinc -nostdlib -fno-stack-protector -ffreestanding -fPIE -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -static -nostdlib -Wl,-gc-sections

all: payload.elf

clean:
	rm -f payload.elf payload.bin

payload.elf: lib/lib.a main.c info.c struct.h ps4-libjbc/jailbreak.c ps4-libjbc/kernelrw.c ps4-libjbc/utils.c
	$(CC) $(CFLAGS) $(LDFLAGS) lib/lib.a main.c info.c ps4-libjbc/jailbreak.c ps4-libjbc/kernelrw.c ps4-libjbc/utils.c -o $@
