obj-m += i2c_bimawen.o
i2c_bimawen-y := hook.o ddc.o backlight.o retry.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules $(if $(C),C=$(C))

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

test-ddc: test-ddc.c ddc.c ddc.h
	$(CC) -Wall -Wextra -I. -DI2C_BIMAWEN_USERSPACE_TEST -o $@ test-ddc.c ddc.c

check: test-ddc
	./test-ddc