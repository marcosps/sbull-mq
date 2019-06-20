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
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>

#ifndef BLK_STS_OK
typedef int blk_status_t;
#define BLK_STS_OK 0
#define OLDER_KERNEL 1
#endif

#ifndef BLK_STS_IOERR
#define BLK_STS_IOERR 10
#endif

#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT 9
#endif

/* FIXME: implement these macros in kernel mainline */
#define size_to_sectors(size) ((size) >> SECTOR_SHIFT)
#define sectors_to_size(size) ((size) << SECTOR_SHIFT)

MODULE_LICENSE("Dual BSD/GPL");

static int sbull_major;
module_param(sbull_major, int, 0);
static int logical_block_size = 512;
module_param(logical_block_size, int, 0);
static char* disk_size = "256M";
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
 * The internal representation of our device.
 */
struct sbull_dev {
	int size;                       /* Device size in sectors */
	u8 *data;                       /* The data array */
	spinlock_t lock;                /* For mutual exclusion */
	struct request_queue *queue;    /* The device request queue */
	struct gendisk *gd;             /* The gendisk structure */
	struct blk_mq_tag_set tag_set;
};

static struct sbull_dev *Devices;

/* Handle an I/O request */
static blk_status_t sbull_transfer(struct sbull_dev *dev, sector_t offset,
			unsigned int len, char *buffer, int op)
{
	if ((offset + len) > dev->size) {
		pr_notice("Beyond-end write (%lld %u)\n", offset, len);
		return BLK_STS_IOERR;
	}

	if  (debug)
		pr_info("%s: %s, len: %u, offset: %lld",
			dev->gd->disk_name,
			op == REQ_OP_WRITE ? "WRITE" : "READ", len,
			offset);

	/* will be only REQ_OP_READ or REQ_OP_WRITE */
	if (op == REQ_OP_WRITE)
		memcpy(dev->data + offset, buffer, len);
	else
		memcpy(buffer, dev->data + offset, len);

	return BLK_STS_OK;
}

static blk_status_t sbull_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	struct sbull_dev *dev = req->rq_disk->private_data;
	int op = req_op(req);
	sector_t sector = blk_rq_pos(req);
	unsigned int len;
	struct bio_vec bvec;
	struct req_iterator iter;
	void *mem;
	blk_status_t ret = BLK_STS_OK;

	blk_mq_start_request(req);

	if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
		pr_notice("Skip non-fs request\n");
		blk_mq_end_request(req, BLK_STS_IOERR);
		spin_unlock(&dev->lock);
		return BLK_STS_IOERR;
	}

	spin_lock(&dev->lock);
	rq_for_each_segment(bvec, req, iter) {
		len = bvec.bv_len;
		mem = kmap_atomic(bvec.bv_page);

		ret = sbull_transfer(dev, sectors_to_size(sector),
				len,
				mem + bvec.bv_offset, op);

		sector += size_to_sectors(len);

		kunmap_atomic(mem);
	}
	spin_unlock(&dev->lock);

	blk_mq_end_request(req, ret);
	return ret;
}

/*
 * The device operations structure.
 */
static const struct block_device_operations sbull_ops = {
	.owner		= THIS_MODULE,
};

static const struct blk_mq_ops sbull_mq_ops = {
	.queue_rq = sbull_queue_rq,
};

static struct request_queue *create_req_queue(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

#ifndef OLDER_KERNEL
	q = blk_mq_init_sq_queue(set, &sbull_mq_ops,
			2, BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING);
#else
	int ret;

	memset(set, 0, sizeof(*set));
	set->ops = &sbull_mq_ops;
	set->nr_hw_queues = 1;
	/*set->nr_maps = 1;*/
	set->queue_depth = 2;
	set->numa_node = NUMA_NO_NODE;
	set->flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING;

	ret = blk_mq_alloc_tag_set(set);
	if (ret)
		return ERR_PTR(ret);

	q = blk_mq_init_queue(set);
	if (IS_ERR(q)) {
		blk_mq_free_tag_set(set);
		return q;
	}
#endif

	return q;
}

/*
 * Set up our internal device.
 */
static void setup_device(struct sbull_dev *dev, int which)
{
	long long sbull_size = memparse(disk_size, NULL);

	memset(dev, 0, sizeof(struct sbull_dev));
	dev->size = sbull_size;
	dev->data = vzalloc(dev->size);
	if (dev->data == NULL) {
		pr_notice("vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	dev->queue = create_req_queue(&dev->tag_set);
	if (IS_ERR(dev->queue))
		goto out_vfree;

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
	set_capacity(dev->gd, size_to_sectors(sbull_size));
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
	unregister_blkdev(sbull_major, "sbull");
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
		if (dev->queue)
			blk_cleanup_queue(dev->queue);

		if (dev->data)
			vfree(dev->data);
	}
	unregister_blkdev(sbull_major, "sbull");
	kfree(Devices);
}

module_init(sbull_init);
module_exit(sbull_exit);
