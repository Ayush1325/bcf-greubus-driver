#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/serdev.h>

static const struct of_device_id bcf_greybus_of_match[] = {{
    .compatible = "beagle,bcfserial",
}};

static struct serdev_device_driver bcfserial_driver = {
    .probe = bcf_greybus_probe,
    .remove = bcf_greybus_remove,
    .driver =
        {
            .name = BCFSERIAL_DRV_NAME,
            .of_match_table = of_match_ptr(bcf_greybus_of_match),
        },
};

module_serdev_device_driver(bcfserial_driver);

static int __init bcf_greybus_init(void) {
  pr_info("Init BCF_GREYBUS Module\n");
  return 0;
}

static void __exit bcf_greybus_exit(void) {
  pr_info("Exit BCF_GREYBUS Module\n");
}

module_init(bcf_greybus_init);
module_exit(bcf_greybus_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ayush Singh <ayushdevel1325@gmail.com>");
MODULE_DESCRIPTION("A Greybus driver for BeaglePlay");
