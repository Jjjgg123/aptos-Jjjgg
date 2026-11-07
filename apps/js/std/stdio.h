#ifndef APTOS_STD_STDIO_H
#define APTOS_STD_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* aptos.h 提供 printf/snprintf/vsnprintf/puts/putchar 声明与实现 */
#include "aptos.h"

typedef long FILE;
#define stdin   ((FILE *)1)
#define stdout  ((FILE *)2)
#define stderr  ((FILE *)3)

#endif
