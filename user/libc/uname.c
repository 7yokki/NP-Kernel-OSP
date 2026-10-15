#include <npk_runtime.h>
#include <errno.h>
#include <sys/utsname.h>
int uname(struct utsname *name) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_UNAME, (long)name, 0, 0, 0, 0, 0));
}
