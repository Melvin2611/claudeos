#pragma once
#include <kernel.h>

typedef struct pci_dev {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, irq;
    uint32_t bar[6];
} pci_dev_t;

void pci_init(void);
int pci_count(void);
pci_dev_t *pci_get(int i);
pci_dev_t *pci_find(uint16_t vendor, uint16_t device);
pci_dev_t *pci_find_class(uint8_t cls, uint8_t sub);
uint32_t pci_read32(pci_dev_t *d, uint8_t off);
void pci_write32(pci_dev_t *d, uint8_t off, uint32_t v);
uint16_t pci_read16(pci_dev_t *d, uint8_t off);
void pci_write16(pci_dev_t *d, uint8_t off, uint16_t v);
void pci_enable_bus_master(pci_dev_t *d);
const char *pci_class_name(uint8_t cls, uint8_t sub);
