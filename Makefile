obj-m += memudisk.o

all:
	make -C /media/root/e755aa6d-e75c-46b1-a871-d4c7a5587972/usr/src/linux M=`pwd`

running:
	make -C /usr/src/Linux-pmfs M=`pwd`
	
clean:
	rm -rf *.o *.mod.c memudisk.ko
