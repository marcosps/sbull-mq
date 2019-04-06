ifdef SQ
	obj-m := sbull.o
else
	obj-m := sbull-mq.o
endif

KDIR ?= /lib/modules/$(shell uname -r)/build

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
