/*
 * FingerVU touch and IR/keys driver
 *
 * Copyright (C) 2013 Wolfgang Astleitner (mrwastl@users.sourceforge.net)
 *
 *   Heavily based on the imon.c,
 *   Copyright(C) 2010  Jarod Wilson <jarod@wilsonet.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 *
*/

#define DEBUG
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/timer.h>

#define DRIVER_VERSION "v0.3"
#define DRIVER_AUTHOR  "Wolfgang Astleitner (mrwastl@users.sourceforge.net)"
#define DRIVER_DESC    "SoundGraph FingerVU touch and IR/Keys/RC driver"
#define DRIVER_NAME    "fingervu"

/* to prevent races between open() and disconnect(), probing, etc */
static DEFINE_MUTEX(driver_lock);

static bool debug = 0;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debug messages: 0=no, 1=yes (default: no)");

static bool mirror_x = 0;
module_param(mirror_x, bool, 0644);
MODULE_PARM_DESC(mirror_x, "Mirror X axis: 0=no, 1=yes (default: no)");

static bool mirror_y = 0;
module_param(mirror_y, bool, 0644);
MODULE_PARM_DESC(mirror_y, "Mirror Y axis: 0=no, 1=yes (default: no)");

static bool disable_touch = 0;
module_param(disable_touch, bool, 0644);
MODULE_PARM_DESC(disable_touch, "Disable touchscreen: 0=no, 1=yes (default: no)");

static bool disable_idev = 0;
module_param(disable_idev, bool, 0644);
MODULE_PARM_DESC(disable_idev, "Disable input device: 0=no, 1=yes (default: no)");

static unsigned int repeat_delay = 660;
module_param(repeat_delay, uint, 0644);
MODULE_PARM_DESC(repeat_delay, "Delay after which a repeated key event will be generated (default: 660ms)");

static unsigned int repeat_rate = 10;
module_param(repeat_rate, uint, 0644);
MODULE_PARM_DESC(repeat_rate, "Max. repeats per seconds (default: 10 repeats/s)");

/* table of devices that work with this driver */
static struct usb_device_id id_table [] = {
  /* Soundgraph display in Thermaltake DH202 chassis */
  { USB_DEVICE(0x15c2, 0x30c0) },
  /* FingerVU 706 */
  { USB_DEVICE(0x15c2, 0x3480) },
  { },
};

/* internal flags */
static int flag_warned_urb = 0;
static int flag_warned_epipe = 0;

/* main driver context */
/*static*/ struct fingervu_context {
  struct device *dev;

  struct usb_device* udev;

  struct usb_device *usbdev_intf[4];

  bool dev_present_intf[3];   /* USB device presence, interface 0 */

  struct usb_endpoint_descriptor *rx_endpoint_intf[3];

  struct mutex lock;          /* to lock this object */
  spinlock_t kc_lock;         /* make sure we get keycodes right */

  struct urb *rx_urb_intf[3];
  unsigned char usb_rx_buf[64];

  u16 vendor;                 /* usb vendor ID */
  u16 product;                /* usb product ID */

  struct input_dev *idev;     /* input device for panel & IR mouse */
  struct input_dev *touch;    /* input device for touchscreen */

  char name_idev[128];        /* input device name */
  char phys_idev[64];         /* input device phys path */
  struct timer_list ktimer;   /* touch screen timer */
  u32 key_pending;            /* keycode waiting for timer to fire down event */
  int key_repeated;           /* has current keycode already been repeated at least once ? */
  struct timespec prev_event; /* timestamp of previous reported key event */
  u32 prev_key;               /* previous reported key event */
  u16 repeat_rate_ms;         /* precalculated repeated in min delay between two events in ms ( = 1000ms / repeat_rate ) */
  

  char name_touch[128];       /* touch screen name */
  char phys_touch[64];        /* touch screen phys path */
  struct timer_list ttimer;   /* touch screen timer */
  int touch_x;                /* x coordinate on touchscreen */
  int touch_y;                /* y coordinate on touchscreen */
  int touch_active;           /* touch down occured but no up from timer yet? */
};


/* imon receiver front panel/knob key table */
static const struct {
  u32 hw_code;
  u32 keycode;
  unsigned char flags;
} fingervu_panel_key_table[] = {
  /* 8 byte raw code: XX 00 YY 00 00 00 00 00  */
  /*  0400XXYY                                 */
  /* 64byte raw code: 41 02 ed aX XY Y4 00 ... */
  /*  0800XXYY                                 */
  { 0x04000028, KEY_ENTER,       0 },
  { 0x0400002a, KEY_EXIT,        0 },
  { 0x04000065, KEY_MENU,        0 },
  { 0x08008891, KEY_POWER,       0 },
  { 0x08009811, KEY_CLOSE,       0 },
  { 0x08008a55, KEY_UP,          0 },
  { 0x08008a91, KEY_DOWN,        0 },
  { 0x08008a59, KEY_LEFT,        0 },
  { 0x08008a5d, KEY_RIGHT,       0 },
  { 0x08009a9d, KEY_MUTE,        0 },
  { 0x08009c51, KEY_VOLUMEUP,    1 },
  { 0x08009a51, KEY_VOLUMEDOWN,  1 },
  { 0x08009c91, KEY_CHANNELUP,   0 },
  { 0x08009e11, KEY_CHANNELDOWN, 0 },
  { 0x08008c19, KEY_PREVIOUS,    0 },
  { 0x08008c1d, KEY_NEXT,        0 },
  { 0x08008c15, KEY_PLAYPAUSE,   0 },
  { 0x080098d5, KEY_PROGRAM,     0 },

  { 0x04000027, KEY_0,           0 },
  { 0x0400001e, KEY_1,           0 },
  { 0x0400001f, KEY_2,           0 },
  { 0x04000020, KEY_3,           0 },
  { 0x04000021, KEY_4,           0 },
  { 0x04000022, KEY_5,           0 },
  { 0x04000023, KEY_6,           0 },
  { 0x04000024, KEY_7,           0 },
  { 0x04000025, KEY_8,           0 },
  { 0x04000026, KEY_9,           0 },
  { 0x08008a1d, KEY_RED,         0 },
  { 0x08009899, KEY_GREEN,       0 },
  { 0x08008a51, KEY_YELLOW,      0 },
  { 0x0800885d, KEY_BLUE,        0 },
  { 0x08009815, KEY_REWIND,      0 },
  { 0x0800881d, KEY_FORWARD,     0 },
  { 0x08008895, KEY_PAUSE,       0 },
  { 0x08008819, KEY_RECORD,      0 },
  { 0x08008815, KEY_PLAY,        0 },
  { 0x0800889d, KEY_PREVIOUS,    0 },
  { 0x08009819, KEY_NEXT,        0 },
  { 0x08008e9d, KEY_STOP,        0 },
  { 0x080098d9, KEY_EJECTCD,     0 },
  /* knob/panel command: 50 XX YY 00 ... */
  /*  0500XXYY                           */
  { 0x0500022a, KEY_VOLUMEDOWN,  1 }, /* knob left */
  { 0x05000228, KEY_VOLUMEUP,    1 }, /* knob right */
  { 0x0500020a, KEY_VOLUMEDOWN,  1 }, /* knob left alt */
  { 0x05000208, KEY_VOLUMEUP,    1 }, /* knob right alt */
  { 0x05000101, KEY_MUTE,        0 }, /* knob press */
  { 0x0500010f, KEY_MEDIA,       0 }, /* panel iMedian */
  { 0x0500012b, KEY_EXIT,        0 }, /* panel app exit */
  { 0x05000117, KEY_ESC,         0 }, /* panel esc */
  { 0x05000112, KEY_UP,          0 }, /* panel up */
  { 0x05000116, KEY_ENTER,       0 }, /* panel enter */
  { 0x0500012c, KEY_SELECT,      0 }, /* panel start */
  { 0x0500012d, KEY_MENU,        0 }, /* panel menu */
  { 0x05000114, KEY_LEFT,        0 }, /* panel left */
  { 0x05000113, KEY_DOWN,        0 }, /* panel down */
  { 0x05000115, KEY_RIGHT,       0 }, /* panel right */
};

#define TOUCH_TIMEOUT (HZ/30)
#define KEYEV_TIMEOUT (HZ/30)


/* function prototypes */
static int               fingervu_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void              fingervu_disconnect(struct usb_interface *interface);
static struct input_dev* fingervu_init_touch(struct fingervu_context *context);
static struct input_dev* fingervu_init_idev(struct fingervu_context *context);
static void              usb_rx_callback(struct urb *urb);
static u32               find_keycode(u32 scancode, unsigned char*);





static int fingervu_probe(struct usb_interface *interface, const struct usb_device_id *id) {
  struct usb_device *usbdev = interface_to_usbdev(interface);
  struct fingervu_context *context = NULL;
  struct usb_host_interface *iface_desc = NULL;
  struct usb_endpoint_descriptor *ep;
  int ret = 0;
  int ifnum;
  u16 vendor, product;
  bool success = 1;

  iface_desc = interface->cur_altsetting;
  ifnum      = iface_desc->desc.bInterfaceNumber;
  vendor     = le16_to_cpu(usb_get_dev(usbdev)->descriptor.idVendor);
  product    = le16_to_cpu(usb_get_dev(usbdev)->descriptor.idProduct);
  ep         = &iface_desc->endpoint[0].desc;


  if (debug) {
    dev_info(
      &interface->dev, "%s: found FingerVu device (%04x:%04x, ifnum=%d, ep=%02x)\n",
      __func__, vendor, product, ifnum, ep->bEndpointAddress
    );
  }

  /* stuff that needs to be done only once */
  if (ifnum == 0) {
    context = kmalloc(sizeof(struct fingervu_context), GFP_KERNEL);
    if (context == NULL) {
      dev_err(&interface->dev, "Unable to get memory for FingerVU struct\n");
      return -ENOMEM;
    }
    memset (context, 0x00, sizeof (*context));

    context->dev = &interface->dev;
    context->udev = usb_get_dev(usbdev);
    context->vendor = vendor;
    context->product = product;

    mutex_init(&context->lock);
    spin_lock_init(&context->kc_lock);
  } else {
    context = usb_get_intfdata (usb_ifnum_to_if(usbdev, 0));
    if (context == NULL) {
      dev_err(&interface->dev, "Unable to get FingerVU context (ifnum=%d)\n", ifnum);
      return -ENOMEM;
    }
  }

  /* prevent races probing devices w/multiple interfaces */
  mutex_lock(&driver_lock);

  /* assign context to current interface */
  usb_set_intfdata (interface, context);

  context->usbdev_intf[ifnum] = usb_get_dev(usbdev);

  context->rx_endpoint_intf[ifnum] = ep;

  /* pre-init with no success */
  context->dev_present_intf[ifnum] = false;
  success = false;

  /* initialise interface */
  if ( (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN ) {
    struct urb* rx_urb;
    rx_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (! rx_urb) {
      dev_err(&interface->dev, "usb_alloc_urb failed for ifnum=%d, err %d", ifnum, ret);
    } else {
      mutex_lock(&context->lock);

      context->rx_urb_intf[ifnum] = rx_urb;

      if (ep->bEndpointAddress == 0x83) {
        if ( ! disable_touch ) {
          context->touch = fingervu_init_touch(context);
        }
        if (! disable_idev ) {
          context->idev = fingervu_init_idev(context);
        }

      }

      usb_fill_int_urb(
        context->rx_urb_intf[ifnum],
        context->usbdev_intf[ifnum],
        usb_rcvintpipe( context->usbdev_intf[ifnum], context->rx_endpoint_intf[ifnum]->bEndpointAddress),
        context->usb_rx_buf, ((ep->bEndpointAddress == 0x83) ? sizeof(context->usb_rx_buf) : 8),
        usb_rx_callback,
        context,
        context->rx_endpoint_intf[ifnum]->bInterval
      );

      ret = usb_submit_urb(context->rx_urb_intf[ifnum], GFP_KERNEL);
      if (ret) {
        dev_err(&interface->dev, "usb_submit_urb failed for ifnum=%d, err %d", ifnum, ret);
      }

      mutex_unlock(&context->lock);

      context->dev_present_intf[ifnum] = true;
      success = true;
    }
  }

  mutex_unlock(&driver_lock);

  if (success) {
    dev_info(&interface->dev, "FingerVU device now attached\n");
  } else {
    ret = -ENODEV;
    usb_put_dev(context->udev);
    dev_err(&interface->dev, "Unable to register FingerVU device, intf %d, err %d\n", ifnum, ret);
  }

  return ret;
}


/**
 * Disconnect current interface and clean up allocated structures and items if last interface
 */
static void fingervu_disconnect(struct usb_interface *interface) {
  int ifnum = interface->cur_altsetting->desc.bInterfaceNumber;
  struct usb_host_interface *iface_desc = interface->cur_altsetting;
  struct usb_endpoint_descriptor *ep = &iface_desc->endpoint[0].desc;

  struct fingervu_context *context;
  int i, present = false;


  /* prevent races with multi-interface device probing and display_open */
  mutex_lock(&driver_lock);

  context = usb_get_intfdata (interface);

  /* remove context from current interface */
  usb_set_intfdata (interface, NULL);

  context->rx_endpoint_intf[ifnum] = NULL;

  if (context->dev_present_intf[ifnum]) {
    context->dev_present_intf[ifnum] = false;

    if (ep->bEndpointAddress == 0x83) {
      if (context->touch) {
        del_timer_sync(&context->ttimer);
        input_unregister_device(context->touch);
        context->touch = NULL;
      }
      if (context->idev) {
        del_timer_sync(&context->ktimer);
        input_unregister_device(context->idev);
        context->idev = NULL;
      }
    }
    usb_kill_urb(context->rx_urb_intf[ifnum]);
    usb_free_urb(context->rx_urb_intf[ifnum]);
  }

  i = 0;
  while (!present && i < 3) {
    present = context->dev_present_intf[i];
    i++;
  }
  if (!present && ! context->touch && ! context->idev) {
    kfree(context);
  }

  mutex_unlock(&driver_lock);
  dev_info(&interface->dev, "FingerVU device now disconnected\n");
}



/**
 * Callback function for touch timeout
 */
static void fingervu_touch_timeout(unsigned long data) {
  struct fingervu_context *context = (struct fingervu_context *)data;
  if (context->touch && context->touch_active) {
    context->touch_active = 0;
    input_report_abs(context->touch, ABS_X, context->touch_x);
    input_report_abs(context->touch, ABS_Y, context->touch_y);
    input_report_key(context->touch, BTN_TOUCH, 0x00);
    input_sync(context->touch);
  }
}



static void fingervu_key_timeout(unsigned long data) {
  struct fingervu_context *context = (struct fingervu_context *)data;
  if (context->idev && context->key_pending != KEY_RESERVED) {
    input_report_key(context->idev, context->key_pending, 0x00);
    input_sync(context->idev);
    context->key_pending = KEY_RESERVED;
  }
}


static u32 find_keycode(u32 scancode, unsigned char* flags) {
  int i = 0;
  int l = ARRAY_SIZE(fingervu_panel_key_table);
  while (i < l) {
    if (fingervu_panel_key_table[i].hw_code == scancode) {
      *flags = fingervu_panel_key_table[i].flags;
      return fingervu_panel_key_table[i].keycode;
    }
    i++; 
  }
  *flags = 0;
  return KEY_RESERVED;
}

/**
 * Initialise touch device
 */
static struct input_dev* fingervu_init_touch(struct fingervu_context *context) {
  struct input_dev *touch;
  int ret;

  touch = input_allocate_device();
  if (!touch) {
    dev_err(context->dev, "touchscreen input dev allocation failed\n");
    return NULL;
  }

  snprintf(context->name_touch, sizeof(context->name_touch),
    "SoundGraph Touchscreen (%04x:%04x)",
    context->vendor, context->product);
  touch->name = context->name_touch;

  usb_make_path(context->usbdev_intf[0], context->phys_touch,
          sizeof(context->phys_touch));
  strlcat(context->phys_touch, "/input0", sizeof(context->phys_touch));
  touch->phys = context->phys_touch;

  touch->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
  touch->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
  input_set_abs_params(touch, ABS_X, 0x00, 0xffff, 0, 0);
  input_set_abs_params(touch, ABS_Y, 0x00, 0xffff, 0, 0);

  init_timer(&context->ttimer);
  context->ttimer.data = (unsigned long)context;
  context->ttimer.function = fingervu_touch_timeout;

  input_set_drvdata(touch, context);

  usb_to_input_id(context->usbdev_intf[0], &touch->id);
  touch->dev.parent = context->dev;
  ret = input_register_device(touch);
  if (ret <  0) {
    del_timer_sync(&context->ttimer);
    context->ttimer.function = NULL;
    input_free_device(touch);
    dev_info(context->dev, "touchscreen input dev register failed\n");
    return NULL;
  }

  return touch;
}

static struct input_dev* fingervu_init_idev(struct fingervu_context *context) {
  struct input_dev *idev;
  int ret, i;


  idev = input_allocate_device();
  if (!idev) {
    dev_err(context->dev, "input dev allocation failed\n");
    return NULL;
  }

  snprintf(context->name_idev, sizeof(context->name_idev),
    "SoundGraph Input (%04x:%04x)",
    context->vendor, context->product);
  idev->name = context->name_idev;

  usb_make_path(context->usbdev_intf[2], context->phys_idev,
          sizeof(context->phys_idev));
  strlcat(context->phys_idev, "/input1", sizeof(context->phys_idev));
  idev->phys = context->phys_idev;

  idev->evbit[0] = BIT_MASK(EV_KEY) /*| BIT_MASK(EV_REP) | BIT_MASK(EV_REL)*/;

  /* panel and/or knob code support */
  for (i = 0; i < ARRAY_SIZE(fingervu_panel_key_table); i++) {
    u32 kc = fingervu_panel_key_table[i].keycode;
    set_bit(kc, idev->keybit);
  }


  usb_to_input_id(context->usbdev_intf[2], &idev->id);
  idev->dev.parent = context->dev;
  input_set_drvdata(idev, context);

  init_timer(&context->ktimer);
  context->ktimer.data = (unsigned long)context;
  context->ktimer.function = fingervu_key_timeout;

  ret = input_register_device(idev);
  if (ret < 0) {
    del_timer_sync(&context->ktimer);
    context->ktimer.function = NULL;
    dev_err(context->dev, "input dev register failed\n");
    input_free_device(idev);
    return NULL;
  }

  context->prev_key = KEY_RESERVED;
  getnstimeofday(&(context->prev_event));
  context->repeat_rate_ms = 1000 / repeat_rate;   /* min. delay between two events   in ms */
  context->key_repeated = 0;
  
  return idev;
}



/**
 * Process the incoming packet
 */
static void fingervu_incoming_packet(struct fingervu_context *context, struct urb *urb, int ifnum) {
  int len = urb->actual_length;
  unsigned char *buf = urb->transfer_buffer;
  int found = false;
  int delayok = false;
  u32 keycode = KEY_RESERVED;
  int keytype = 0x01;  /* default: pressed */
  unsigned char flags;
  struct timespec curr_time;
  unsigned long  timediff;

  if (len == 64 && buf[0] == 0x32 && buf[1] == 0xf1) { /* tprintk(KERN_INFO  ".");
ouch event */
    if (context->touch) {
      mod_timer(&context->ttimer, jiffies + TOUCH_TIMEOUT);
      context->touch_x = (buf[2] << 8) | buf[3];
      context->touch_y = (buf[4] << 8) | buf[5];
      if (mirror_x) {
        context->touch_x = 0xffff - context->touch_x;
      }
      if (mirror_y) {
        context->touch_y = 0xffff - context->touch_y;
      }
      input_report_abs(context->touch, ABS_X, context->touch_x);
      input_report_abs(context->touch, ABS_Y, context->touch_y);
      input_report_key(context->touch, BTN_TOUCH, 0x01);
      context->touch_active = 1;
      found = true;
    }
  } else if (len == 64 && buf[0] == 0x41 && buf[1] == 0x02 && buf[2] == 0xed) { /* event from interface 2 */
    if (context->idev) {
      u16 rawcode = ((buf[3] & 0x0F) << 12) | (buf[4] << 4) | ((buf[5] & 0xF0 ) >> 4);
      u32 scancode = 0x08000000 | rawcode;
      u32 togglemask = 0x00002000;

      if (! (scancode & togglemask)) {
        keycode = find_keycode(scancode, &flags);
        found = (keycode != KEY_RESERVED);
      } else {
        /* ignore key up event */
//        printk(KERN_INFO "T %08x %s %08x\n", context->rdev->last_scancode,(found) ? "==" : "!=", scancode );
        keycode = find_keycode(scancode, &flags);
        keytype = 0x00;
        found = true;
      }
    }
  } else if (len == 8 && buf[0] == 0 && buf[1] == 0) { /* event from interface 1 */
    if (context->idev) {
      u16 rawcode = (buf[0] << 8) | buf[2];
      u32 scancode = 0x04000000 | rawcode;

      if (rawcode == 0x0000) {
        found = true; /* ignore key up event */
      } else {
        keycode = find_keycode(scancode, &flags);
        found = (keycode != KEY_RESERVED);
      }
    }
  } else if (len == 64 && buf[0] == 0x50) { /* knob/panel */
    if (context->idev) {
      u16 rawcode = buf[1] << 8 | buf[2];
      u32 scancode = 0x05000000 | rawcode;
      
      keycode = find_keycode(scancode, &flags);
      found = (keycode != KEY_RESERVED);
    }
  }

  if (found && keycode != KEY_RESERVED /*&& context->key_pending == KEY_RESERVED*/) {
    int force_norepeat = 0;
    
    del_timer(&context->ktimer);
    getnstimeofday(&curr_time);

    delayok = 1;

    if ( keycode == context->prev_key ) {

      delayok = 0;
      timediff = (curr_time.tv_sec - context->prev_event.tv_sec) * 1000 + ((curr_time.tv_nsec - context->prev_event.tv_nsec) / 1000000);  /* diff in ms */

      if (timediff >= (repeat_delay + context->repeat_rate_ms)) { /* same key, but with interruption */
         force_norepeat = 1;
      }

      if ( ( context->key_repeated || (flags & 0x01) ) && timediff >= context->repeat_rate_ms) {
        delayok = 1;
      } else if (timediff >= repeat_delay) {
        delayok = 1;
      }
    }

    if (delayok) {
      input_report_key(context->idev, keycode, keytype);
      input_sync(context->idev);
      context->key_pending = keycode;
      mod_timer(&context->ktimer, jiffies + KEYEV_TIMEOUT);
      context->prev_event.tv_sec = curr_time.tv_sec;
      context->prev_event.tv_nsec = curr_time.tv_nsec;
      if ( (!force_norepeat) && (keycode == context->prev_key) ) {
        context->key_repeated = 1;   /* same keycode repeated at least once */
      } else {
        context->prev_key = keycode; /* new keycode */
        context->key_repeated = 0;
      }
    }
    //printk(KERN_INFO "sending keycode %08x\n", keycode);
  }

  if (!found && debug) {
    int i;
    printk(KERN_INFO "intf%d decoded packet (len=%d): ", ifnum, len);
    for (i = 0; i < len; ++i)
      printk("%02x ", buf[i]);
    printk("\n");
  }
}

/**
 * Callback function for USB core API: receive data
 */
static void usb_rx_callback(struct urb *urb) {
  struct fingervu_context *context;
  struct usb_endpoint_descriptor *ep = (struct usb_endpoint_descriptor *)urb->ep;
  int ifnum = (ep->bEndpointAddress == 0x82) ? 1 : 2;

  if (!urb) {
    if (debug) {
      printk(KERN_INFO "intf%d: empty urb", ifnum);
    }
    return;
  }

  context = (struct fingervu_context *)urb->context;
  if (!context) {
    usb_unlink_urb(urb);
    return;
  }


  /*
   * if we get a callback before we're done configuring the hardware, we
   * can't yet process the data, as there's nowhere to send it, but we
   * still need to submit a new rx URB to avoid wedging the hardware
   */
  if (context->dev_present_intf[ifnum]) {

    switch (urb->status) {
    case -ECONNRESET:
    case -ENOENT:
    case -ESHUTDOWN: /* usbcore unlink successful! */
      usb_unlink_urb(urb);
      return;

    case -EPIPE:	/*  ? */
      usb_unlink_urb(urb);
      if (flag_warned_epipe == 0 || flag_warned_epipe >= 100) {
        flag_warned_epipe = 0;
        dev_warn(context->dev, "fingervu %s:  -EPIPE occured, next 100 such errors will be ignored\n", __func__);
      }
      flag_warned_epipe ++;
      break;

    case 0:
      fingervu_incoming_packet(context, urb, ifnum);
      flag_warned_urb = 0;
      flag_warned_epipe = 0;
      break;

    default:
      if (!flag_warned_urb) {
        dev_warn(context->dev, "fingervu %s: status(%d): ignored\n", __func__, urb->status);
        flag_warned_urb = 1;
      }
      break;
    }
  }
  usb_submit_urb(context->rx_urb_intf[ifnum], GFP_KERNEL /*GFP_ATOMIC*/);
}


static int fingervu_suspend(struct usb_interface *interface, pm_message_t message) {
  struct fingervu_context *context = usb_get_intfdata(interface);
  int ifnum = interface->cur_altsetting->desc.bInterfaceNumber;


  if (context->rx_urb_intf[ifnum]) {
    if (debug) {
      dev_info(&interface->dev, "%s: suspending FingerVu device (ifnum=%d)\n", __func__, ifnum);
    }
    usb_kill_urb(context->rx_urb_intf[ifnum]);
  }

  return 0;
}

static int fingervu_resume(struct usb_interface *interface) {
  int rc = 0;
  struct fingervu_context *context = usb_get_intfdata(interface);
  int ifnum = interface->cur_altsetting->desc.bInterfaceNumber;
  struct usb_host_interface *iface_desc = interface->cur_altsetting;
  struct usb_endpoint_descriptor *ep = &iface_desc->endpoint[0].desc;

  if (context->rx_urb_intf[ifnum]) {
    if (debug) {
      dev_info(&interface->dev, "%s: resuming FingerVu device (ifnum=%d)\n", __func__, ifnum);
    }
    usb_fill_int_urb(
      context->rx_urb_intf[ifnum], context->usbdev_intf[ifnum],
      usb_rcvintpipe(context->usbdev_intf[ifnum], context->rx_endpoint_intf[ifnum]->bEndpointAddress),
      context->usb_rx_buf, ((ep->bEndpointAddress == 0x83) ? sizeof(context->usb_rx_buf) : 8),
      usb_rx_callback,
      context,
      context->rx_endpoint_intf[ifnum]->bInterval);

    rc = usb_submit_urb(context->rx_urb_intf[ifnum], GFP_ATOMIC);
  }

  return rc;
}



MODULE_DEVICE_TABLE (usb, id_table);

static struct usb_driver fingervu_driver = {
  .name       = DRIVER_NAME,
  .probe      = fingervu_probe,
  .disconnect = fingervu_disconnect,
  .suspend    = fingervu_suspend,
  .resume     = fingervu_resume,
  .id_table   = id_table,
};

module_usb_driver(fingervu_driver);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_VERSION(DRIVER_VERSION);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

