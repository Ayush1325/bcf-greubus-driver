#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static int __init bcf_greybus_init(void) {
  pr_info("Hello World 1.\n");
  return 0;
}

static void __exit bcf_greybus_exit(void) { pr_info("Goodbye World 1.\n"); }

module_init(bcf_greybus_init);
module_exit(bcf_greybus_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ayush Singh <ayushdevel1325@gmail.com>");
MODULE_DESCRIPTION("A Greybus driver for BeaglePlay");
