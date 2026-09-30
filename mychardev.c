/*
 * mychardev.c - A simple character device driver
 *
 * Demonstrates:
 *   - Dynamic major/minor allocation (alloc_chrdev_region)
 *   - cdev-based registration
 *   - Automatic /dev node creation (class_create + device_create)
 *   - kmalloc/kfree dynamic kernel buffer
 *   - Mutex-protected shared buffer
 *   - Full file_operations: open, read, write, release, llseek
 *
 * Build with the accompanying Makefile using the kernel's Kbuild system.
 */

#include <linux/module.h>      // module_init, module_exit, MODULE_LICENSE, etc.
#include <linux/kernel.h>      // pr_info/pr_err and other kernel-wide helpers
#include <linux/fs.h>          // struct file_operations, alloc_chrdev_region
#include <linux/cdev.h>        // struct cdev, cdev_init, cdev_add, cdev_del
#include <linux/device.h>      // class_create, device_create, device_destroy
#include <linux/uaccess.h>     // copy_to_user, copy_from_user
#include <linux/slab.h>        // kmalloc, kfree
#include <linux/mutex.h>       // struct mutex, mutex_init/lock/unlock
#include <linux/errno.h>       // -ENOMEM, -EFAULT, -EINVAL, -ENOSPC, etc.
#include <linux/string.h>      // memset

#define DEVICE_NAME "mychardev"        // Shown in /dev and /proc/devices
#define CLASS_NAME  "mychardev_class"  // Shown under /sys/class/
#define BUFFER_SIZE 1024                // Size (bytes) of our kernel buffer

/*
 * struct mychardev_data
 * ----------------------
 * Bundles all per-device state together instead of using scattered
 * global variables. See Step 4 Section 1 / Step 6 for full field-by-field
 * explanation of each embedded structure below.
 *
 *   .cdev  -> struct cdev (see Step 6 #2)
 *              Fields used from struct cdev in this file:
 *                - cdev.ops    : set indirectly by cdev_init() to &mychardev_fops
 *                - cdev.owner  : set explicitly below to THIS_MODULE
 *              (cdev.kobj, cdev.list, cdev.dev, cdev.count are all managed
 *               internally by cdev_init()/cdev_add() -- we never touch them
 *               directly.)
 *
 *   .lock  -> struct mutex (see Step 6 #5)
 *              Fields (owner, wait_lock, wait_list) are all internal;
 *              we only ever call mutex_init/lock/unlock/destroy on it.
 */
struct mychardev_data {
    struct cdev cdev;              // Embedded cdev -> enables container_of() in open()
    char *kernel_buffer;           // kmalloc'd buffer -- our device's "storage"
    size_t buffer_size;            // Actual allocated size of kernel_buffer
    struct mutex lock;             // Protects kernel_buffer from concurrent access
};

static struct mychardev_data mychardev;  // The single instance of our device state
static dev_t dev_num;                     // Holds (major,minor) from alloc_chrdev_region
static struct class *mychardev_class;     // Needed for device_create()/device_destroy()

/* ---------- Function prototypes for file_operations ---------- */
static int     mychardev_open(struct inode *inode, struct file *file);
static int     mychardev_release(struct inode *inode, struct file *file);
static ssize_t mychardev_read(struct file *file, char __user *user_buf,
                               size_t len, loff_t *offset);
static ssize_t mychardev_write(struct file *file, const char __user *user_buf,
                                size_t len, loff_t *offset);
static loff_t  mychardev_llseek(struct file *file, loff_t offset, int whence);

/*
 * struct file_operations (see Step 6 #1 for full field table)
 * -------------------------------------------------------------
 * Fields used here:
 *   .owner   -> struct module*  : ties this table to our module (THIS_MODULE)
 *                                 for automatic reference counting on open/close
 *   .open    -> function ptr    : called by VFS on open()
 *   .release -> function ptr    : called by VFS when the last close() happens
 *   .read    -> function ptr    : called by VFS on read()
 *   .write   -> function ptr    : called by VFS on write()
 *   .llseek  -> function ptr    : called by VFS on lseek()
 *
 * Every field NOT listed here (mmap, unlocked_ioctl, poll, ...) is left
 * NULL by this designated-initializer syntax -- VFS handles those as
 * "not implemented" for our device.
 */
static struct file_operations mychardev_fops = {
    .owner   = THIS_MODULE,
    .open    = mychardev_open,
    .release = mychardev_release,
    .read    = mychardev_read,
    .write   = mychardev_write,
    .llseek  = mychardev_llseek,
};

/* =====================================================================
 * mychardev_open()
 * =====================================================================
 * Parameters use struct inode (see Step 6 #3) and struct file (Step 6 #4):
 *   inode->i_cdev  : pointer to the matching struct cdev, set by VFS the
 *                    first time this device file is opened (cached from
 *                    the kernel's device-number lookup table populated
 *                    by cdev_add() in mychardev_init()).
 *   file->private_data : void* slot we use to stash our device context
 *                    so read/write/release/llseek can retrieve it instantly.
 */
static int mychardev_open(struct inode *inode, struct file *file)
{
    struct mychardev_data *dev_data;

    /* Recover our full struct mychardev_data from the embedded cdev
     * pointer handed to us via inode->i_cdev. This works because
     * struct cdev is EMBEDDED (not pointed-to) inside mychardev_data,
     * so container_of can walk backward using the compile-time offset
     * of the "cdev" member within "struct mychardev_data". */
    dev_data = container_of(inode->i_cdev, struct mychardev_data, cdev);

    /* Stash it in struct file's private_data field (Step 6 #4) so every
     * later read/write/release/llseek call on THIS open instance can
     * retrieve it without repeating the container_of() lookup. */
    file->private_data = dev_data;

    pr_info("mychardev: device opened\n");

    return 0;
}

/* =====================================================================
 * mychardev_release()
 * =====================================================================
 * Called when a struct file's reference count reaches zero (last close()
 * on this open instance, or process termination while still open).
 *
 * NOTE: we deliberately do NOT kfree() dev_data->kernel_buffer here --
 * the buffer belongs to the MODULE's lifetime (freed in mychardev_exit),
 * not to any single FILE's lifetime. See Step 4 Section 5 for the full
 * lifetime-separation discussion.
 */
static int mychardev_release(struct inode *inode, struct file *file)
{
    pr_info("mychardev: device closed\n");

    file->private_data = NULL;  // Defensive: avoid stale pointer use after release

    return 0;
}

/* =====================================================================
 * mychardev_read()
 * =====================================================================
 * Parameters (see Step 4 Section 2 / Step 6 #4):
 *   file    : this open instance; file->private_data holds our dev_data
 *   user_buf: __user-tagged pointer -- the destination buffer in the
 *             CALLING PROCESS's address space. Never dereferenced
 *             directly; always via copy_to_user().
 *   len     : bytes requested by the caller's read(fd, buf, len)
 *   offset  : pointer to the current file position (same underlying
 *             value as file->f_pos -- see struct file, Step 6 #4);
 *             VFS updates *offset for us if we don't touch f_pos
 *             directly, since we modify *offset here.
 */
static ssize_t mychardev_read(struct file *file, char __user *user_buf,
                               size_t len, loff_t *offset)
{
    struct mychardev_data *dev_data = file->private_data;
    ssize_t bytes_to_read;
    ssize_t not_copied;

    /* Lock dev_data->lock (struct mutex, Step 6 #5) before touching the
     * shared kernel_buffer. _interruptible so a blocked process can still
     * be woken by a signal (e.g. Ctrl+C) instead of sleeping forever. */
    if (mutex_lock_interruptible(&dev_data->lock))
        return -ERESTARTSYS;

    /* EOF check: nothing left to read from this position onward. */
    if (*offset >= dev_data->buffer_size) {
        bytes_to_read = 0;   /* POSIX convention: 0 == end-of-file */
        goto out;
    }

    /* Clamp to whatever is smaller: what's actually left in the buffer,
     * or what the caller asked for -- prevents reading past the end of
     * our kmalloc'd allocation (a buffer over-read). */
    bytes_to_read = min((size_t)(dev_data->buffer_size - *offset), len);

    /* Copy from KERNEL buffer -> USER buffer. Returns bytes NOT copied
     * (0 == full success). See Step 1 doubt discussion + Step 5 #14. */
    not_copied = copy_to_user(user_buf, dev_data->kernel_buffer + *offset,
                               bytes_to_read);
    if (not_copied) {
        pr_err("mychardev: copy_to_user failed, %zd bytes not copied\n",
               not_copied);
        bytes_to_read = -EFAULT;
        goto out;
    }

    /* Advance the shared file position so the NEXT read() on this fd
     * continues from here (sequential-read semantics). */
    *offset += bytes_to_read;

    pr_info("mychardev: read %zd bytes\n", bytes_to_read);

out:
    mutex_unlock(&dev_data->lock);
    return bytes_to_read;
}

/* =====================================================================
 * mychardev_write()
 * =====================================================================
 * Mirrors mychardev_read(), but data flows USER -> KERNEL, and hitting
 * the end of the buffer is a genuine error (-ENOSPC), not EOF, since
 * there's no more room to store what the caller wants to write.
 */
static ssize_t mychardev_write(struct file *file, const char __user *user_buf,
                                size_t len, loff_t *offset)
{
    struct mychardev_data *dev_data = file->private_data;
    ssize_t bytes_to_write;
    ssize_t not_copied;

    if (mutex_lock_interruptible(&dev_data->lock))
        return -ERESTARTSYS;

    /* Buffer is full from this offset onward -- no room left. */
    if (*offset >= dev_data->buffer_size) {
        pr_warn("mychardev: write attempted past end of buffer\n");
        bytes_to_write = -ENOSPC;
        goto out;
    }

    /* Clamp to prevent writing past the end of our allocation
     * (a buffer OVERFLOW -- more severe than read's over-read case,
     * since it would corrupt adjacent kernel heap memory). */
    bytes_to_write = min((size_t)(dev_data->buffer_size - *offset), len);

    /* Copy from USER buffer -> KERNEL buffer.
     * copy_from_user(to, from, n) -- destination first, same convention
     * as memcpy(). See Step 4 Section 7 / Step 5 #14. */
    not_copied = copy_from_user(dev_data->kernel_buffer + *offset,
                                 user_buf, bytes_to_write);
    if (not_copied) {
        pr_err("mychardev: copy_from_user failed, %zd bytes not copied\n",
               not_copied);
        bytes_to_write = -EFAULT;
        goto out;
    }

    *offset += bytes_to_write;

    pr_info("mychardev: wrote %zd bytes\n", bytes_to_write);

out:
    mutex_unlock(&dev_data->lock);
    return bytes_to_write;
}

/* =====================================================================
 * mychardev_llseek()
 * =====================================================================
 * Implements SEEK_SET / SEEK_CUR / SEEK_END, with bounds-checking to
 * ensure the resulting position never goes negative or past the end
 * of the buffer -- both would otherwise let a later read()/write()
 * compute an out-of-bounds pointer (kernel_buffer + offset).
 */
static loff_t mychardev_llseek(struct file *file, loff_t offset, int whence)
{
    struct mychardev_data *dev_data = file->private_data;
    loff_t new_pos;

    if (mutex_lock_interruptible(&dev_data->lock))
        return -ERESTARTSYS;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;                          /* absolute position */
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;             /* relative to current
                                                        -- file->f_pos is the
                                                        same field struct file
                                                        exposes; see Step 6 #4 */
        break;
    case SEEK_END:
        new_pos = dev_data->buffer_size + offset;   /* relative to end */
        break;
    default:
        mutex_unlock(&dev_data->lock);
        return -EINVAL;
    }

    /* Reject any position outside [0, buffer_size] -- see Step 4
     * Section 8 for why this check is a hard security/correctness
     * requirement, not optional defensive dressing. */
    if (new_pos < 0 || new_pos > dev_data->buffer_size) {
        mutex_unlock(&dev_data->lock);
        return -EINVAL;
    }

    file->f_pos = new_pos;   /* commit the new position */

    pr_info("mychardev: seek to position %lld\n", new_pos);

    mutex_unlock(&dev_data->lock);
    return new_pos;
}

/* =====================================================================
 * mychardev_init() -- module load entry point
 * =====================================================================
 * Registration order matters: each step below is undone, in EXACT
 * REVERSE order, by the matching "fail_*" label if a later step fails,
 * and by mychardev_exit() on a normal, successful unload.
 */
static int __init mychardev_init(void)
{
    int ret;
    struct device *dev_ret;

    /* 1) Dynamically reserve a major number + 1 minor number.
     *    Fills dev_num (dev_t) -- see Step 5 #2. */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("mychardev: failed to allocate chrdev region\n");
        return ret;
    }
    pr_info("mychardev: allocated major=%d minor=%d\n",
            MAJOR(dev_num), MINOR(dev_num));

    /* 2) Allocate our device's kernel buffer (kmalloc, Step 5 #10).
     *    memset() zeroes it -- kmalloc does NOT zero memory itself,
     *    and leaving it uninitialized could leak stale kernel memory
     *    contents to user space on a read() before any write(). */
    mychardev.kernel_buffer = kmalloc(BUFFER_SIZE, GFP_KERNEL);
    if (!mychardev.kernel_buffer) {
        pr_err("mychardev: failed to allocate kernel buffer\n");
        ret = -ENOMEM;
        goto fail_buffer;
    }
    mychardev.buffer_size = BUFFER_SIZE;
    memset(mychardev.kernel_buffer, 0, BUFFER_SIZE);

    /* 3) Initialize the mutex (struct mutex, Step 6 #5) BEFORE the
     *    device becomes reachable from user space. */
    mutex_init(&mychardev.lock);

    /* 4) Initialize our embedded struct cdev (Step 6 #2) and link it
     *    to our file_operations table, then register it live in the
     *    kernel's char-device lookup table (keyed by dev_num). */
    cdev_init(&mychardev.cdev, &mychardev_fops);
    mychardev.cdev.owner = THIS_MODULE;   /* cdev.owner field, Step 6 #2 */

    ret = cdev_add(&mychardev.cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("mychardev: failed to add cdev\n");
        goto fail_cdev;
    }

    /* 5) Create the device class -- shows up under /sys/class/. */
    mychardev_class = class_create(CLASS_NAME);
    if (IS_ERR(mychardev_class)) {
        pr_err("mychardev: failed to create class\n");
        ret = PTR_ERR(mychardev_class);
        goto fail_class;
    }

    /* 6) Create the actual device -- triggers a uevent that udev uses
     *    to materialize /dev/mychardev. Stored in a local variable
     *    ONCE (this fixes the double-call bug flagged in Step 4). */
    dev_ret = device_create(mychardev_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(dev_ret)) {
        pr_err("mychardev: failed to create device\n");
        ret = PTR_ERR(dev_ret);
        goto fail_device;
    }

    pr_info("mychardev: module loaded successfully\n");
    return 0;

    /* Error unwind path -- reverse order of what succeeded above. */
fail_device:
    class_destroy(mychardev_class);
fail_class:
    cdev_del(&mychardev.cdev);
fail_cdev:
    mutex_destroy(&mychardev.lock);
    kfree(mychardev.kernel_buffer);
fail_buffer:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

/* =====================================================================
 * mychardev_exit() -- module unload entry point
 * =====================================================================
 * Tears down every resource created in mychardev_init(), in exact
 * reverse order (mirrors the fail_* labels above).
 */
static void __exit mychardev_exit(void)
{
    device_destroy(mychardev_class, dev_num);
    class_destroy(mychardev_class);
    cdev_del(&mychardev.cdev);
    mutex_destroy(&mychardev.lock);
    kfree(mychardev.kernel_buffer);
    unregister_chrdev_region(dev_num, 1);

    pr_info("mychardev: module unloaded successfully\n");
}

module_init(mychardev_init);
module_exit(mychardev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple character device driver demonstrating cdev, kmalloc, mutex, and file_operations");
MODULE_VERSION("1.0");
