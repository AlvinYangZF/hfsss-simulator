#ifndef __HFSSS_HAL_PCI_H
#define __HFSSS_HAL_PCI_H

#include "common/common.h"

/* HAL PCI Context (Placeholder for future implementation) */
struct hal_pci_ctx {
    bool initialized;
};

/* Function Prototypes (Placeholders) */
int hal_pci_init(struct hal_pci_ctx *ctx);
void hal_pci_cleanup(struct hal_pci_ctx *ctx);

#endif /* __HFSSS_HAL_PCI_H */
