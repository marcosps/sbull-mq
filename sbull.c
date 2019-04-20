/*
 * Sample disk driver, from the beginning.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>

MODULE_LICENSE("Dual BSD/GPL");

static int sbull_major;
module_param(sbull_major, int, 0);
static int logical_block_size = 512;
module_param(logical_block_size, int, 0);
static char *disk_size = "256M";
module_param(disk_size, charp, 0);
static int ndevices = 1;
module_param(ndevices, int, 0);
static bool debug = false;
module_param(debug, bool, false);

/*
 * The different "request modes" we can use.
 */
enum {
	RM_SIMPLE  = 0,	/* The extra-simple request function */
	RM_FULL    = 1,	/* The full-blown version */
	RM_NOQUEUE = 2,	/* Use make_request */
};
static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);

/*
 * Minor number and partition management.
 */
#define SBULL_MINORS	16

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE	512

/*
 * After this much idle time, the driver will simulate a media change.
 */
#define INVALIDATE_DELAY	(30 * HZ)

/*
 * The internal representation of our device.
 */
struct sbull_dev {
	int size;                       /* Device size in sectors */
	u8 *data;                       /* The data array */
	short users;                    /* How many users */
	short media_change;             /* Flag a media change? */
	spinlock_t lock;                /* For mutual exclusion */
	struct request_queue *queue;    /* The device request queue */
	struct gendisk *gd;             /* The gendisk structure */
};


static struct sbull_dev *Devices;

/* Handle an I/O request */
static void sbull_transfer(struct sbull_dev *dev, unsigned long sector,
			unsigned long nsect, char *buffer, int op)

{
	unsigned long offset = sector*KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect*KERNEL_SECTOR_SIZE;

	if ((offset + nbytes) > dev->size) {
		pr_notice("Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}

	if  (debug)
		pr_info("%s: %s, sector: %ld, nsectors: %ld, offset: %ld,"
			       "nbytes: %ld",
			dev->gd->disk_name,
			op == REQ_OP_WRITE ? "WRITE" : "READ", sector, nsect,
			offset, nbytes);

	/* will be only REQ_OP_READ or REQ_OP_WRITE */
	if (op == REQ_OP_WRITE)
		memcpy(dev->data + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->data + offset, nbytes);
}

/*
 * The simple form of the request function.
 */
static void sbull_request(struct request_queue *q)
{
	struct request *req;

	while ((req = blk_fetch_request(q)) != NULL) {
		struct sbull_dev *dev = req->rq_disk->private_data;
		int op = req_op(req);

		if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
			pr_notice("Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}

		sbull_transfer(dev, blk_rq_pos(req),
				blk_rq_cur_sectors(req),
				bio_data(req->bio), op);
		__blk_end_request_all(req, 0);
	}
}


/*
 * Transfer a single BIO.
 */
static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
{
	struct bvec_iter i;
	struct bio_vec bvec;
	sector_t sector = bio->bi_iter.bi_sector;

	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, i) {
		char *buffer = kmap_atomic(bvec.bv_page) + bvec.bv_offset;

		sbull_transfer(dev, sector, bio_cur_bytes(bio),
				buffer, bio_op(bio));
		sector += bio_cur_bytes(bio);
		kunmap_atomic(buffer);
	}
	return 0; /* Always "succeed" */
}

/*
 * Transfer a full request.
 */
static int sbull_xfer_request(struct sbull_dev *dev, struct request *req)
{
	struct req_iterator iter;
	int nsect = 0;
	struct bio_vec bvec;

	/* Macro rq_for_each_bio is gone.
	 * In most cases one should use rq_for_each_segment.
	 */
	rq_for_each_segment(bvec, req, iter) {
		char *buffer = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
		sector_t sector = iter.bio->bi_iter.bi_sector;
		int op = bio_op(iter.bio);

		if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
			pr_notice("Skip non-fs request\n");
			return -1;
		}

		sbull_transfer(dev, sector, bio_cur_bytes(iter.bio), buffer,
				op);
		sector += bio_cur_bytes(iter.bio);
		kunmap_atomic(buffer);
		nsect += iter.bio->bi_iter.bi_size/KERNEL_SECTOR_SIZE;
	}
	return nsect;
}


/*
 * Smarter request function that "handles clustering".
 */
static void sbull_full_request(struct request_queue *q)
{
	struct request *req;
	int sectors_xferred;
	struct sbull_dev *dev = q->queuedata;

	while ((req = blk_peek_request(q)) != NULL) {
		sectors_xferred = sbull_xfer_request(dev, req);
		if (sectors_xferred == -1)
			__blk_end_request(req, -EIO, blk_rq_cur_sectors(req));
		else
			__blk_end_request(req, 0, sectors_xferred << 9);
		/* The above includes a call to add_disk_randomness(). */
	}
}

/*
 * The direct make request version.
 */
static blk_qc_t sbull_make_request(struct request_queue *q, struct bio *bio)
{
	struct sbull_dev *dev = q->queuedata;

	bio->bi_flags = sbull_xfer_bio(dev, bio);
	bio_endio(bio);
	return 0;
}


/*
 * Open and close.
 */

static int sbull_open(struct block_device *bdev, fmode_t mode)
{
	struct sbull_dev *dev = bdev->bd_disk->private_data;

	(void)mode;
	spin_lock(&dev->lock);
	if (!dev->users)
		check_disk_change(bdev);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static void sbull_release(struct gendisk *disk, fmode_t mode)
{
	struct sbull_dev *dev = disk->private_data;

	(void)mode;
	spin_lock(&dev->lock);
	dev->users--;
	spin_unlock(&dev->lock);
}

/*
 * Look for a (simulated) media change.
 */
int sbull_media_changed(struct gendisk *gd)
{
	struct sbull_dev *dev = gd->private_data;

	return dev->media_change;
}

static int sbull_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	long size;
	struct sbull_dev *dev = bdev->bd_disk->private_data;

	/*
	 * since we are a virtual device, we have to make
	 * up something plausible.  So we claim 16 sectors, four heads,
	 * and calculate the corresponding number of cylinders.  We set the
	 * start of data at sector four.
	 */
	size = dev->size * (logical_block_size/KERNEL_SECTOR_SIZE);
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 4;
	return 0;
}

/*
 * The device operations structure.
 */
static const struct block_device_operations sbull_ops = {
	.owner		= THIS_MODULE,
	.open		= sbull_open,
	.release	= sbull_release,
	.media_changed  = sbull_media_changed,
	.getgeo     = sbull_getgeo,
};

/*
 * Set up our internal device.
 */
static void setup_device(struct sbull_dev *dev, int which)
{
	/*
	 * Get some memory.
	 */

	long long sbull_size = memparse(disk_size, NULL);

	memset(dev, 0, sizeof(struct sbull_dev));
	dev->size = sbull_size;
	dev->data = vzalloc(dev->size);
	if (dev->data == NULL) {
		pr_notice("vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	/*
	 * The I/O queue, depending on whether we are using our own
	 * make_request function or not.
	 */
	switch (request_mode) {
	case RM_NOQUEUE:
		dev->queue = blk_alloc_queue(GFP_KERNEL);
		if (dev->queue == NULL)
			goto out_vfree;
		blk_queue_make_request(dev->queue, sbull_make_request);
		break;

	case RM_FULL:
		dev->queue = blk_init_queue(sbull_full_request, &dev->lock);
		if (dev->queue == NULL)
			goto out_vfree;
		break;

	default:
		pr_notice("Bad request mode %d, using simple\n", request_mode);
		/* fall-thru */
	case RM_SIMPLE:
		dev->queue = blk_init_queue(sbull_request, &dev->lock);
		if (dev->queue == NULL)
			goto out_vfree;
		break;
	}
	blk_queue_logical_block_size(dev->queue, logical_block_size);
	dev->queue->queuedata = dev;
	/*
	 * And the gendisk structure.
	 */
	dev->gd = alloc_disk(SBULL_MINORS);
	if (!dev->gd) {
		pr_notice("alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = sbull_major;
	dev->gd->first_minor = which*SBULL_MINORS;
	dev->gd->fops = &sbull_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, 32, "sbull%c", which + 'a');
	set_capacity(dev->gd, sbull_size / KERNEL_SECTOR_SIZE);
	add_disk(dev->gd);
	return;

out_vfree:
	if (dev->data)
		vfree(dev->data);
}



static int __init sbull_init(void)
{
	int i;
	/*
	 * Get registered.
	 */
	sbull_major = register_blkdev(sbull_major, "sbull");
	if (sbull_major <= 0) {
		pr_warn("sbull: unable to get major number\n");
		return -EBUSY;
	}

	/*
	 * Allocate the device array, and initialize each one.
	 */
	Devices = kmalloc(ndevices * sizeof(struct sbull_dev), GFP_KERNEL);
	if (Devices == NULL)
		goto out_unregister;
	for (i = 0; i < ndevices; i++)
		setup_device(Devices + i, i);

	return 0;

out_unregister:
	unregister_blkdev(sbull_major, "sbd");
	return -ENOMEM;
}

static void sbull_exit(void)
{
	int i;

	for (i = 0; i < ndevices; i++) {
		struct sbull_dev *dev = Devices + i;

		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue) {
			/* blk_put_queue() is no longer an exported symbol */
			if (request_mode == RM_NOQUEUE)
				kobject_put(&dev->queue->kobj);
			else
				blk_cleanup_queue(dev->queue);
		}
		if (dev->data)
			vfree(dev->data);
	}
	unregister_blkdev(sbull_major, "sbull");
	kfree(Devices);
}

module_init(sbull_init);
module_exit(sbull_exit);
