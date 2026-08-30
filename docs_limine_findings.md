# Limine configuration findings

The official Limine CONFIG reference states that the configuration file must be located at `/boot/limine/limine.conf` (among other fallback paths) and that a native Limine menu entry begins with a line starting with `/`. The relevant native kernel option is `path`, whose value is the executable path. The current NPKernel ISO places the configuration at `/boot/limine/limine.conf`, but its entry syntax should be simplified to the native `protocol: limine` and `path: boot():/boot/npkernel.elf` form; a separate `kernel protocol` line is not required for a native Limine entry.

Source: https://github.com/Limine-Bootloader/Limine/blob/v12.x/CONFIG.md
