#pragma once
/* USB core interface between the xHCI host driver and the class drivers */
#include <kernel.h>

/* standard requests */
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09
#define USB_REQ_SET_INTERFACE  0x0B

#define USB_DT_DEVICE    1
#define USB_DT_CONFIG    2
#define USB_DT_STRING    3
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT  5
#define USB_DT_HID       0x21
#define USB_DT_REPORT    0x22

#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

typedef struct PACKED {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_device_desc_t;

typedef struct PACKED {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} usb_config_desc_t;

typedef struct PACKED {
    uint8_t bLength, bDescriptorType;
    uint8_t bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} usb_interface_desc_t;

typedef struct PACKED {
    uint8_t bLength, bDescriptorType;
    uint8_t bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_endpoint_desc_t;

struct usb_device;

typedef struct usb_endpoint {
    struct usb_device *dev;
    uint8_t addr;               /* bEndpointAddress (bit 7 = IN) */
    uint8_t type;               /* 1 isoc, 2 bulk, 3 interrupt */
    uint16_t max_packet;
    uint8_t interval;
    int dci;                    /* xHCI device context index */
    void *ring;                 /* host controller private */
    /* completion of the last transfer */
    volatile bool done;
    volatile uint8_t cc;
    volatile uint32_t residual;
    /* interrupt IN endpoints: called for every completed transfer */
    void (*on_data)(struct usb_endpoint *ep, uint8_t *data, uint32_t len);
    void *driver;
    uint8_t *buf;
    uint64_t buf_phys;
    uint32_t buf_len;
} usb_endpoint_t;

typedef struct usb_device {
    void *hc;
    int slot;
    int port;                   /* port on the parent (root hub port for root devices) */
    int root_port;
    uint32_t route;             /* xHCI route string */
    int depth;                  /* number of hubs between the root and this device */
    struct usb_device *parent;  /* NULL on a root port */
    int speed;
    bool is_hub;
    int hub_ports;
    bool hub_mtt;
    uint8_t hub_ttt;
    bool gone;
    usb_device_desc_t desc;
    uint8_t *config;            /* full configuration descriptor */
    uint16_t config_len;
    char product[64];
    usb_endpoint_t ep0;
    usb_endpoint_t eps[8];
    int neps;
    /* class drivers bound to the interfaces: started once the configuration is active */
    void (*start[4])(struct usb_device *d, void *ctx);
    void (*stop[4])(struct usb_device *d, void *ctx);
    void *ctx[4];
    int nbound;
} usb_device_t;

void usb_bind(usb_device_t *d, void (*start)(usb_device_t *, void *), void (*stop)(usb_device_t *, void *), void *ctx);

/* host controller services */
int usb_control(usb_device_t *d, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, void *data,
                uint16_t len);
int usb_bulk(usb_endpoint_t *ep, void *data, uint32_t len, uint32_t *actual);
bool usb_interrupt_start(usb_endpoint_t *ep, uint32_t len);   /* continuous IN polling */
usb_endpoint_t *usb_add_endpoint(usb_device_t *d, const usb_endpoint_desc_t *e);
int usb_configure_endpoints(usb_device_t *d);
int usb_clear_halt(usb_endpoint_t *ep);

/* class drivers: return true if they took the interface */
bool usb_hid_probe(usb_device_t *d, usb_interface_desc_t *intf, uint8_t *extra, int extra_len);
bool usb_msc_probe(usb_device_t *d, usb_interface_desc_t *intf, uint8_t *extra, int extra_len);
void usb_hid_tick(void);      /* key repeat, called every ~10 ms */

void xhci_init(void);

/* enumeration services for the hub driver */
usb_device_t *usb_enumerate(void *hc, usb_device_t *parent, int port, int speed);
void usb_device_remove(usb_device_t *d);
bool usb_hub_probe(usb_device_t *d, usb_interface_desc_t *intf, uint8_t *extra, int extra_len);
void usb_hub_poll(void);
