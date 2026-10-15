#ifndef NPK_USER_SYS_IOCTL_H
#define NPK_USER_SYS_IOCTL_H
#include <stdint.h>

#define TCGETS 0x5401UL
#define TCSETS 0x5402UL
#define TCSETSW 0x5403UL
#define TCSETSF 0x5404UL
#define TIOCGWINSZ 0x5413UL
#define TTY_ICANON 0x00000002U
#define TTY_ECHO 0x00000008U
int ioctl(int fd, unsigned long request, void *argument);

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} npk_termios_t;

#endif
