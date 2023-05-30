#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/serdev.h>

static const struct of_device_id bcf_greybus_of_match[] = {
    {
        .compatible = "beagle,bcfserial",
    },
    {},
};
MODULE_DEVICE_TABLE(of, bcf_greybus_of_match);

static int bcf_greybus_probe(struct serdev_device *serdev) {
  pr_info("Probe BCF_GREYBUS\n");
  return 0;
}

static void bcf_greybus_remove(struct serdev_device *serdev) {
  pr_info("Remove BCF_GREYBUS\n");
}

static struct serdev_device_driver bcfserial_driver = {
    .probe = bcf_greybus_probe,
    .remove = bcf_greybus_remove,
    .driver =
        {
            .name = "BCF_GREYBUS",
            .of_match_table = of_match_ptr(bcf_greybus_of_match),
        },
};

module_serdev_device_driver(bcfserial_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ayush Singh <ayushdevel1325@gmail.com>");
MODULE_DESCRIPTION("A Greybus driver for BeaglePlay");
MODULE_VERSION("0.1.0");
