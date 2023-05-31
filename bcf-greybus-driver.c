#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/serdev.h>

static int bcf_greybus_tty_receive(struct serdev_device *serdev,
                                   const unsigned char *data, size_t count) {
  pr_info("BCF_GREYBUS tty recieve\n");
  return 0;
}

static void bcf_greybus_tty_wakeup(struct serdev_device *serdev) {
  pr_info("BCF_GREYBUS tty wakeup\n");
}

static struct serdev_device_ops bcf_greybus_ops = {
    .receive_buf = bcf_greybus_tty_receive,
    .write_wakeup = bcf_greybus_tty_wakeup,
};

struct bcf_greybus {
  struct serdev_device *serdev;
};

static const struct of_device_id bcf_greybus_of_match[] = {
    {
        .compatible = "beagle,bcfserial",
    },
    {},
};
MODULE_DEVICE_TABLE(of, bcf_greybus_of_match);

static int bcf_greybus_probe(struct serdev_device *serdev) {
  pr_info("Probe BCF_GREYBUS\n");

  u32 speed = 115200;
  int ret = 0;

  struct bcf_greybus *bcf_greybus =
      devm_kmalloc(&serdev->dev, sizeof(struct bcf_greybus), GFP_KERNEL);

  bcf_greybus->serdev = serdev;

  serdev_device_set_drvdata(serdev, bcf_greybus);
  serdev_device_set_client_ops(serdev, &bcf_greybus_ops);
  ret = serdev_device_open(serdev);
  if (ret) {
    pr_err("Unable to Open Device");
    return ret;
  }

  speed = serdev_device_set_baudrate(serdev, speed);
  pr_info("Using baudrate %u\n", speed);

  serdev_device_set_flow_control(serdev, false);

  pr_info("Successful device initilaization");

  return 0;
}

static void bcf_greybus_remove(struct serdev_device *serdev) {
  pr_info("Remove BCF_GREYBUS\n");

  serdev_device_close(serdev);
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
