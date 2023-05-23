#ifndef __KERNEL__
#define __KERNEL__
#endif
#ifndef MODULE
#define MODULE
#endif

#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/rwsem.h>

/* This would normally go in a .h file */
#define DEVICE_NAME "dm510_dev"
#define DM510_MAJOR 255
#define BASE_MINOR 0
#define DEVICE_COUNT 2

#define MAX_WRITERS 1
#define DEFAULT_MAX_READERS 16
#define DEFAULT_BUFFER_SIZE 1024

#define IOCTL_GET_MAX_READERS _IOR(DM510_MAJOR, 0, ssize_t)
#define IOCTL_SET_MAX_READERS _IOW(DM510_MAJOR, 1, size_t)
#define IOCTL_GET_BUFFER_SIZE _IOR(DM510_MAJOR, 2, ssize_t)
#define IOCTL_SET_BUFFER_SIZE _IOW(DM510_MAJOR, 3, size_t)
#define IOCTL_MAX_NR 3

struct dm510_dev {
	struct mutex mutex;
	struct dm510_dev *buddy;
	atomic_t nreaders, nwriters;
	unsigned long max_readers;
	struct kfifo kfifo;
	wait_queue_head_t readq, writeq;
	struct cdev cdev;
};

static int dm510_open(struct inode *, struct file *);
static int dm510_release(struct inode *, struct file *);
static ssize_t dm510_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t dm510_write(struct file *, const char __user *, size_t,
			   loff_t *);
static long dm510_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
/* end of what really should have been in a .h file */

static struct dm510_dev *dm510_devs = NULL;

static int dm510_open(struct inode *inode, struct file *filp)
{
	struct dm510_dev *dev;
	dev = container_of(inode->i_cdev, struct dm510_dev, cdev);
	filp->private_data = dev;

	pr_info("DM510: Opening DM510-%d.", MINOR(dev->cdev.dev));

	if (filp->f_mode & FMODE_READ &&
	    atomic_fetch_inc(&dev->nreaders) >= dev->max_readers) {
		atomic_dec(&dev->nreaders);
		pr_info("DM510-%d: Too many readers.", MINOR(dev->cdev.dev));
		return -EBUSY;
	}
	/* Concurrent writers can kindly not */
	if (filp->f_mode & FMODE_WRITE &&
	    atomic_fetch_inc(&dev->nwriters) >= MAX_WRITERS) {
		atomic_dec(&dev->nwriters);
		pr_info("DM510-%d: Too many writers.", MINOR(dev->cdev.dev));
		return -EBUSY;
	}

	/* We don't implement lseek, it doesn't make much sense here... */
	return nonseekable_open(inode, filp);
}

static int dm510_release(struct inode *inode, struct file *filp)
{
	struct dm510_dev *dev;
	dev = filp->private_data;

	pr_info("DM510: Closing DM510-%d.", MINOR(dev->cdev.dev));

	if (filp->f_mode & FMODE_WRITE)
		atomic_dec(&dev->nwriters);
	if (filp->f_mode & FMODE_READ)
		atomic_dec(&dev->nreaders);

	return 0;
}

static ssize_t dm510_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *f_pos)
{
	struct dm510_dev *dev;
	int ret;
	unsigned int copied;

	dev = filp->private_data;
	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;
	while (kfifo_is_empty(&dev->kfifo)) {
		mutex_unlock(&dev->mutex);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(dev->readq,
					     !kfifo_is_empty(&dev->kfifo)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}
	ret = kfifo_to_user(&dev->kfifo, buf, count, &copied);
	mutex_unlock(&dev->mutex);
	if (unlikely(ret))
		return ret;

	wake_up_interruptible(&dev->buddy->writeq);

	return copied;
}

static ssize_t dm510_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *f_pos)
{
	struct dm510_dev *dev, *buddy;
	int ret;
	unsigned int copied;

	dev = filp->private_data;
	buddy = dev->buddy;

	if (mutex_lock_interruptible(&buddy->mutex))
		return -ERESTARTSYS;
	while (kfifo_is_full(&buddy->kfifo)) {
		mutex_unlock(&buddy->mutex);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(dev->writeq,
					     !kfifo_is_full(&buddy->kfifo)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&buddy->mutex))
			return -ERESTARTSYS;
	}
	ret = kfifo_from_user(&buddy->kfifo, buf, count, &copied);
	mutex_unlock(&buddy->mutex);
	if (unlikely(ret))
		return ret;

	wake_up_interruptible(&buddy->readq);

	return copied;
}

static long resize_kfifo(struct dm510_dev *dev, unsigned long buffer_size)
{
	int ret;
	struct kfifo new_kfifo;

	ret = kfifo_alloc(&new_kfifo, buffer_size, GFP_KERNEL);
	if (ret) {
		pr_err("DM510: Resizing kfifo for DM510-%d failed with error code %d.\n",
		       MINOR(dev->cdev.dev), ret);
		return ret;
	}
	/* We're already holding the lock */
	kfifo_free(&dev->kfifo);
	dev->kfifo = new_kfifo;

	pr_info("DM510: Resized kfifo for DM510-%d.\n", MINOR(dev->cdev.dev));

	return 0;
}

static long dm510_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dm510_dev *dev;
	long ret = 0;

	dev = filp->private_data;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	switch (cmd) {
	case IOCTL_GET_MAX_READERS:
		pr_info("DM510: GET_MAX_READERS ioctl called.\n");
		ret = dev->max_readers;
		break;
	case IOCTL_SET_MAX_READERS: {
		pr_info("DM510: SET_MAX_READERS ioctl called.\n");
		if (!capable(CAP_SYS_ADMIN)) {
			ret = -EPERM;
			break;
		}
		dev->max_readers = arg;
		break;
	}
	case IOCTL_GET_BUFFER_SIZE:
		pr_info("DM510: GET_BUFFER_SIZE ioctl called.\n");
		ret = kfifo_size(&dev->kfifo);
		break;
	case IOCTL_SET_BUFFER_SIZE: {
		pr_info("DM510: SET_BUFFER_SIZE ioctl called.\n");
		if (!capable(CAP_SYS_ADMIN)) {
			ret = -EPERM;
			break;
		}
		ret = resize_kfifo(dev, arg);
		break;
	}
	default:
		pr_info("DM510: ioctl called with unknown command.\n");
		ret = -ENOIOCTLCMD;
	}

	mutex_unlock(&dev->mutex);

	return ret;
}

static struct file_operations dm510_fops = {
	.owner = THIS_MODULE,
	.read = dm510_read,
	.write = dm510_write,
	.open = dm510_open,
	.release = dm510_release,
	.unlocked_ioctl = dm510_ioctl,
	.llseek = no_llseek,
};

static void dm510_exit(void)
{
	if (dm510_devs) {
		for (int i = 0; i < DEVICE_COUNT; i++) {
			cdev_del(&dm510_devs[i].cdev);
			kfifo_free(&dm510_devs[i].kfifo);
		}
		kfree(dm510_devs);
	}

	unregister_chrdev_region(MKDEV(DM510_MAJOR, BASE_MINOR), DEVICE_COUNT);
	pr_info("DM510: Module unloaded.\n");
}

#define CHECK_ERR(exp)                 \
	{                              \
		int __ret = exp;       \
		if (unlikely(__ret)) { \
			dm510_exit();  \
			return __ret;  \
		}                      \
	}

static int __init dm510_init(void)
{
	int ret;

	ret = register_chrdev_region(MKDEV(DM510_MAJOR, BASE_MINOR),
				     DEVICE_COUNT, DEVICE_NAME);
	if (ret)
		return ret;

	dm510_devs =
		kmalloc(DEVICE_COUNT * sizeof(struct dm510_dev), GFP_KERNEL);
	if (!dm510_devs) {
		dm510_exit();
		return -ENOMEM;
	}

	for (int i = 0; i < DEVICE_COUNT; i++) {
		cdev_init(&dm510_devs[i].cdev, &dm510_fops);
		dm510_devs[i].cdev.owner = THIS_MODULE;
		dm510_devs[i].buddy = &dm510_devs[(i + 1) % DEVICE_COUNT];
		dm510_devs[i].max_readers = DEFAULT_MAX_READERS;
		atomic_set(&dm510_devs[i].nreaders, 0);
		mutex_init(&dm510_devs[i].mutex);
		init_waitqueue_head(&dm510_devs[i].readq);
		init_waitqueue_head(&dm510_devs[i].writeq);
		CHECK_ERR(kfifo_alloc(&dm510_devs[i].kfifo, DEFAULT_BUFFER_SIZE,
				      GFP_KERNEL));
	}

	/* Add cdevs *after* initializing everything at once because we depend on buddies */
	for (int i = 0; i < DEVICE_COUNT; i++) {
		CHECK_ERR(cdev_add(&dm510_devs[i].cdev,
				   MKDEV(DM510_MAJOR, BASE_MINOR + i), 1));
	}

	pr_info("DM510: Module installed.\n");

	return 0;
}

module_init(dm510_init);
module_exit(dm510_exit);

MODULE_AUTHOR("...Frederik List");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DM510 module");
