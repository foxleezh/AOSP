/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <android-base/stringprintf.h>

#include "action.h"
#include "init.h"
#include "log.h"
#include "service.h"
#include "util.h"

static int signal_write_fd = -1;
static int signal_read_fd = -1;

static void handle_signal() {
    // Clear outstanding requests.
    char buf[32];
    read(signal_read_fd, buf, sizeof(buf));

    ServiceManager::GetInstance().ReapAnyOutstandingChildren();
}

static void SIGCHLD_handler(int) {
    if (TEMP_FAILURE_RETRY(write(signal_write_fd, "1", 1)) == -1) {
        PLOG(ERROR) << "write(signal_write_fd) failed";
    }
}

void signal_handler_init() {
    // Create a signalling mechanism for SIGCHLD.
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, s) == -1) {
        PLOG(ERROR) << "socketpair failed";
        exit(1);
    }

    signal_write_fd = s[0];
    signal_read_fd = s[1];

    // Write to signal_write_fd if we catch SIGCHLD.
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIGCHLD_handler;
    act.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &act, 0);

    ServiceManager::GetInstance().ReapAnyOutstandingChildren();

    register_epoll_handler(signal_read_fd, handle_signal);
}
