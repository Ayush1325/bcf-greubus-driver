#include <linux/circ_buf.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/serdev.h>

#define DEBUG

#define BCF_GREYBUS_DRV_VERSION "0.1.0"
#define BCF_GREYBUS_DRV_NAME "bcfgreybus"
#define TX_CIRC_BUF_SIZE 1024

struct bcf_greybus {
  struct serdev_device *serdev;

  struct work_struct tx_work;
  spinlock_t tx_producer_lock;
  spinlock_t tx_consumer_lock;
  struct circ_buf tx_circ_buf;
};

static const struct of_device_id bcf_greybus_of_match[] = {
    {
        .compatible = "beagle,bcfserial",
    },
    {},
};
MODULE_DEVICE_TABLE(of, bcf_greybus_of_match);

static int bcf_greybus_tty_receive(struct serdev_device *serdev,
                                   const unsigned char *data, size_t count) {
  struct bcf_greybus *bcf_greybus;

  bcf_greybus = serdev_device_get_drvdata(serdev);
  dev_info(&bcf_greybus->serdev->dev, "tty recieve\n");
  dev_info(&bcf_greybus->serdev->dev, "Data: %s\n", data);

  return count;
}

static void bcf_greybus_tty_wakeup(struct serdev_device *serdev) {
  struct bcf_greybus *bcf_greybus;

  bcf_greybus = serdev_device_get_drvdata(serdev);
  dev_info(&bcf_greybus->serdev->dev, "tty wakeup\n");

  schedule_work(&bcf_greybus->tx_work);
}

static struct serdev_device_ops bcf_greybus_ops = {
    .receive_buf = bcf_greybus_tty_receive,
    .write_wakeup = bcf_greybus_tty_wakeup,
};

static void bcf_greybus_append(struct bcf_greybus *bcf_greybus, u8 value) {
  // must be locked already
  int head = bcf_greybus->tx_circ_buf.head;

  while (true) {
    int tail = READ_ONCE(bcf_greybus->tx_circ_buf.tail);

    if (CIRC_SPACE(head, tail, TX_CIRC_BUF_SIZE) >= 1) {

      bcf_greybus->tx_circ_buf.buf[head] = value;

      smp_store_release(&(bcf_greybus->tx_circ_buf.head),
                        (head + 1) & (TX_CIRC_BUF_SIZE - 1));
      return;
    } else {
      dev_dbg(&bcf_greybus->serdev->dev, "Tx circ buf full\n");
      usleep_range(3000, 5000);
    }
  }
}

static void bcf_greybus_serdev_write_locked(struct bcf_greybus *bcf_greybus) {
  // must be locked already
  int head = smp_load_acquire(&bcf_greybus->tx_circ_buf.head);
  int tail = bcf_greybus->tx_circ_buf.tail;
  int count = CIRC_CNT_TO_END(head, tail, TX_CIRC_BUF_SIZE);
  int written;

  if (count >= 1) {
    written = serdev_device_write_buf(
        bcf_greybus->serdev, &bcf_greybus->tx_circ_buf.buf[tail], count);
    dev_info(&bcf_greybus->serdev->dev, "Written Data of Len: %u\n", written);

    smp_store_release(&(bcf_greybus->tx_circ_buf.tail),
                      (tail + written) & (TX_CIRC_BUF_SIZE - 1));
  }
}

static void bcf_greybus_uart_transmit(struct work_struct *work) {
  struct bcf_greybus *bcf_greybus =
      container_of(work, struct bcf_greybus, tx_work);

  spin_lock_bh(&bcf_greybus->tx_consumer_lock);
  dev_info(&bcf_greybus->serdev->dev, "Write to tx buffer");
  bcf_greybus_serdev_write_locked(bcf_greybus);
  spin_unlock_bh(&bcf_greybus->tx_consumer_lock);
}

// A simple function to write "HelloWorld" over UART.
// TODO: Remove in the future.
static void hello_world(struct bcf_greybus *bcf_greybus) {
  const char msg[] = "HelloWorld\0";
  const ssize_t msg_len = strlen(msg);
  ssize_t i;

  spin_lock(&bcf_greybus->tx_producer_lock);
  for (i = 0; i < msg_len; ++i) {
    bcf_greybus_append(bcf_greybus, msg[i]);
  }
  spin_unlock(&bcf_greybus->tx_producer_lock);

  spin_lock(&bcf_greybus->tx_consumer_lock);
  bcf_greybus_serdev_write_locked(bcf_greybus);
  spin_unlock(&bcf_greybus->tx_consumer_lock);

  dev_info(&bcf_greybus->serdev->dev, "Written Hello World");
};

static int bcf_greybus_probe(struct serdev_device *serdev) {
  u32 speed = 115200;
  int ret = 0;

  struct bcf_greybus *bcf_greybus =
      devm_kmalloc(&serdev->dev, sizeof(struct bcf_greybus), GFP_KERNEL);

  bcf_greybus->serdev = serdev;

  INIT_WORK(&bcf_greybus->tx_work, bcf_greybus_uart_transmit);
  spin_lock_init(&bcf_greybus->tx_producer_lock);
  spin_lock_init(&bcf_greybus->tx_consumer_lock);
  bcf_greybus->tx_circ_buf.head = 0;
  bcf_greybus->tx_circ_buf.tail = 0;
  bcf_greybus->tx_circ_buf.buf =
      devm_kmalloc(&serdev->dev, TX_CIRC_BUF_SIZE, GFP_KERNEL);

  serdev_device_set_drvdata(serdev, bcf_greybus);
  serdev_device_set_client_ops(serdev, &bcf_greybus_ops);

  ret = serdev_device_open(serdev);
  if (ret) {
    dev_err(&bcf_greybus->serdev->dev, "Unable to Open Device");
    return ret;
  }

  speed = serdev_device_set_baudrate(serdev, speed);
  dev_dbg(&bcf_greybus->serdev->dev, "Using baudrate %u\n", speed);

  serdev_device_set_flow_control(serdev, false);

  hello_world(bcf_greybus);

  dev_info(&bcf_greybus->serdev->dev, "Successful Probe %s\n",
           BCF_GREYBUS_DRV_NAME);

  return 0;
}

static void bcf_greybus_remove(struct serdev_device *serdev) {
  struct bcf_greybus *bcf_greybus = serdev_device_get_drvdata(serdev);

  dev_info(&bcf_greybus->serdev->dev, "Remove Driver\n");

  flush_work(&bcf_greybus->tx_work);
  serdev_device_close(serdev);
}

static struct serdev_device_driver bcfserial_driver = {
    .probe = bcf_greybus_probe,
    .remove = bcf_greybus_remove,
    .driver =
        {
            .name = BCF_GREYBUS_DRV_NAME,
            .of_match_table = of_match_ptr(bcf_greybus_of_match),
        },
};

module_serdev_device_driver(bcfserial_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ayush Singh <ayushdevel1325@gmail.com>");
MODULE_DESCRIPTION("A Greybus driver for BeaglePlay");
MODULE_VERSION(BCF_GREYBUS_DRV_VERSION);
