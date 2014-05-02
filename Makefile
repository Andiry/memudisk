obj-m += memudisk.o

all:
	make -C /media/root/e755aa6d-e75c-46b1-a871-d4c7a5587972/usr/src/Linux-pmfs  SUBDIRS=`pwd` modules

running:
	make -C /media/root/5afd72e1-c6c2-41f0-9606-8cd62259c55b/Linux-pmfs SUBDIRS=`pwd` modules
	
clean:
	rm -rf *.o *.mod.c memudisk.ko
