MODULE_NAME := throttle_module
obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-y := module.o dev.o ioctl.o hook.o throttle.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y += -I$(PWD)/header

.PHONY: all module client test clean generate_headers ins rem

all: generate_headers module client test ins

generate_headers:
	@./generate_mapping.sh

module: generate_headers
	$(MAKE) -C $(KDIR) M=$(PWD) modules

client: client.c
	$(CC) -Wall -Wextra -o client client.c

test: test_throttle.c
	$(CC) -Wall -Wextra -pthread -o test_throttle test_throttle.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f client test_throttle header/syscall_mapping.h

ins:
	@sudo insmod $(MODULE_NAME).ko

rem:
	@sudo ./safe_rmmod.sh
