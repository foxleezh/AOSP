/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <selinux/android.h>
#include <selinux/label.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <thread>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <cutils/android_reboot.h>
/* for ANDROID_SOCKET_* */
#include <cutils/sockets.h>

#include "init.h"
#include "log.h"
#include "reboot.h"
#include "util.h"

static unsigned int do_decode_uid(const char *s)
{
    unsigned int v;

    if (!s || *s == '\0')
        return UINT_MAX;

    if (isalpha(s[0])) {
        struct passwd* pwd = getpwnam(s);
        if (!pwd)
            return UINT_MAX;
        return pwd->pw_uid;
    }

    errno = 0;
    v = (unsigned int) strtoul(s, 0, 0);
    if (errno)
        return UINT_MAX;
    return v;
}

/*
 * decode_uid - decodes and returns the given string, which can be either the
 * numeric or name representation, into the integer uid or gid. Returns
 * UINT_MAX on error.
 */
unsigned int decode_uid(const char *s) {
    unsigned int v = do_decode_uid(s);
    if (v == UINT_MAX) {
        LOG(ERROR) << "decode_uid: Unable to find UID for '" << s << "'; returning UINT_MAX";
    }
    return v;
}

/*
 * create_socket - creates a Unix domain socket in ANDROID_SOCKET_DIR
 * ("/dev/socket") as dictated in init.rc. This socket is inherited by the
 * daemon. We communicate the file descriptor's value via the environment
 * variable ANDROID_SOCKET_ENV_PREFIX<name> ("ANDROID_SOCKET_foo").
 */
int create_socket(const char *name, int type, mode_t perm, uid_t uid,
                  gid_t gid, const char *socketcon)
{
    if (socketcon) {
        if (setsockcreatecon(socketcon) == -1) {
            PLOG(ERROR) << "setsockcreatecon(\"" << socketcon << "\") failed";
            return -1;
        }
    }

    android::base::unique_fd fd(socket(PF_UNIX, type, 0));
    if (fd < 0) {
        PLOG(ERROR) << "Failed to open socket '" << name << "'";
        return -1;
    }

    if (socketcon) setsockcreatecon(NULL);

    struct sockaddr_un addr;
    memset(&addr, 0 , sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), ANDROID_SOCKET_DIR"/%s",
             name);

    if ((unlink(addr.sun_path) != 0) && (errno != ENOENT)) {
        PLOG(ERROR) << "Failed to unlink old socket '" << name << "'";
        return -1;
    }

    char *filecon = NULL;
    if (sehandle) {
        if (selabel_lookup(sehandle, &filecon, addr.sun_path, S_IFSOCK) == 0) {
            setfscreatecon(filecon);
        }
    }

    int ret = bind(fd, (struct sockaddr *) &addr, sizeof (addr));
    int savederrno = errno;

    setfscreatecon(NULL);
    freecon(filecon);

    if (ret) {
        errno = savederrno;
        PLOG(ERROR) << "Failed to bind socket '" << name << "'";
        goto out_unlink;
    }

    if (lchown(addr.sun_path, uid, gid)) {
        PLOG(ERROR) << "Failed to lchown socket '" << addr.sun_path << "'";
        goto out_unlink;
    }
    if (fchmodat(AT_FDCWD, addr.sun_path, perm, AT_SYMLINK_NOFOLLOW)) {
        PLOG(ERROR) << "Failed to fchmodat socket '" << addr.sun_path << "'";
        goto out_unlink;
    }

    LOG(INFO) << "Created socket '" << addr.sun_path << "'"
              << ", mode " << std::oct << perm << std::dec
              << ", user " << uid
              << ", group " << gid;

    return fd.release();

out_unlink:
    unlink(addr.sun_path);
    return -1;
}

bool read_file(const std::string& path, std::string* content) {
    content->clear();

    android::base::unique_fd fd(
        TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)));
    if (fd == -1) {
        return false;
    }

    // For security reasons, disallow world-writable
    // or group-writable files.
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        PLOG(ERROR) << "fstat failed for '" << path << "'";
        return false;
    }
    if ((sb.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        LOG(ERROR) << "skipping insecure file '" << path << "'";
        return false;
    }

    return android::base::ReadFdToString(fd, content);
}

bool write_file(const std::string& path, const std::string& content) {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(
        open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0600)));
    if (fd == -1) {
        PLOG(ERROR) << "write_file: Unable to open '" << path << "'";
        return false;
    }
    bool success = android::base::WriteStringToFd(content, fd);
    if (!success) {
        PLOG(ERROR) << "write_file: Unable to write to '" << path << "'";
    }
    return success;
}

boot_clock::time_point boot_clock::now() {
  timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return boot_clock::time_point(std::chrono::seconds(ts.tv_sec) +
                                std::chrono::nanoseconds(ts.tv_nsec));
}

int mkdir_recursive(const char *pathname, mode_t mode)
{
    char buf[128];
    const char *slash;
    const char *p = pathname;
    int width;
    int ret;
    struct stat info;

    while ((slash = strchr(p, '/')) != NULL) {
        width = slash - pathname;
        p = slash + 1;
        if (width < 0)
            break;
        if (width == 0)
            continue;
        if ((unsigned int)width > sizeof(buf) - 1) {
            LOG(ERROR) << "path too long for mkdir_recursive";
            return -1;
        }
        memcpy(buf, pathname, width);
        buf[width] = 0;
        if (stat(buf, &info) != 0) {
            ret = make_dir(buf, mode);
            if (ret && errno != EEXIST)
                return ret;
        }
    }
    ret = make_dir(pathname, mode);
    if (ret && errno != EEXIST)
        return ret;
    return 0;
}

/*
 * replaces any unacceptable characters with '_', the
 * length of the resulting string is equal to the input string
 */
void sanitize(char *s)
{
    const char* accept =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789"
            "_-.";

    if (!s)
        return;

    while (*s) {
        s += strspn(s, accept);
        if (*s) *s++ = '_';
    }
}

int wait_for_file(const char* filename, std::chrono::nanoseconds timeout) {
    boot_clock::time_point timeout_time = boot_clock::now() + timeout;
    while (boot_clock::now() < timeout_time) {
        struct stat sb;
        if (stat(filename, &sb) != -1) return 0;

        std::this_thread::sleep_for(10ms);
    }
    return -1;
}

void import_kernel_cmdline(bool in_qemu,
                           const std::function<void(const std::string&, const std::string&, bool)>& fn) {
    std::string cmdline;
    android::base::ReadFileToString("/proc/cmdline", &cmdline);

    for (const auto& entry : android::base::Split(android::base::Trim(cmdline), " ")) {
        std::vector<std::string> pieces = android::base::Split(entry, "=");
        if (pieces.size() == 2) {
            fn(pieces[0], pieces[1], in_qemu);
        }
    }
}

int make_dir(const char *path, mode_t mode)
{
    int rc;

    char *secontext = NULL;

    if (sehandle) {
        selabel_lookup(sehandle, &secontext, path, mode);
        setfscreatecon(secontext);
    }

    rc = mkdir(path, mode);

    if (secontext) {
        int save_errno = errno;
        freecon(secontext);
        setfscreatecon(NULL);
        errno = save_errno;
    }

    return rc;
}

int restorecon(const char* pathname, int flags)
{
    return selinux_android_restorecon(pathname, flags);
}

/*
 * Writes hex_len hex characters (1/2 byte) to hex from bytes.
 */
std::string bytes_to_hex(const uint8_t* bytes, size_t bytes_len) {
    std::string hex("0x");
    for (size_t i = 0; i < bytes_len; i++)
        android::base::StringAppendF(&hex, "%02x", bytes[i]);
    return hex;
}

/*
 * Returns true is pathname is a directory
 */
bool is_dir(const char* pathname) {
    struct stat info;
    if (stat(pathname, &info) == -1) {
        return false;
    }
    return S_ISDIR(info.st_mode);
}

bool expand_props(const std::string& src, std::string* dst) {
    const char* src_ptr = src.c_str();

    if (!dst) {
        return false;
    }

    /* - variables can either be $x.y or ${x.y}, in case they are only part
     *   of the string.
     * - will accept $$ as a literal $.
     * - no nested property expansion, i.e. ${foo.${bar}} is not supported,
     *   bad things will happen
     * - ${x.y:-default} will return default value if property empty.
     */
    while (*src_ptr) {
        const char* c;

        c = strchr(src_ptr, '$');
        if (!c) {
            dst->append(src_ptr);
            return true;
        }

        dst->append(src_ptr, c);
        c++;

        if (*c == '$') {
            dst->push_back(*(c++));
            src_ptr = c;
            continue;
        } else if (*c == '\0') {
            return true;
        }

        std::string prop_name;
        std::string def_val;
        if (*c == '{') {
            c++;
            const char* end = strchr(c, '}');
            if (!end) {
                // failed to find closing brace, abort.
                LOG(ERROR) << "unexpected end of string in '" << src << "', looking for }";
                return false;
            }
            prop_name = std::string(c, end);
            c = end + 1;
            size_t def = prop_name.find(":-");
            if (def < prop_name.size()) {
                def_val = prop_name.substr(def + 2);
                prop_name = prop_name.substr(0, def);
            }
        } else {
            prop_name = c;
            LOG(ERROR) << "using deprecated syntax for specifying property '" << c << "', use ${name} instead";
            c += prop_name.size();
        }

        if (prop_name.empty()) {
            LOG(ERROR) << "invalid zero-length property name in '" << src << "'";
            return false;
        }

        std::string prop_val = android::base::GetProperty(prop_name, "");
        if (prop_val.empty()) {
            if (def_val.empty()) {
                LOG(ERROR) << "property '" << prop_name << "' doesn't exist while expanding '" << src << "'";
                return false;
            }
            prop_val = def_val;
        }

        dst->append(prop_val);
        src_ptr = c;
    }

    return true;
}

void panic() {
    LOG(ERROR) << "panic: rebooting to bootloader";
    DoReboot(ANDROID_RB_RESTART2, "reboot", "bootloader", false);
}

std::ostream& operator<<(std::ostream& os, const Timer& t) {
    os << t.duration_s() << " seconds";
    return os;
}

// Reads the content of device tree file under kAndroidDtDir directory.
// Returns true if the read is success, false otherwise.
bool read_android_dt_file(const std::string& sub_path, std::string* dt_content) {
    const std::string file_name = kAndroidDtDir + sub_path;
    if (android::base::ReadFileToString(file_name, dt_content)) {
        if (!dt_content->empty()) {
            dt_content->pop_back();  // Trims the trailing '\0' out.
            return true;
        }
    }
    return false;
}

bool is_android_dt_value_expected(const std::string& sub_path, const std::string& expected_content) {
    std::string dt_content;
    if (read_android_dt_file(sub_path, &dt_content)) {
        if (dt_content == expected_content) {
            return true;
        }
    }
    return false;
}
