/*
 * QEMU coroutine implementation
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * Copyright (C) 2020, TrussLab@University of California, Irvine. 
 *  Authors: Yingtong Liu <yingtong@uci.edu>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef QEMU_COROUTINE_H
#define QEMU_COROUTINE_H

#include <stdbool.h>

/**
 * Coroutines are a mechanism for stack switching and can be used for
 * cooperative userspace threading.  These functions provide a simple but
 * useful flavor of coroutines that is suitable for writing sequential code,
 * rather than callbacks, for operations that need to give up control while
 * waiting for events to complete.
 *
 * These functions are re-entrant and may be used outside the global mutex.
 */

/**
 * Mark a function that executes in coroutine context
 *
 * Functions that execute in coroutine context cannot be called directly from
 * normal functions.  In the future it would be nice to enable compiler or
 * static checker support for catching such errors.  This annotation might make
 * it possible and in the meantime it serves as documentation.
 *
 * For example:
 *
 *   static void coroutine_fn foo(void) {
 *       ....
 *   }
 */
#define coroutine_fn

typedef struct Coroutine Coroutine;

/**
 * Coroutine entry point
 *
 * When the coroutine is entered for the first time, opaque is passed in as an
 * argument.
 *
 * When this function returns, the coroutine is destroyed automatically and
 * execution continues in the caller who last entered the coroutine.
 */
typedef void coroutine_fn CoroutineEntry(void *opaque);

/**
 * Create a new coroutine
 *
 * Use qemu_coroutine_enter() to actually transfer control to the coroutine.
 */
extern Coroutine *qemu_coroutine_create(CoroutineEntry *entry);

/**
 * Transfer control to a coroutine
 *
 * The opaque argument is passed as the argument to the entry point when
 * entering the coroutine for the first time.  It is subsequently ignored.
 */
extern void qemu_coroutine_enter(Coroutine *coroutine, void *opaque);

/**
 * Transfer control back to a coroutine's caller
 *
 * This function does not return until the coroutine is re-entered using
 * qemu_coroutine_enter().
 */
extern void coroutine_fn qemu_coroutine_yield(void);

#endif /* QEMU_COROUTINE_H */
