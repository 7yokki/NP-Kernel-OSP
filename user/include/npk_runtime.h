#ifndef NPK_USER_RUNTIME_H
#define NPK_USER_RUNTIME_H
#include <npk/user_abi.h>
long npk_syscall6(long number, long a1, long a2, long a3, long a4, long a5, long a6);
long npk_errno_result(long result);
#endif
