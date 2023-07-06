#include "beagleplay-greybus-driver.h"
#include <linux/tty_driver.h>
#include <linux/crc-ccitt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/serdev.h>
#include <linux/tty.h>

#define DEBUG

#define BEAGLEPLAY_GREYBUS_DRV_VERSION "0.1.0"
#define BEAGLEPLAY_GREYBUS_DRV_NAME "bcfgreybus"
#define TX_CIRC_BUF_SIZE 1024

#define HDLC_FRAME 0x7E
#define HDLC_ESC 0x7D
#define HDLC_XOR 0x20

#define ADDRESS_GREYBUS 0x01
#define ADDRESS_DBG 0x02

#define MAX_RX_HDLC (1 + RX_HDLC_PAYLOAD + CRC_LEN)
#define RX_HDLC_PAYLOAD 140
#define CRC_LEN 2

static const struct of_device_id beagleplay_greybus_of_match[] = {
    {
        .compatible = "beagle,bcfserial",
    },
    {},
};
MODULE_DEVICE_TABLE(of, beagleplay_greybus_of_match);

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

static void
bcfserial_append_tx_frame(struct beagleplay_greybus *beagleplay_greybus) {
  beagleplay_greybus->tx_crc = 0xFFFF;
  beagleplay_greybus_append(beagleplay_greybus, HDLC_FRAME);
}

static void
beagleplay_greybus_append_tx_u8(struct beagleplay_greybus *beagleplay_greybus,
                                u8 value) {
  beagleplay_greybus->tx_crc = crc_ccitt(beagleplay_greybus->tx_crc, &value, 1);
  beagleplay_greybus_append(beagleplay_greybus, value);
}

static void beagleplay_greybus_append_tx_buffer(
    struct beagleplay_greybus *beagleplay_greybus, const void *buffer,
    size_t len) {
  size_t i;
  for (i = 0; i < len; i++) {
    beagleplay_greybus_append_tx_u8(beagleplay_greybus, ((u8 *)buffer)[i]);
  }
}

static void
beagleplay_greybus_append_escaped(struct beagleplay_greybus *beagleplay_greybus,
                                  u8 value) {
  if (value == HDLC_FRAME || value == HDLC_ESC) {
    beagleplay_greybus_append(beagleplay_greybus, HDLC_ESC);
    value ^= HDLC_XOR;
  }
  beagleplay_greybus_append(beagleplay_greybus, value);
}

static void
beagleplay_append_tx_crc(struct beagleplay_greybus *beagleplay_greybus) {
  beagleplay_greybus->tx_crc ^= 0xffff;
  beagleplay_greybus_append_escaped(beagleplay_greybus,
                                    beagleplay_greybus->tx_crc & 0xff);
  beagleplay_greybus_append_escaped(beagleplay_greybus,
                                    (beagleplay_greybus->tx_crc >> 8) & 0xff);
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

static void
beagleplay_greybus_hdlc_send(struct beagleplay_greybus *beagleplay_greybus,
                             u16 length, const void *buffer, u8 address,
                             u8 control) {
  // HDLC_FRAME
  // 0 address : 0x01
  // 1 control : 0x03
  // contents
  // x/y crc
  // HDLC_FRAME

  spin_lock(&beagleplay_greybus->tx_producer_lock);

  bcfserial_append_tx_frame(beagleplay_greybus);
  beagleplay_greybus_append_tx_u8(beagleplay_greybus,
                                  address);                     // address
  beagleplay_greybus_append_tx_u8(beagleplay_greybus, control); // control
  beagleplay_greybus_append_tx_buffer(beagleplay_greybus, buffer, length);
  beagleplay_append_tx_crc(beagleplay_greybus);
  bcfserial_append_tx_frame(beagleplay_greybus);

  spin_unlock(&beagleplay_greybus->tx_producer_lock);

  schedule_work(&beagleplay_greybus->tx_work);
}

static int beagleplay_greybus_tty_receive(struct serdev_device *serdev,
                                          const unsigned char *data,
                                          size_t count) {
  struct beagleplay_greybus *beagleplay_greybus;
  u16 crc_check = 0;
  size_t i;
  u8 c;

  beagleplay_greybus = serdev_device_get_drvdata(serdev);

  for (i = 0; i < count; ++i) {
    c = data[i];
    if (c == HDLC_FRAME) {
      if (beagleplay_greybus->rx_address != 0xFF) {
        crc_check = crc_ccitt(0xffff, &beagleplay_greybus->rx_address, 1);
        crc_check = crc_ccitt(crc_check, beagleplay_greybus->rx_buffer,
                              beagleplay_greybus->rx_offset);

        if (crc_check == 0xf0b8) {
          if ((beagleplay_greybus->rx_buffer[0] & 1) == 0) {
            // I-Frame, send S-Frame ACK
            beagleplay_greybus_hdlc_send(
                beagleplay_greybus, 0, NULL, beagleplay_greybus->rx_address,
                (beagleplay_greybus->rx_buffer[0] >> 1) & 0x7);
          }

          if (beagleplay_greybus->rx_address == ADDRESS_DBG) {
            beagleplay_greybus->rx_buffer[beagleplay_greybus->rx_offset - 2] =
                0;
            dev_dbg(&beagleplay_greybus->serdev->dev, "Frame Data: %s\n",
                     beagleplay_greybus->rx_buffer + 1);
          }
        } else {
          dev_err(&beagleplay_greybus->serdev->dev,
                  "CRC Failed from %02x: 0x%04x\n",
                  beagleplay_greybus->rx_address, crc_check);
        }
      }
      beagleplay_greybus->rx_offset = 0;
      beagleplay_greybus->rx_address = 0xFF;
    } else if (c == HDLC_ESC) {
      beagleplay_greybus->rx_in_esc = 1;
    } else {
      if (beagleplay_greybus->rx_in_esc) {
        c ^= 0x20;
        beagleplay_greybus->rx_in_esc = 0;
      }

      if (beagleplay_greybus->rx_address == 0xFF) {
        beagleplay_greybus->rx_address = c;
        if (beagleplay_greybus->rx_address == ADDRESS_DBG) {
        } else {
          beagleplay_greybus->rx_address = 0xFF;
        }
        beagleplay_greybus->rx_offset = 0;
      } else {
        if (beagleplay_greybus->rx_offset < MAX_RX_HDLC) {
          beagleplay_greybus->rx_buffer[beagleplay_greybus->rx_offset] = c;
          beagleplay_greybus->rx_offset++;
        } else {
          // buffer overflow
          dev_err(&beagleplay_greybus->serdev->dev, "RX Buffer Overflow\n");
          beagleplay_greybus->rx_address = 0xFF;
          beagleplay_greybus->rx_offset = 0;
        }
      }
    }
  }

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

static void beagleplay_greybus_uart_transmit(struct work_struct *work) {
  struct beagleplay_greybus *beagleplay_greybus =
      container_of(work, struct beagleplay_greybus, tx_work);

  dev_info(&beagleplay_greybus->serdev->dev, "Write to tx buffer");
  spin_lock_bh(&beagleplay_greybus->tx_consumer_lock);
  beagleplay_greybus_serdev_write_locked(beagleplay_greybus);
  spin_unlock_bh(&beagleplay_greybus->tx_consumer_lock);
}

// A simple function to write "HelloWorld" over UART.
// TODO: Remove in the future.
static void hello_world(struct beagleplay_greybus *beagleplay_greybus) {
  const char msg[] = "Hello From Linux\0";
  const size_t msg_len = strlen(msg);

  beagleplay_greybus_hdlc_send(beagleplay_greybus, msg_len, msg,
                               ADDRESS_GREYBUS, 0x03);
  dev_info(&beagleplay_greybus->serdev->dev, "Written Hello World");
};

static int mcutty_open(struct tty_struct *tty, struct file *file) {
  dev_info(tty->dev, "MCUTTY Open Called");
  return 0;
}

static void mcutty_close(struct tty_struct *tty, struct file *file) {
  dev_info(tty->dev, "MCUTTY Close Called");
}

static int mcutty_write(struct tty_struct *tty, const unsigned char *buf, int count) {
  dev_info(tty->dev, "MCUTTY Write Called with %s", buf);
  return count;
}

static int mcutty_write_room(struct tty_struct *tty) {
  dev_info(tty->dev, "MCUTTY Write Room Called");
  return 0;
}

static void mcutty_set_termios(struct tty_struct *tty, struct ktermios *old) {
  dev_info(tty->dev, "MCUTTY Set Termios Called");
}

static struct tty_operations mcu_ops = {
  .open = mcutty_open,
  .close = mcutty_close,
  .write = mcutty_write,
  .write_room = mcutty_write_room,
  .set_termios = mcutty_set_termios
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

  beagleplay_greybus->rx_buffer =
      devm_kmalloc(&serdev->dev, MAX_RX_HDLC, GFP_KERNEL);
  beagleplay_greybus->rx_offset = 0;
  beagleplay_greybus->rx_address = 0xff;
  beagleplay_greybus->rx_in_esc = 0;

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

  // Init MCUmgr TTY
  beagleplay_greybus->mcumgr_tty = tty_alloc_driver(1, TTY_DRIVER_DYNAMIC_DEV | TTY_DRIVER_REAL_RAW);
  if (beagleplay_greybus->mcumgr_tty == NULL) {
    dev_err(&beagleplay_greybus->serdev->dev, "Unable to allocate MCUmgr TTY");
    return -ENOMEM;
  }
  beagleplay_greybus->mcumgr_tty_port = devm_kmalloc(&serdev->dev, sizeof(struct tty_port), GFP_KERNEL);
  if (beagleplay_greybus->mcumgr_tty_port == NULL) {
    dev_err(&beagleplay_greybus->serdev->dev, "Unable to allocate MCUmgr TTY Port");
    return -ENOMEM;
  }
  tty_port_init(beagleplay_greybus->mcumgr_tty_port);
  beagleplay_greybus->mcumgr_tty->driver_name = "cc1352_mcumgr_tty";
  beagleplay_greybus->mcumgr_tty->name = "ttyMCU";
  beagleplay_greybus->mcumgr_tty->driver_state = beagleplay_greybus;
  beagleplay_greybus->mcumgr_tty->type = TTY_DRIVER_TYPE_SERIAL;
  tty_set_operations(beagleplay_greybus->mcumgr_tty, &mcu_ops);
  ret = tty_register_driver(beagleplay_greybus->mcumgr_tty);
  if (ret) {
    dev_err(&beagleplay_greybus->serdev->dev, "Unable to register TTY driver");
    return ret;
  }

  tty_port_register_device(beagleplay_greybus->mcumgr_tty_port, beagleplay_greybus->mcumgr_tty, 0, &serdev->dev);

  dev_info(&beagleplay_greybus->serdev->dev, "Successful Probe\n");

  hello_world(beagleplay_greybus);

  return 0;
}

static void beagleplay_greybus_remove(struct serdev_device *serdev) {
  struct beagleplay_greybus *beagleplay_greybus =
      serdev_device_get_drvdata(serdev);

  dev_info(&beagleplay_greybus->serdev->dev, "Remove Driver\n");

  // Free tty stuff
  tty_unregister_device(beagleplay_greybus->mcumgr_tty, 0);
  tty_unregister_driver(beagleplay_greybus->mcumgr_tty);
  tty_driver_kref_put(beagleplay_greybus->mcumgr_tty);
  tty_port_destroy(beagleplay_greybus->mcumgr_tty_port);

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
