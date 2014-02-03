#include "memudisk.h"

static struct class *brd_chardev_class;

int brd_char_open(struct inode *inode, struct file *filp)
{
	struct brd_device *brd = container_of(inode->i_cdev, struct brd_device,
						chardev);

	filp->private_data = brd;

	return 0;
}

int brd_char_release(struct inode *inode, struct file *filp)
{
	brd_info("release brd char class\n");
	return 0;
}

long brd_char_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct brd_device *brd = filp->private_data;

	switch (cmd) {
	case BRD_CHAR_IOCTL_CACHE_DATA:
		brd_info("ioctl sends to device %d, cmd 0x%x, arg %d\n", brd->brd_number, cmd, *(int *)arg);
		break;
	default:
		break;
	}

	return 0;
}

const struct file_operations brd_char_fops = {
	.open = brd_char_open,
	.release = brd_char_release,
	.unlocked_ioctl = brd_char_ioctl,
};

int brd_char_init(void)
{
	brd_chardev_class = class_create(THIS_MODULE, "bankshotCtrl");
	brd_info("create char class\n");
	return 0;
}	

void brd_char_exit(void)
{
	class_destroy(brd_chardev_class);
	brd_info("destroy char class\n");
}	

int brd_char_setup(struct brd_device *brd)
{
	if (alloc_chrdev_region(&brd->chardevnum, brd->brd_number, 1, "bankshotCtrl"))
		goto err_alloc_chrdev;

	cdev_init(&brd->chardev, &brd_char_fops);
	brd->chardev.owner = THIS_MODULE;

	cdev_add(&brd->chardev, brd->chardevnum, 1);

	device_create(brd_chardev_class, NULL, brd->chardevnum, NULL,
			"bankshotCtrl%d", MINOR(brd->chardevnum));

	brd_info("Add char device %d, %d as bankshotCtrl%d\n",
		 MAJOR(brd->chardevnum), MINOR(brd->chardevnum), 
		 MINOR(brd->chardevnum));

	return 0;

err_alloc_chrdev:
	brd_info("Failed to register char device\n");
	return -EINVAL;
}

void brd_char_destroy(struct brd_device* brd)
{
	device_destroy(brd_chardev_class, brd->chardevnum);
	unregister_chrdev_region(brd->chardevnum, 1);
	cdev_del(&brd->chardev);
}

