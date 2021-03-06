#include <linux/time.h>
#include <linux/fs.h>
#include <linux/blkdev.h>

#include "memudisk.h"

void brd_bio_endio(struct bio *bio, int error)
{
	struct bio *bio_org = (struct bio *)(bio->bi_private);

	bio_endio(bio_org, 0);
	bio_put(bio);
}

int submit_bio_to_cache(struct brd_device *brd, struct bio *bio)
{
	struct brd_cache_info *cinfo = brd->cache_info;
	struct bio *bdev_bio;
	int rw = bio_rw(bio);

	if (cinfo) {
		bdev_bio = bio_clone(bio, GFP_KERNEL);
		bdev_bio->bi_bdev = cinfo->bs_bdev;
		bdev_bio->bi_private = bio;
		bdev_bio->bi_end_io = brd_bio_endio;
//		brd_info("submit bio %p to cache %p\n", bdev_bio, cinfo->bs_bdev);
		submit_bio(rw, bdev_bio);

		return 1;
	}

	return 0;
}

static int brd_cache_assign_backing_dev(struct brd_cache_info *cinfo,
					struct block_device *bdev)
{
	int ret;

	brd_info("Opened handle to the block device %p\n", bdev);

	if (bdev->bd_disk){
		cinfo->backing_store_rqueue = bdev_get_queue(bdev);
		brd_info("Backing store %p request queue is %p\n",
				bdev, cinfo->backing_store_rqueue);
		if (cinfo->backing_store_rqueue) {
			brd_info("max_request_in_queue %lu, "
				"max_sectors %d, "
				"physical_block_size %d, "
				"io_min %d, io_op %d, "
				"make_request_fn %p\n",
			cinfo->backing_store_rqueue->nr_requests,
			cinfo->backing_store_rqueue->limits.max_sectors,
			cinfo->backing_store_rqueue->limits.physical_block_size,
		 	cinfo->backing_store_rqueue->limits.io_min,
			cinfo->backing_store_rqueue->limits.io_opt,
			cinfo->backing_store_rqueue->make_request_fn
			);
			brd_info("Backing store number %d\n",
				bdev->bd_dev);

			cinfo->bs_bdev = bdev;

			return 0;

		} else
			brd_info("Backing store request queue "
					"is null pointer\n");
	} else
		brd_info("Backing store bdisk is null\n");

	return ret;
}
	

int brd_cache_open_backing_dev(struct block_device **bdev,
					char* backing_dev_name,
					struct brd_device* brd)
{
	dev_t dev;
	int ret;

	brd_info("Init brd cache, backing device %s\n", backing_dev_name);
	*bdev = lookup_bdev(backing_dev_name);
	if (IS_ERR(*bdev)) {
		brd_info("Backing device not found\n");
		ret = -EINVAL;
		goto fail;
	}

	dev = (*bdev)->bd_dev;
	if (!(*bdev)->bd_inode) {
		brd_info("Backing device inode is NULL\n");
		ret = -EINVAL;
		goto fail;
	}

	if (dev) {
		*bdev = blkdev_get_by_dev(dev, FMODE_READ |
					FMODE_WRITE | FMODE_EXCL, brd);
		if(IS_ERR(*bdev)) {
			return -EINVAL;
			goto fail;
		}

		return 0;
	} else
		brd_info("Backing store bdisk is null\n");

	ret = -EINVAL;

fail:
	return ret;
}

int brd_cache_init(struct brd_device *brd, struct block_device* bdev)
{
	struct brd_cache_info *cinfo;
	int ret;

	cinfo = kzalloc(sizeof(struct brd_cache_info), GFP_KERNEL);

	if (!cinfo) {
		brd_info("Cache info allocation failed\n");
		return -ENOMEM;
	}

	ret = brd_cache_assign_backing_dev(cinfo, bdev);

	if (ret != 0) {
		brd_info("Init brd cache failed %d\n", ret);
		return ret;
	}

	brd_info("cache enabled\n");
	brd->cache_info = cinfo;

	return 0;
}

void brd_cache_exit(struct brd_device *brd)
{
	brd_info("exiting cache\n");

	kfree(brd->cache_info);
}
