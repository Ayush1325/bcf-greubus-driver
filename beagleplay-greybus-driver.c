#include <linux/circ_buf.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/serdev.h>

#define DEBUG

#define BEAGLEPLAY_GREYBUS_DRV_VERSION "0.1.0"
#define BEAGLEPLAY_GREYBUS_DRV_NAME "bcfgreybus"
#define TX_CIRC_BUF_SIZE 1024

struct beagleplay_greybus {
  struct serdev_device *serdev;

  struct work_struct tx_work;
  spinlock_t tx_producer_lock;
  spinlock_t tx_consumer_lock;
  struct circ_buf tx_circ_buf;
};

static const struct of_device_id beagleplay_greybus_of_match[] = {
    {
        .compatible = "beagle,bcfserial",
    },
    {},
};
MODULE_DEVICE_TABLE(of, beagleplay_greybus_of_match);

static int beagleplay_greybus_tty_receive(struct serdev_device *serdev,
                                          const unsigned char *data,
                                          size_t count) {
  struct beagleplay_greybus *beagleplay_greybus;

  beagleplay_greybus = serdev_device_get_drvdata(serdev);
  dev_info(&beagleplay_greybus->serdev->dev, "tty recieve\n");
  dev_info(&beagleplay_greybus->serdev->dev, "Data: %s\n", data);

  return count;
}

static void beagleplay_greybus_tty_wakeup(struct serdev_device *serdev) {
  struct beagleplay_greybus *beagleplay_greybus;

  beagleplay_greybus = serdev_device_get_drvdata(serdev);
  dev_info(&beagleplay_greybus->serdev->dev, "tty wakeup\n");

  schedule_work(&beagleplay_greybus->tx_work);
}

static struct serdev_device_ops beagleplay_greybus_ops = {
    .receive_buf = beagleplay_greybus_tty_receive,
    .write_wakeup = beagleplay_greybus_tty_wakeup,
};

static void
beagleplay_greybus_append(struct beagleplay_greybus *beagleplay_greybus,
                          u8 value) {
  // must be locked already
  int head = beagleplay_greybus->tx_circ_buf.head;

  while (true) {
    int tail = READ_ONCE(beagleplay_greybus->tx_circ_buf.tail);

    if (CIRC_SPACE(head, tail, TX_CIRC_BUF_SIZE) >= 1) {

      beagleplay_greybus->tx_circ_buf.buf[head] = value;

      smp_store_release(&(beagleplay_greybus->tx_circ_buf.head),
                        (head + 1) & (TX_CIRC_BUF_SIZE - 1));
      return;
    } else {
      dev_dbg(&beagleplay_greybus->serdev->dev, "Tx circ buf full\n");
      usleep_range(3000, 5000);
    }
  }
}

static void beagleplay_greybus_serdev_write_locked(
    struct beagleplay_greybus *beagleplay_greybus) {
  // must be locked already
  int head = smp_load_acquire(&beagleplay_greybus->tx_circ_buf.head);
  int tail = beagleplay_greybus->tx_circ_buf.tail;
  int count = CIRC_CNT_TO_END(head, tail, TX_CIRC_BUF_SIZE);
  int written;

  if (count >= 1) {
    written = serdev_device_write_buf(
        beagleplay_greybus->serdev, &beagleplay_greybus->tx_circ_buf.buf[tail],
        count);
    dev_info(&beagleplay_greybus->serdev->dev, "Written Data of Len: %u\n",
             written);

    smp_store_release(&(beagleplay_greybus->tx_circ_buf.tail),
                      (tail + written) & (TX_CIRC_BUF_SIZE - 1));
  }
}

static void beagleplay_greybus_uart_transmit(struct work_struct *work) {
  struct beagleplay_greybus *beagleplay_greybus =
      container_of(work, struct beagleplay_greybus, tx_work);

  spin_lock_bh(&beagleplay_greybus->tx_consumer_lock);
  dev_info(&beagleplay_greybus->serdev->dev, "Write to tx buffer");
  beagleplay_greybus_serdev_write_locked(beagleplay_greybus);
  spin_unlock_bh(&beagleplay_greybus->tx_consumer_lock);
}

// A simple function to write "HelloWorld" over UART.
// TODO: Remove in the future.
static void hello_world(struct beagleplay_greybus *beagleplay_greybus) {
  const char msg[] = "HelloWorld\n\0";
  const ssize_t msg_len = strlen(msg);
  ssize_t i;

  spin_lock(&beagleplay_greybus->tx_producer_lock);
  for (i = 0; i < msg_len; ++i) {
    beagleplay_greybus_append(beagleplay_greybus, msg[i]);
  }
  spin_unlock(&beagleplay_greybus->tx_producer_lock);

  spin_lock(&beagleplay_greybus->tx_consumer_lock);
  beagleplay_greybus_serdev_write_locked(beagleplay_greybus);
  spin_unlock(&beagleplay_greybus->tx_consumer_lock);

  dev_info(&beagleplay_greybus->serdev->dev, "Written Hello World");
};

static int beagleplay_greybus_probe(struct serdev_device *serdev) {
  u32 speed = 115200;
  int ret = 0;

  struct beagleplay_greybus *beagleplay_greybus =
      devm_kmalloc(&serdev->dev, sizeof(struct beagleplay_greybus), GFP_KERNEL);

  beagleplay_greybus->serdev = serdev;

  INIT_WORK(&beagleplay_greybus->tx_work, beagleplay_greybus_uart_transmit);
  spin_lock_init(&beagleplay_greybus->tx_producer_lock);
  spin_lock_init(&beagleplay_greybus->tx_consumer_lock);
  beagleplay_greybus->tx_circ_buf.head = 0;
  beagleplay_greybus->tx_circ_buf.tail = 0;
  beagleplay_greybus->tx_circ_buf.buf =
      devm_kmalloc(&serdev->dev, TX_CIRC_BUF_SIZE, GFP_KERNEL);

  serdev_device_set_drvdata(serdev, beagleplay_greybus);
  serdev_device_set_client_ops(serdev, &beagleplay_greybus_ops);

  ret = serdev_device_open(serdev);
  if (ret) {
    dev_err(&beagleplay_greybus->serdev->dev, "Unable to Open Device");
    return ret;
  }

  speed = serdev_device_set_baudrate(serdev, speed);
  dev_dbg(&beagleplay_greybus->serdev->dev, "Using baudrate %u\n", speed);

  serdev_device_set_flow_control(serdev, false);

  dev_info(&beagleplay_greybus->serdev->dev, "Successful Probe\n");

  return 0;
}

static void beagleplay_greybus_remove(struct serdev_device *serdev) {
  struct beagleplay_greybus *beagleplay_greybus =
      serdev_device_get_drvdata(serdev);

  dev_info(&beagleplay_greybus->serdev->dev, "Remove Driver\n");

  flush_work(&beagleplay_greybus->tx_work);
  serdev_device_close(serdev);
}

static struct serdev_device_driver beagleplay_greybus_driver = {
    .probe = beagleplay_greybus_probe,
    .remove = beagleplay_greybus_remove,
    .driver =
        {
            .name = BEAGLEPLAY_GREYBUS_DRV_NAME,
            .of_match_table = of_match_ptr(beagleplay_greybus_of_match),
        },
};

module_serdev_device_driver(beagleplay_greybus_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ayush Singh <ayushdevel1325@gmail.com>");
MODULE_DESCRIPTION("A Greybus driver for BeaglePlay");
MODULE_VERSION(BEAGLEPLAY_GREYBUS_DRV_VERSION);
