#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/radix-tree.h>
#include <linux/buffer_head.h> /* invalidate_bh_lrus() */
#include <linux/proc_fs.h>
#include <linux/delay.h>

#include <asm/uaccess.h>

#define brd_dbg(s, args ...)	pr_debug(s, ## args)
#define brd_info(s, args ...)	pr_info(s, ## args)

/* cache.c */
struct brd_cache_info {
	struct block_device *bs_bdev;
	struct request_queue *backing_store_rqueue;
};

/*
 * Each block ramdisk device has a radix_tree brd_pages of pages that stores
 * the pages containing the block device's contents. A brd page's ->index is
 * its offset in PAGE_SIZE units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */
struct brd_device {
	int		brd_number;
	int		brd_refcnt;
	loff_t		brd_offset;
	loff_t		brd_sizelimit;
	unsigned	brd_blocksize;

	struct request_queue	*brd_queue;
	struct gendisk		*brd_disk;
	struct list_head	brd_list;

	struct cdev chardev;
	dev_t chardevnum;

	/*
	 * Backing store of pages and lock to protect it. This is the contents
	 * of the block device.
	 */
	spinlock_t		brd_lock;
	struct radix_tree_root	brd_pages;
	struct brd_cache_info *cache_info;
};


int submit_bio_to_cache(struct brd_device *brd, struct bio *bio);
int brd_cache_open_backing_dev(struct block_device **bdev,
					char* backing_dev_name,
					struct brd_device* brd);
int brd_cache_init(struct brd_device *brd, struct block_device* bdev);
void brd_cache_exit(struct brd_device *brd);

/* char.c */
int brd_char_init(void);
void brd_char_exit(void);
int brd_char_setup(struct brd_device *brd);
void brd_char_destroy(struct brd_device *brd);

/* ioctls */
#define BRD_CHAR_IOCTL_CACHE_DATA	0xBCD00000
