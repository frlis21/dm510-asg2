# Change this if you keep your files elsewhere
ROOT := ../uml
SHARED_DIR = $(ROOT)/shared
KERNEL_DIR = $(ROOT)/linux-6.1.9
PWD = $(shell pwd)
BINS := moduletest ioctl
STUFF = dm510_dev.ko $(BINS) dm510_load dm510_unload
SHARED_STUFF = $(addprefix $(SHARED_DIR)/,$(STUFF))

obj-m += dm510_dev.o

all: modules $(BINS) $(SHARED_STUFF)

modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) LDDINC=$(KERNEL_DIR)/include ARCH=um modules

$(SHARED_STUFF): $(SHARED_DIR)/%: %
	cp -rf $^ $(SHARED_DIR)

clean:
	rm -rf *.o *~ $(BINS) core .depend .*.cmd *.ko *.mod.c .tmp_versions $(SHARED_STUFF)
