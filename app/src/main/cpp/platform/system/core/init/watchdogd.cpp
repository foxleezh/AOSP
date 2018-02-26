/*
 * Copyright (C) 2012 The Android Open Source Project
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/watchdog.h>

#include "log.h"
#include "util.h"

#define DEV_NAME "/dev/watchdog"

int watchdogd_main(int argc, char **argv) {
    InitKernelLogging(argv);

    int interval = 10;
    if (argc >= 2) interval = atoi(argv[1]);

    int margin = 10;
    if (argc >= 3) margin = atoi(argv[2]);

    LOG(INFO) << "watchdogd started (interval " << interval << ", margin " << margin << ")!";

    int fd = open(DEV_NAME, O_RDWR|O_CLOEXEC);
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << DEV_NAME;
        return 1;
    }

    int timeout = interval + margin;
    int ret = ioctl(fd, WDIOC_SETTIMEOUT, &timeout);
    if (ret) {
        PLOG(ERROR) << "Failed to set timeout to " << timeout;
        ret = ioctl(fd, WDIOC_GETTIMEOUT, &timeout);
        if (ret) {
            PLOG(ERROR) << "Failed to get timeout";
        } else {
            if (timeout > margin) {
                interval = timeout - margin;
            } else {
                interval = 1;
            }
            LOG(WARNING) << "Adjusted interval to timeout returned by driver: "
                         << "timeout " << timeout
                         << ", interval " << interval
                         << ", margin " << margin;
        }
    }

    while (true) {
        write(fd, "", 1);
        sleep(interval);
    }
}
