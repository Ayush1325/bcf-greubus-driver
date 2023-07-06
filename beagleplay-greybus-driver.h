#ifndef BEAGLEPLAY_GREBUBS_DRIVER_H
#define BEAGLEPLAY_GREBUBS_DRIVER_H

#include <linux/circ_buf.h>
#include <linux/init.h>
#include <linux/workqueue.h>

struct beagleplay_greybus {
  struct serdev_device *serdev;

  struct tty_driver *mcumgr_tty;

  struct work_struct tx_work;
  spinlock_t tx_producer_lock;
  spinlock_t tx_consumer_lock;
  struct circ_buf tx_circ_buf;
  u16 tx_crc;
  u8 tx_ack_seq; /* current TX ACK sequence number */

  u8 rx_address;
  u8 *rx_buffer;
  u16 rx_offset;
  u8 rx_in_esc;
};

#endif
