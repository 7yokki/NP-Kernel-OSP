#ifndef NPK_PROCFS_H
#define NPK_PROCFS_H

#include "types.h"

bool procfs_is_path(const char *path);
ssize_t procfs_snapshot(const char *path, char *buffer, size_t capacity);

#endif
