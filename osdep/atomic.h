/*
 * This file is part of mpv.
 * Copyright (c) 2013 Stefano Pigozzi <stefano.pigozzi@gmail.com>
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MP_ATOMIC_H
#define MP_ATOMIC_H

#include <inttypes.h>
#include "config.h"

#if HAVE_STDATOMIC
#include <stdatomic.h>
#else

// Emulate the parts of C11 stdatomic.h needed by mpv.
// Still relies on gcc/clang atomic builtins.

typedef struct { volatile unsigned long v;      } atomic_ulong;
typedef struct { volatile int v;                } atomic_int;
typedef struct { volatile unsigned int v;       } atomic_uint;
typedef struct { volatile _Bool v;              } atomic_bool;
typedef struct { volatile long long v;          } atomic_llong;
typedef struct { volatile uint_least32_t v;     } atomic_uint_least32_t;
typedef struct { volatile unsigned long long v; } atomic_ullong;

#define ATOMIC_VAR_INIT(x) \
    {.v = (x)}

#define memory_order_relaxed 1
#define memory_order_seq_cst 2

#define atomic_load_explicit(p, e) atomic_load(p)

#if HAVE_ATOMIC_BUILTINS

#define atomic_load(p) \
    __atomic_load_n(&(p)->v, __ATOMIC_SEQ_CST)
#define atomic_store(p, val) \
    __atomic_store_n(&(p)->v, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_add(a, b) \
    __atomic_fetch_add(&(a)->v, b, __ATOMIC_SEQ_CST)
#define atomic_fetch_and(a, b) \
    __atomic_fetch_and(&(a)->v, b, __ATOMIC_SEQ_CST)
#define atomic_fetch_or(a, b) \
    __atomic_fetch_or(&(a)->v, b, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong(a, b, c) \
    __atomic_compare_exchange_n(&(a)->v, b, c, 0, __ATOMIC_SEQ_CST, \
    __ATOMIC_SEQ_CST)

#elif defined(__GNUC__)

#include <pthread.h>

extern pthread_mutex_t mp_atomic_mutex;

#define atomic_load(p)                                  \
    ({ __typeof__(p) p_ = (p);                          \
       pthread_mutex_lock(&mp_atomic_mutex);            \
       __typeof__(p_->v) v = p_->v;                     \
       pthread_mutex_unlock(&mp_atomic_mutex);          \
       v; })
#define atomic_store(p, val)                            \
    ({ __typeof__(val) val_ = (val);                    \
       __typeof__(p) p_ = (p);                          \
       pthread_mutex_lock(&mp_atomic_mutex);            \
       p_->v = val_;                                    \
       pthread_mutex_unlock(&mp_atomic_mutex); })
#define atomic_fetch_op(a, b, op)                       \
    ({ __typeof__(a) a_ = (a);                          \
       __typeof__(b) b_ = (b);                          \
       pthread_mutex_lock(&mp_atomic_mutex);            \
       __typeof__(a_->v) v = a_->v;                     \
       a_->v = v op b_;                                 \
       pthread_mutex_unlock(&mp_atomic_mutex);          \
       v; })
#define atomic_fetch_add(a, b) atomic_fetch_op(a, b, +)
#define atomic_fetch_and(a, b) atomic_fetch_op(a, b, &)
#define atomic_fetch_or(a, b)  atomic_fetch_op(a, b, |)
#define atomic_compare_exchange_strong(p, old, new)     \
    ({ __typeof__(p) p_ = (p);                          \
       __typeof__(old) old_ = (old);                    \
       __typeof__(new) new_ = (new);                    \
       pthread_mutex_lock(&mp_atomic_mutex);            \
       int res = p_->v == *old_;                        \
       if (res) {                                       \
           p_->v = new_;                                \
       } else {                                         \
           *old_ = p_->v;                               \
       }                                                \
       pthread_mutex_unlock(&mp_atomic_mutex);          \
       res; })

#else
# error "this should have been a configuration error, report a bug please"
#endif /* no atomics */

#endif /* else HAVE_STDATOMIC */

#endif
