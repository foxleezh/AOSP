/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Miniature version of a header-only keyutils.h (no library required) */

#ifndef _INIT_KEYUTILS_H_
#define _INIT_KEYUTILS_H_

#ifndef KEYUTILS_H /* walk away if the _real_ one exists */

#include <linux/keyctl.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline long keyctl(int cmd, ...) {
    va_list va;
    unsigned long arg2, arg3, arg4, arg5;

    va_start(va, cmd);
    arg2 = va_arg(va, unsigned long);
    arg3 = va_arg(va, unsigned long);
    arg4 = va_arg(va, unsigned long);
    arg5 = va_arg(va, unsigned long);
    va_end(va);
    return syscall(__NR_keyctl, cmd, arg2, arg3, arg4, arg5);
}

#endif

#endif
