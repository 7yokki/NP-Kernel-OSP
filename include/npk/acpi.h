#ifndef NPK_ACPI_H
#define NPK_ACPI_H

#include "types.h"

void acpi_init(void);
void acpi_irq_dispatch(void);
void acpi_poll(void);
bool acpi_available(void);
uint8_t acpi_sci_irq(void);
NPK_NORETURN void acpi_poweroff(void);

#endif
