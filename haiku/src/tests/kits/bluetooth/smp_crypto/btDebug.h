/*
 * Minimal btDebug.h shim for userspace crypto tests.
 */
#ifndef _BTDEBUG_H
#define _BTDEBUG_H

#include <stdio.h>

#define TRACE(x...) /* nothing */
#define ERROR(x...) fprintf(stderr, "bt: " x)
#define CALLED(x...) /* nothing */

#endif /* _BTDEBUG_H */
