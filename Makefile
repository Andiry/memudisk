obj-m += memudisk.o

all:
	make -C /usr/src/linux SUBDIRS=`pwd` modules

running:
	make -C /usr/src/Linux-pmfs SUBDIRS=`pwd` modules
	
clean:
	rm -rf *.o *.mod.c memudisk.ko
