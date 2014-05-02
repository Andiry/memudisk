obj-m += memudisk.o

all:
	make -C /usr/src/linux SUBDIRS=`pwd` modules

running:
	make -C /media/root/5afd72e1-c6c2-41f0-9606-8cd62259c55b/Linux-pmfs SUBDIRS=`pwd` modules
	
clean:
	rm -rf *.o *.mod.c memudisk.ko
