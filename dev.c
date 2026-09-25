#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include "module.h"
#include "ioctl.h"


static dev_t dev_num;            
static struct cdev module_cdev;   
static struct class *module_class;

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = manage_ioctl_function,
};


static char *throttle_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;
    return NULL;
}

/* --- SETUP DEL DEVICE --- */
int setup_monitor_device(void) {
    int ret;
    struct device *dev_ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, "throttle_dev");
    if (ret < 0) {
        printk(KERN_ERR "alloc_chrdev_region fallita (%d)\n", ret);
        return ret;
    }

    cdev_init(&module_cdev, &fops);
    module_cdev.owner = THIS_MODULE;

    ret = cdev_add(&module_cdev, dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "cdev_add fallita (%d)\n", ret);
        goto err_cdev;
    }

    module_class = class_create("module_class");
    if (IS_ERR(module_class)) {
        ret = PTR_ERR(module_class);
        printk(KERN_ERR "class_create fallita (%d)\n", ret);
        goto err_class;
    }
    module_class->devnode = throttle_devnode;

    dev_ret = device_create(module_class, NULL, dev_num, NULL, "throttle_dev");
    if (IS_ERR(dev_ret)) {
        ret = PTR_ERR(dev_ret);
        printk(KERN_ERR "device_create fallita (%d)\n", ret);
        goto err_device;
    }

    printk(KERN_INFO "Device /dev/throttle_dev creato. Major=%d Minor=%d\n",
           MAJOR(dev_num), MINOR(dev_num));
           
    return 0;

err_device:
    class_destroy(module_class);
err_class:
    cdev_del(&module_cdev);
err_cdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

/* --- RIMOZIONE DEL DEVICE --- */
void cleanup_monitor_device(void) {
    device_destroy(module_class, dev_num);      
    class_destroy(module_class);                
    cdev_del(&module_cdev);                     
    unregister_chrdev_region(dev_num, 1);      
}
