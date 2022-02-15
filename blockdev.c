#include <linux/kernel.h>
#include <linux/module.h>   
#include <linux/fs.h>       // register_blkdev / unregister_blkdev， block_device_operations
#include <linux/genhd.h>    // register / allocate / del gendisk. This is the actual disk
#include <linux/blkdev.h>   // request_queue, tags
#include <linux/blk-mq.h>   // multiqueue structs like tagset and init_queue


MODULE_AUTHOR("Keer Mei");
MODULE_DESCRIPTION("A SIMPLE BLOCK DRIVER");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.0.0");

static int major_num = 0;   // 0 makes the kernel dynamically allocate one
#define BLOCK_DEV_NAME  "my_block_device"
#define NR_SECTORS      1024
#define K_SECTOR_SIZE   512
#define BUFFER_SIZE     8


// initialize disk data structure
struct my_block_dev {
    struct gendisk *gd;
    struct request_queue *queue;    // device request queue, this is canonical for block devices
    struct blk_mq_tag_set tag_set;  // tagset
    spinlock_t lock;                // spinlock for locking queues
    char *data;                     // device data
    int size;                       // device size
};

struct my_block_dev *dev;
/* ====================================================================================================================================================== */
static int my_block_open(struct block_device *dev, fmode_t mode)
{
    pr_info("Device opened successfully\n");
    return 0;
};

static void my_block_release(struct gendisk *gd, fmode_t mode)
{
    pr_info("Device closed successfully\n");
}
/* ====================================================================================================================================================== */
const struct block_device_operations my_bops = {
    .owner = THIS_MODULE,
    .open = my_block_open,
    .release = my_block_release,
};

/* ====================================================================================================================================================== */
// what to do when you actually get the data
int do_request(struct request *rq, unsigned int *nr_bytes)
{
    struct bio_vec bvec;
    struct req_iterator iter;
    loff_t offset = blk_rq_pos(rq) << SECTOR_SHIFT; // required position for the Read and Write
    void *data;
    unsigned long data_len;

    /* Iterate over reuquests in the queue */
    // this is a define for a loop...    
    rq_for_each_segment(bvec, rq, iter)
    {
        data = page_address(bvec.bv_page) + bvec.bv_offset; /* Get I/O data */
        data_len = bvec.bv_len; /* Length of the data buffer */

        if (rq_data_dir(rq) == WRITE) {
            printk("Writing data to the blk-mq device\n");
        } else {
            printk("Reading data from the blk-mq device\n");
        }

        offset += data_len;
        *nr_bytes += data_len;  /* Increment amount of the processed bytes. */
    }

    return 0;
};

// queue callback function - copied from https://olegkutkov.me/2020/02/10/linux-block-device-driver/
static blk_status_t my_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
    unsigned int nr_bytes = 0;
    blk_status_t status = BLK_STS_OK;
    struct request *rq = bd->rq;  /* Get one top request */

    /* 
        Start new request procedure.
        Please note that this function sets a timer for the data transaction. 
        This internal timer is used to measure request processing time and to detect problems with hardware. 
    */
    blk_mq_start_request(rq);

    /* Serve request */     
    if (do_request(rq, &nr_bytes) != 0) {
        status = BLK_STS_IOERR;
    }

    /* Notify blk-mq about processed bytes */
    if (blk_update_request(rq, status, nr_bytes)) {
        BUG();
    }

    /* End request procedure */     
    __blk_mq_end_request(rq, status);
     
    return status;
};
/* ====================================================================================================================================================== */
const struct blk_mq_ops my_bmq_ops = {
    .queue_rq = my_queue_rq,
};
/* ====================================================================================================================================================== */
static int init_fn(void)
{
    // register number dynamically and replace old value
    major_num = register_blkdev(major_num, BLOCK_DEV_NAME);
    if (major_num < 0) {
        pr_info("Failed to register blk device numbers\n");
        return -EBUSY;
    }

    // add actual disk structure to kernel, the number is the qty of minors and not the minor number
    dev->gd = alloc_disk(1);        
    if (!dev->gd) {
        pr_info("Failed to allocate disk\n");
        goto r_disk;
    }

    // need to init a queue. we use tag_set here so we dont have to go through the hassle to initialize our own tag_set
    dev->queue = blk_mq_init_sq_queue(&dev->tag_set, &my_bmq_ops, 128, BLK_MQ_F_SHOULD_MERGE);

    dev->gd->major = major_num;
    dev->gd->first_minor = 0;
    dev->gd->fops = &my_bops;                           // block device operations
    dev->gd->queue = dev->queue;                        // request queue
    dev->gd->private_data = dev;                        // private data, like char device
    snprintf(dev->gd->disk_name, 32, BLOCK_DEV_NAME);   // put the device name into the disk name member of gd
    set_capacity(dev->gd, NR_SECTORS);                  // set the capacity
    dev->size = NR_SECTORS * K_SECTOR_SIZE;
    dev->data = kmalloc(BUFFER_SIZE, GFP_KERNEL);
    if (dev->data == NULL) {
        pr_info("Cannot allocate memory for dev\n");
        goto r_blk_init;
    }

    // add disk makes it available to the system and this should only be done once everything else is setup because add_disk
    // makes it immediately available
    add_disk(dev->gd);

    pr_info("Kernel Module - %s inserted successfully...\n", BLOCK_DEV_NAME);
    return 0;

r_blk_init:
    if (dev->gd) {
        del_gendisk(dev->gd);
    }
    if (dev->queue) {
        blk_mq_free_tag_set(&dev->tag_set);
        blk_cleanup_queue(dev->queue);
    }
r_disk:
    unregister_blkdev(major_num, BLOCK_DEV_NAME);
    return -ENOMEM;
};

static void cleanup_fn(void)
{   
    // delete disk
    if (dev->gd) {
        del_gendisk(dev->gd);
        put_disk(dev->gd);
    }
    // cleanup queue
    if (dev->queue){
        blk_mq_free_tag_set(&dev->tag_set);
        blk_cleanup_queue(dev->queue);
    }
    kfree(dev->data);
    // unregister numbers
    unregister_blkdev(major_num, BLOCK_DEV_NAME);
    pr_info("Device driver - %s removed successfully\n", BLOCK_DEV_NAME);
};

module_init(init_fn);
module_exit(cleanup_fn);
