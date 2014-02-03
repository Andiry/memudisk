/*
 * Ram backed block device driver
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 */

/*
 *  Modified to support variable latencies through writes to procfs entry
 *  Adrian Caulfield <acaulfie@cs.ucsd.edu>
 *  Sept 2009
 *
 *  2/21/2010 - Adrian Caulfield <acaulfie@cs.ucsd.edu>
 *  Changes to support linux 2.6.30+ kernel changes
 *
 *
 *
 */



#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/radix-tree.h>
#include <linux/buffer_head.h> /* invalidate_bh_lrus() */
#include <linux/proc_fs.h>
#include <linux/delay.h>

#include <asm/uaccess.h>

#include "memudisk.h"


#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)

//static struct proc_dir_entry* proc_entry;
//static struct proc_dir_entry* proc_entry_stats;

/*
 * And now the modules code and kernel interface.
 */
static int rd_nr;
int enable_cache = 0;
int rd_size = CONFIG_BLK_DEV_RAM_SIZE;
static char* backing_dev_name = "/dev/ram0";
static int max_part;
static int part_shift;
int DeviceMajor = 243;
module_param(rd_nr, int, 0);
MODULE_PARM_DESC(rd_nr, "Maximum number of brd devices");
module_param(rd_size, int, 0);
MODULE_PARM_DESC(rd_size, "Size of each RAM disk in kbytes.");
module_param(max_part, int, 0);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per RAM disk");
module_param(enable_cache, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(enable_cache, "Enable cache for memudisk");
module_param(backing_dev_name, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(backing_dev_name, "Backing store device name");
MODULE_LICENSE("GPL");

/* Cache backing device */
struct block_device* backing_dev;

static int ReadDelay;
static int WriteDelay;

static unsigned long long ReadReqs;
static unsigned long long WriteReqs;
static unsigned long long ReadSects;
static unsigned long long WriteSects;
static unsigned long long DirectAccess;

ssize_t memudisk_proc_write(struct file* filp, const char __user *buff, unsigned long len, void* data) {
	sscanf(buff, "%d %d", &ReadDelay, &WriteDelay);

	printk("Memublock Timings Changed to Read: %d ns, Write: %d ns\n",ReadDelay,WriteDelay);

	return len;
}

ssize_t memustats_proc_read(char *page, char** start, off_t off, int count, int *eof, void*data) {
	int len=0;
	len+=sprintf(page, "%llu %llu %llu %llu %llu\n",ReadReqs,ReadSects,WriteReqs,WriteSects, DirectAccess);
	return len;
}

int memudisk_open(struct inode *inode, struct file *file) {
	int len=0;
	return len;
}

static const struct file_operations memudisk_write_fops = {
	.open	 = memudisk_open,
//	.write	 = memudisk_proc_write,
};

static const struct file_operations memudisk_read_fops = {
	.open	 = memudisk_open,
//	.read	 = memustats_proc_read,
};


/*
 * Look up and return a brd's page for a given sector.
 */
static struct page *brd_lookup_page(struct brd_device *brd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;

	/*
	 * The page lifetime is protected by the fact that we have opened the
	 * device node -- brd pages will never be deleted under us, so we
	 * don't need any further locking or refcounting.
	 *
	 * This is strictly true for the radix-tree nodes as well (ie. we
	 * don't actually need the rcu_read_lock()), however that is not a
	 * documented feature of the radix-tree API so it is better to be
	 * safe here (we don't have total exclusion from radix tree updates
	 * here, only deletes).
	 */
	rcu_read_lock();
	idx = sector >> PAGE_SECTORS_SHIFT; /* sector to page index */
	page = radix_tree_lookup(&brd->brd_pages, idx);
	rcu_read_unlock();

	BUG_ON(page && page->index != idx);

	return page;
}

/*
 * Look up and return a brd's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */
static struct page *brd_insert_page(struct brd_device *brd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;
	gfp_t gfp_flags;

	page = brd_lookup_page(brd, sector);
	if (page)
		return page;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support XIP and highmem, because our ->direct_access
	 * routine for XIP must return memory that is always addressable.
	 * If XIP was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */
	gfp_flags = GFP_NOIO | __GFP_ZERO;
#ifndef CONFIG_BLK_DEV_XIP
	gfp_flags |= __GFP_HIGHMEM;
#endif
	page = alloc_page(gfp_flags);
	if (!page)
		return NULL;

	if (radix_tree_preload(GFP_NOIO)) {
		__free_page(page);
		return NULL;
	}

	spin_lock(&brd->brd_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	if (radix_tree_insert(&brd->brd_pages, idx, page)) {
		__free_page(page);
		page = radix_tree_lookup(&brd->brd_pages, idx);
		BUG_ON(!page);
		BUG_ON(page->index != idx);
	} else
		page->index = idx;
	spin_unlock(&brd->brd_lock);

	radix_tree_preload_end();

	return page;
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void brd_free_pages(struct brd_device *brd)
{
	unsigned long pos = 0;
	struct page *pages[FREE_BATCH];
	int nr_pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(&brd->brd_pages,
				(void **)pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			void *ret;

			BUG_ON(pages[i]->index < pos);
			pos = pages[i]->index;
			ret = radix_tree_delete(&brd->brd_pages, pos);
			BUG_ON(!ret || ret != pages[i]);
			__free_page(pages[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_pages == FREE_BATCH);
}

/*
 * copy_to_brd_setup must be called before copy_to_brd. It may sleep.
 */
static int copy_to_brd_setup(struct brd_device *brd, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!brd_insert_page(brd, sector))
		return -ENOMEM;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!brd_insert_page(brd, sector))
			return -ENOMEM;
	}
	return 0;
}

/*
 * Copy n bytes from src to the brd starting at sector. Does not sleep.
 */
static void copy_to_brd(struct brd_device *brd, const void *src,
			sector_t sector, size_t n)
{
	struct page *page;
	void *dst;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

//	printk(KERN_INFO "%s: size %zu\n", __func__, n);
	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = brd_lookup_page(brd, sector);
	BUG_ON(!page);

	dst = kmap_atomic(page);
	memcpy(dst + offset, src, copy);
	//we want to wait WriteDelay ns for every 32 bytes - this matches the bee3 hardware delays
	ndelay(WriteDelay * (copy/64));
	kunmap_atomic(dst);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = brd_lookup_page(brd, sector);
		BUG_ON(!page);

		dst = kmap_atomic(page);
		memcpy(dst, src, copy);
		ndelay(WriteDelay * (copy/64));
		kunmap_atomic(dst);
	}
}

/*
 * Copy n bytes to dst from the brd starting at sector. Does not sleep.
 */
static void copy_from_brd(void *dst, struct brd_device *brd,
			sector_t sector, size_t n)
{
	struct page *page;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = brd_lookup_page(brd, sector);
	if (page) {
		src = kmap_atomic(page);
		memcpy(dst, src + offset, copy);
		ndelay(ReadDelay * (copy/64));
		kunmap_atomic(src);
	} else {
		memset(dst, 0, copy);
		ndelay(ReadDelay * (copy/64));
	}

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = brd_lookup_page(brd, sector);
		if (page) {
			src = kmap_atomic(page);
			memcpy(dst, src, copy);
			ndelay(ReadDelay * (copy/64));
			kunmap_atomic(src);
		} else {
			memset(dst, 0, copy);
			ndelay(ReadDelay * (copy/64));
		}

	}
}

/*
 * Process a single bvec of a bio.
 */
static int brd_do_bvec(struct brd_device *brd, struct page *page,
			unsigned int len, unsigned int off, int rw,
			sector_t sector)
{
	void *mem;
	int err = 0;

	if (rw != READ) {
		err = copy_to_brd_setup(brd, sector, len);
		if (err)
			goto out;
	}

	mem = kmap_atomic(page);
	if (rw == READ) {
		copy_from_brd(mem + off, brd, sector, len);
		flush_dcache_page(page);
	} else
		copy_to_brd(brd, mem + off, sector, len);
	kunmap_atomic(mem);

out:
	return err;
}

static void brd_make_request(struct request_queue *q, struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	struct brd_device *brd = bdev->bd_disk->private_data;
	int rw;
	struct bio_vec *bvec;
	sector_t sector;
	int i;
	int err = -EIO;
	int submitted_to_cache = 0;

	sector = bio->bi_sector;
	if (sector + (bio->bi_size >> SECTOR_SHIFT) >
						get_capacity(bdev->bd_disk))
		goto out;

	rw = bio_rw(bio);
	if (rw == READA)
		rw = READ;
	
	//update statistics
	if (rw == READ) {
		ReadReqs++;
		ReadSects+=bio->bi_size >> SECTOR_SHIFT;
	} else {
		WriteReqs++;
		WriteSects+=bio->bi_size >> SECTOR_SHIFT;
	}

	bio_for_each_segment(bvec, bio, i) {
		unsigned int len = bvec->bv_len;
		err = brd_do_bvec(brd, bvec->bv_page, len,
					bvec->bv_offset, rw, sector);
		if (err)
			break;
		sector += len >> SECTOR_SHIFT;
	}

	if (enable_cache && rw != READ)
		submitted_to_cache = submit_bio_to_cache(brd, bio);
out:
	/* If bio submitted to cache, bio_endio will be called in clone bio */
	if (!submitted_to_cache)
		bio_endio(bio, err);

	return;
}

#ifdef CONFIG_BLK_DEV_XIP
static int brd_direct_access (struct block_device *bdev, sector_t sector,
			void **kaddr, unsigned long *pfn)
{
	struct brd_device *brd = bdev->bd_disk->private_data;
	struct page *page;

	DirectAccess++;
	if (!brd)
		return -ENODEV;
	if (sector & (PAGE_SECTORS-1))
		return -EINVAL;
	if (sector + PAGE_SECTORS > get_capacity(bdev->bd_disk))
		return -ERANGE;
	page = brd_insert_page(brd, sector);
	if (!page)
		return -ENOMEM;
	*kaddr = page_address(page);
	*pfn = page_to_pfn(page);

	return 0;
}
#endif

static int brd_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	int error;
	struct brd_device *brd = bdev->bd_disk->private_data;

	if (cmd != BLKFLSBUF)
		return -ENOTTY;

	/*
	 * ram device BLKFLSBUF has special semantics, we want to actually
	 * release and destroy the ramdisk data.
	 */
	mutex_lock(&bdev->bd_mutex);
	error = -EBUSY;
	if (bdev->bd_openers <= 1) {
		/*
		 * Invalidate the cache first, so it isn't written
		 * back to the device.
		 *
		 * Another thread might instantiate more buffercache here,
		 * but there is not much we can do to close that race.
		 */
		invalidate_bh_lrus();
		truncate_inode_pages(bdev->bd_inode->i_mapping, 0);
		brd_free_pages(brd);
		error = 0;
	}
	mutex_unlock(&bdev->bd_mutex);

	return error;
}

static struct block_device_operations brd_fops = {
	.owner =		THIS_MODULE,
	.ioctl =		brd_ioctl,
#ifdef CONFIG_BLK_DEV_XIP
	.direct_access =	brd_direct_access,
#endif
};

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(brd_devices);
static DEFINE_MUTEX(brd_devices_mutex);

static struct brd_device *brd_alloc(int i)
{
	struct brd_device *brd;
	struct gendisk *disk;
	int ret;

	brd = kzalloc(sizeof(*brd), GFP_KERNEL);
	if (!brd)
		goto out;

	ReadReqs = 0;
	WriteReqs = 0;
	ReadSects = 0;
	WriteSects = 0;
	DirectAccess = 0;

	brd->brd_number		= i;
	spin_lock_init(&brd->brd_lock);
	INIT_RADIX_TREE(&brd->brd_pages, GFP_ATOMIC);

	brd->brd_queue = blk_alloc_queue(GFP_KERNEL);
	if (!brd->brd_queue)
		goto out_free_dev;
	blk_queue_make_request(brd->brd_queue, brd_make_request);
	blk_queue_max_hw_sectors(brd->brd_queue, 1024);
	blk_queue_bounce_limit(brd->brd_queue, BLK_BOUNCE_ANY);

	disk = brd->brd_disk = alloc_disk(1 << part_shift);
	if (!disk)
		goto out_free_queue;
	disk->major		= DeviceMajor;
	disk->first_minor	= i << part_shift;
	disk->fops		= &brd_fops;
	disk->private_data	= brd;
	disk->queue		= brd->brd_queue;
	//disk->flags |= GENHD_FL_SUPPRESS_PARTITION_INFO;
	sprintf(disk->disk_name, "bankshot%d", i);
	set_capacity(disk, rd_size * 2);

/*
	// touch every page, rather than waiting for them to be needed
	sector_t j;
	for(j=0; j<rd_size/4; j++)
	{
		brd_insert_page(brd, j*8);
	}
*/

	ret = brd_char_setup(brd);
	if (ret)
		goto out_free_queue; 

	return brd;

out_free_queue:
	blk_cleanup_queue(brd->brd_queue);
out_free_dev:
	kfree(brd);
out:
	return NULL;
}

static void brd_free(struct brd_device *brd)
{
	put_disk(brd->brd_disk);
	blk_cleanup_queue(brd->brd_queue);
	brd_free_pages(brd);
	if (enable_cache)
		brd_cache_exit(brd);
	brd_char_destroy(brd);
	kfree(brd);
}

static struct brd_device *brd_init_one(int i)
{
	struct brd_device *brd;

	list_for_each_entry(brd, &brd_devices, brd_list) {
		if (brd->brd_number == i)
			goto out;
	}

	brd = brd_alloc(i);
	if (brd) {
		add_disk(brd->brd_disk);
		list_add_tail(&brd->brd_list, &brd_devices);
		if (enable_cache)
			brd_cache_init(brd, backing_dev);
	}
out:
	return brd;
}

static void brd_del_one(struct brd_device *brd)
{
	list_del(&brd->brd_list);
	del_gendisk(brd->brd_disk);
	brd_free(brd);
}

static struct kobject *brd_probe(dev_t dev, int *part, void *data)
{
	struct brd_device *brd;
	struct kobject *kobj;

	mutex_lock(&brd_devices_mutex);
	brd = brd_init_one(dev & MINORMASK);
	kobj = brd ? get_disk(brd->brd_disk) : ERR_PTR(-ENOMEM);
	mutex_unlock(&brd_devices_mutex);

	*part = 0;
	return kobj;
}

static int __init brd_init(void)
{
	int i, nr;
	int ret;
	unsigned long range;
	struct brd_device *brd, *next;

	/*
	 * brd module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 * However, this will not work well with user space tool that doesn't
	 * know about such "feature".  In order to not break any existing
	 * tool, we do the following:
	 *
	 * (1) if rd_nr is specified, create that many upfront, and this
	 *     also becomes a hard limit.
	 * (2) if rd_nr is not specified, create 1 rd device on module
	 *     load, user can further extend brd device by create dev node
	 *     themselves and have kernel automatically instantiate actual
	 *     device on-demand.
	 */

	part_shift = 0;
	if (max_part > 0)
		part_shift = fls(max_part);

	if (rd_nr > 1UL << (MINORBITS - part_shift))
		return -EINVAL;

	if (rd_nr) {
		nr = rd_nr;
		range = rd_nr;
	} else {
		nr = CONFIG_BLK_DEV_RAM_COUNT;
		range = 1UL << (MINORBITS - part_shift);
	}

	if (register_blkdev(DeviceMajor, "memuramdisk"))
		return -EIO;

#if 0
	if (enable_cache) {
		ret = brd_cache_open_backing_dev(&backing_dev, backing_dev_name);
		if (ret)
			brd_info("brd cache initialization failed\n");
	}
#endif

	brd_char_init();

	for (i = 0; i < nr; i++) {
		brd = brd_alloc(i);
		if (!brd)
			goto out_free;
		list_add_tail(&brd->brd_list, &brd_devices);
		if (enable_cache) {
			if (i == 0) {
				ret = brd_cache_open_backing_dev(&backing_dev,
							backing_dev_name, brd);
				if (ret) {
					brd_info("brd cache initialization "
						"failed, disable cache\n");
					enable_cache = 0;
				}
			}
			brd_cache_init(brd, backing_dev);
		}
	}

	/* point of no return */

	list_for_each_entry(brd, &brd_devices, brd_list)
		add_disk(brd->brd_disk);

	blk_register_region(MKDEV(DeviceMajor, 0), range,
				  THIS_MODULE, brd_probe, NULL, NULL);

	printk(KERN_INFO "bankshot2: module loaded\n");

#if 0
	//create proc entry
//	proc_entry = create_proc_entry("memudisk", S_IFREG, NULL);
	proc_entry = proc_create_data("memudisk", S_IFREG, NULL, &memudisk_write_fops, NULL);
	if (proc_entry == NULL) {
		printk("Unable to create proc entry for memudisk\n");
	} else {
//		proc_entry->write_proc = memudisk_proc_write;

		//acaulfie: kernels >= 2.6.30 don't have an owner field
		//proc_entry->owner = THIS_MODULE;
	}

//	proc_entry_stats = create_proc_entry("memustats", S_IFREG, NULL);
	proc_entry_stats = proc_create_data("memustats", S_IFREG, NULL, &memudisk_read_fops, NULL);
	if (proc_entry_stats == NULL) {
		printk("Unable to create proc entry for memustats\n");
	} else {
//		proc_entry_stats->read_proc = memustats_proc_read;

	}
#endif

	return 0;

out_free:
	list_for_each_entry_safe(brd, next, &brd_devices, brd_list) {
		list_del(&brd->brd_list);
		brd_free(brd);
	}
	unregister_blkdev(DeviceMajor, "memuramdisk");

	return -ENOMEM;
}

static void __exit brd_exit(void)
{
	unsigned long range;
	struct brd_device *brd, *next;

	range = rd_nr ? rd_nr :  1UL << (MINORBITS - part_shift);

	list_for_each_entry_safe(brd, next, &brd_devices, brd_list)
		brd_del_one(brd);

	if (enable_cache)
		blkdev_put(backing_dev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);

	brd_char_exit();
	blk_unregister_region(MKDEV(DeviceMajor, 0), range);
	unregister_blkdev(DeviceMajor, "memuramdisk");

	//clean up proc entry
//	remove_proc_entry("memudisk", NULL);
//	remove_proc_entry("memustats", NULL);
}

module_init(brd_init);
module_exit(brd_exit);

