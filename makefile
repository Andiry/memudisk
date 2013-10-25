
all:
	make -C $(LINUXKERNEL) SUBDIRS=`pwd` modules

running:
	make -C /usr/src/kernels/`uname -r` SUBDIRS=`pwd` modules
	
clean:
	rm -rf *.o *.mod.c memudisk.ko
