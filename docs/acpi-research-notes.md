# ACPI implementation research notes

## Sources

- UEFI Forum, ACPI Specification 6.4, ACPI Hardware Specification: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/04_ACPI_Hardware_Specification/ACPI_Hardware_Specification.html
- UEFI Forum, ACPI Specification 6.4, AML Specification: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/20_AML_Specification/AML_Specification.html
- OSDev FADT overview: https://wiki.osdev.org/FADT
- OSDev ACPI poweroff discussion: https://forum.osdev.org/viewtopic.php?t=16990

## Relevant facts

ACPI soft-off is the OS-initiated G2/S5 transition. The standard PM1 control sequence uses the `_S5_` sleep package from AML: encode the sleep type in the SLP_TYP field and set SLP_EN in PM1a_CNT (and PM1b_CNT when present). Fixed ACPI registers are described by FADT legacy fields and, for newer tables, Generic Address Structure X_* fields. The power button event is represented by PWRBTN_STS/PWRBTN_EN in the PM1 event block and is delivered through the platform’s SCI.

The current NPKernel PIC implementation can route the common legacy SCI IRQ9 path. Hardware-reduced ACPI and arbitrary APIC/GSI SCI routing require later IOAPIC/GSI support. Limine API revision 0 exposes `limine_rsdp_response.address` as a mapped pointer; API revision 1 and later expose it as a physical address. NPKernel therefore tracks the representation and uses the HHDM only for physical ACPI table pointers obtained from the RSDP.
