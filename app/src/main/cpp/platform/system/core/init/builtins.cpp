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

#include "builtins.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <net/if.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/loop.h>
#include <linux/module.h>

#include <string>
#include <thread>

#include <selinux/android.h>
#include <selinux/selinux.h>
#include <selinux/label.h>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <ext4_utils/ext4_crypt.h>
#include <ext4_utils/ext4_crypt_init_extensions.h>
#include <fs_mgr.h>
#include <logwrap/logwrap.h>

#include "action.h"
#include "bootchart.h"
#include "devices.h"
#include "init.h"
#include "init_parser.h"
#include "log.h"
#include "property_service.h"
#include "reboot.h"
#include "service.h"
#include "signal_handler.h"
#include "util.h"

using namespace std::literals::string_literals;

#define chmod DO_NOT_USE_CHMOD_USE_FCHMODAT_SYMLINK_NOFOLLOW

static constexpr std::chrono::nanoseconds kCommandRetryTimeout = 5s;

static int insmod(const char *filename, const char *options, int flags) {
    int fd = open(filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        PLOG(ERROR) << "insmod: open(\"" << filename << "\") failed";
        return -1;
    }
    int rc = syscall(__NR_finit_module, fd, options, flags);
    if (rc == -1) {
        PLOG(ERROR) << "finit_module for \"" << filename << "\" failed";
    }
    close(fd);
    return rc;
}

static int __ifupdown(const char *interface, int up) {
    struct ifreq ifr;
    int s, ret;

    strlcpy(ifr.ifr_name, interface, IFNAMSIZ);

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;

    ret = ioctl(s, SIOCGIFFLAGS, &ifr);
    if (ret < 0) {
        goto done;
    }

    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;

    ret = ioctl(s, SIOCSIFFLAGS, &ifr);

done:
    close(s);
    return ret;
}

static int reboot_into_recovery(const std::vector<std::string>& options) {
    std::string err;
    if (!write_bootloader_message(options, &err)) {
        LOG(ERROR) << "failed to set bootloader message: " << err;
        return -1;
    }
    DoReboot(ANDROID_RB_RESTART2, "reboot", "recovery", false);
    return 0;
}

static int do_class_start(const std::vector<std::string>& args) {
        /* Starting a class does not start services
         * which are explicitly disabled.  They must
         * be started individually.
         */
    ServiceManager::GetInstance().
        ForEachServiceInClass(args[1], [] (Service* s) { s->StartIfNotDisabled(); });
    return 0;
}

static int do_class_stop(const std::vector<std::string>& args) {
    ServiceManager::GetInstance().
        ForEachServiceInClass(args[1], [] (Service* s) { s->Stop(); });
    return 0;
}

static int do_class_reset(const std::vector<std::string>& args) {
    ServiceManager::GetInstance().
        ForEachServiceInClass(args[1], [] (Service* s) { s->Reset(); });
    return 0;
}

static int do_class_restart(const std::vector<std::string>& args) {
    ServiceManager::GetInstance().
        ForEachServiceInClass(args[1], [] (Service* s) { s->Restart(); });
    return 0;
}

static int do_domainname(const std::vector<std::string>& args) {
    return write_file("/proc/sys/kernel/domainname", args[1]) ? 0 : 1;
}

static int do_enable(const std::vector<std::string>& args) {
    Service* svc = ServiceManager::GetInstance().FindServiceByName(args[1]);
    if (!svc) {
        return -1;
    }
    return svc->Enable();
}

static int do_exec(const std::vector<std::string>& args) {
    return ServiceManager::GetInstance().Exec(args) ? 0 : -1;
}

static int do_exec_start(const std::vector<std::string>& args) {
    return ServiceManager::GetInstance().ExecStart(args[1]) ? 0 : -1;
}

static int do_export(const std::vector<std::string>& args) {
    return add_environment(args[1].c_str(), args[2].c_str());
}

static int do_hostname(const std::vector<std::string>& args) {
    return write_file("/proc/sys/kernel/hostname", args[1]) ? 0 : 1;
}

static int do_ifup(const std::vector<std::string>& args) {
    return __ifupdown(args[1].c_str(), 1);
}

static int do_insmod(const std::vector<std::string>& args) {
    int flags = 0;
    auto it = args.begin() + 1;

    if (!(*it).compare("-f")) {
        flags = MODULE_INIT_IGNORE_VERMAGIC | MODULE_INIT_IGNORE_MODVERSIONS;
        it++;
    }

    std::string filename = *it++;
    std::string options = android::base::Join(std::vector<std::string>(it, args.end()), ' ');
    return insmod(filename.c_str(), options.c_str(), flags);
}

static int do_mkdir(const std::vector<std::string>& args) {
    mode_t mode = 0755;
    int ret;

    /* mkdir <path> [mode] [owner] [group] */

    if (args.size() >= 3) {
        mode = std::strtoul(args[2].c_str(), 0, 8);
    }

    ret = make_dir(args[1].c_str(), mode);
    /* chmod in case the directory already exists */
    if (ret == -1 && errno == EEXIST) {
        ret = fchmodat(AT_FDCWD, args[1].c_str(), mode, AT_SYMLINK_NOFOLLOW);
    }
    if (ret == -1) {
        return -errno;
    }

    if (args.size() >= 4) {
        uid_t uid = decode_uid(args[3].c_str());
        gid_t gid = -1;

        if (args.size() == 5) {
            gid = decode_uid(args[4].c_str());
        }

        if (lchown(args[1].c_str(), uid, gid) == -1) {
            return -errno;
        }

        /* chown may have cleared S_ISUID and S_ISGID, chmod again */
        if (mode & (S_ISUID | S_ISGID)) {
            ret = fchmodat(AT_FDCWD, args[1].c_str(), mode, AT_SYMLINK_NOFOLLOW);
            if (ret == -1) {
                return -errno;
            }
        }
    }

    if (e4crypt_is_native()) {
        if (e4crypt_set_directory_policy(args[1].c_str())) {
            const std::vector<std::string> options = {
                "--prompt_and_wipe_data",
                "--reason=set_policy_failed:"s + args[1]};
            reboot_into_recovery(options);
            return -1;
        }
    }
    return 0;
}

/* umount <path> */
static int do_umount(const std::vector<std::string>& args) {
  return umount(args[1].c_str());
}

static struct {
    const char *name;
    unsigned flag;
} mount_flags[] = {
    { "noatime",    MS_NOATIME },
    { "noexec",     MS_NOEXEC },
    { "nosuid",     MS_NOSUID },
    { "nodev",      MS_NODEV },
    { "nodiratime", MS_NODIRATIME },
    { "ro",         MS_RDONLY },
    { "rw",         0 },
    { "remount",    MS_REMOUNT },
    { "bind",       MS_BIND },
    { "rec",        MS_REC },
    { "unbindable", MS_UNBINDABLE },
    { "private",    MS_PRIVATE },
    { "slave",      MS_SLAVE },
    { "shared",     MS_SHARED },
    { "defaults",   0 },
    { 0,            0 },
};

#define DATA_MNT_POINT "/data"

/* mount <type> <device> <path> <flags ...> <options> */
static int do_mount(const std::vector<std::string>& args) {
    char tmp[64];
    const char *source, *target, *system;
    const char *options = NULL;
    unsigned flags = 0;
    std::size_t na = 0;
    int n, i;
    int wait = 0;

    for (na = 4; na < args.size(); na++) {
        for (i = 0; mount_flags[i].name; i++) {
            if (!args[na].compare(mount_flags[i].name)) {
                flags |= mount_flags[i].flag;
                break;
            }
        }

        if (!mount_flags[i].name) {
            if (!args[na].compare("wait"))
                wait = 1;
            /* if our last argument isn't a flag, wolf it up as an option string */
            else if (na + 1 == args.size())
                options = args[na].c_str();
        }
    }

    system = args[1].c_str();
    source = args[2].c_str();
    target = args[3].c_str();

    if (!strncmp(source, "loop@", 5)) {
        int mode, loop, fd;
        struct loop_info info;

        mode = (flags & MS_RDONLY) ? O_RDONLY : O_RDWR;
        fd = open(source + 5, mode | O_CLOEXEC);
        if (fd < 0) {
            return -1;
        }

        for (n = 0; ; n++) {
            snprintf(tmp, sizeof(tmp), "/dev/block/loop%d", n);
            loop = open(tmp, mode | O_CLOEXEC);
            if (loop < 0) {
                close(fd);
                return -1;
            }

            /* if it is a blank loop device */
            if (ioctl(loop, LOOP_GET_STATUS, &info) < 0 && errno == ENXIO) {
                /* if it becomes our loop device */
                if (ioctl(loop, LOOP_SET_FD, fd) >= 0) {
                    close(fd);

                    if (mount(tmp, target, system, flags, options) < 0) {
                        ioctl(loop, LOOP_CLR_FD, 0);
                        close(loop);
                        return -1;
                    }

                    close(loop);
                    goto exit_success;
                }
            }

            close(loop);
        }

        close(fd);
        LOG(ERROR) << "out of loopback devices";
        return -1;
    } else {
        if (wait)
            wait_for_file(source, kCommandRetryTimeout);
        if (mount(source, target, system, flags, options) < 0) {
            return -1;
        }

    }

exit_success:
    return 0;

}

/* Imports .rc files from the specified paths. Default ones are applied if none is given.
 *
 * start_index: index of the first path in the args list
 */
static void import_late(const std::vector<std::string>& args, size_t start_index, size_t end_index) {
    Parser& parser = Parser::GetInstance();
    if (end_index <= start_index) {
        // Fallbacks for partitions on which early mount isn't enabled.
        if (!parser.is_system_etc_init_loaded()) {
            parser.ParseConfig("/system/etc/init");
            parser.set_is_system_etc_init_loaded(true);
        }
        if (!parser.is_vendor_etc_init_loaded()) {
            parser.ParseConfig("/vendor/etc/init");
            parser.set_is_vendor_etc_init_loaded(true);
        }
        if (!parser.is_odm_etc_init_loaded()) {
            parser.ParseConfig("/odm/etc/init");
            parser.set_is_odm_etc_init_loaded(true);
        }
    } else {
        for (size_t i = start_index; i < end_index; ++i) {
            parser.ParseConfig(args[i]);
        }
    }

    // Turning this on and letting the INFO logging be discarded adds 0.2s to
    // Nexus 9 boot time, so it's disabled by default.
    if (false) parser.DumpState();
}

/* mount_fstab
 *
 *  Call fs_mgr_mount_all() to mount the given fstab
 */
static int mount_fstab(const char* fstabfile, int mount_mode) {
    int ret = -1;

    /*
     * Call fs_mgr_mount_all() to mount all filesystems.  We fork(2) and
     * do the call in the child to provide protection to the main init
     * process if anything goes wrong (crash or memory leak), and wait for
     * the child to finish in the parent.
     */
    pid_t pid = fork();
    if (pid > 0) {
        /* Parent.  Wait for the child to return */
        int status;
        int wp_ret = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
        if (wp_ret == -1) {
            // Unexpected error code. We will continue anyway.
            PLOG(WARNING) << "waitpid failed";
        }

        if (WIFEXITED(status)) {
            ret = WEXITSTATUS(status);
        } else {
            ret = -1;
        }
    } else if (pid == 0) {
        /* child, call fs_mgr_mount_all() */

        // So we can always see what fs_mgr_mount_all() does.
        // Only needed if someone explicitly changes the default log level in their init.rc.
        android::base::ScopedLogSeverity info(android::base::INFO);

        struct fstab* fstab = fs_mgr_read_fstab(fstabfile);
        int child_ret = fs_mgr_mount_all(fstab, mount_mode);
        fs_mgr_free_fstab(fstab);
        if (child_ret == -1) {
            PLOG(ERROR) << "fs_mgr_mount_all returned an error";
        }
        _exit(child_ret);
    } else {
        /* fork failed, return an error */
        return -1;
    }
    return ret;
}

/* Queue event based on fs_mgr return code.
 *
 * code: return code of fs_mgr_mount_all
 *
 * This function might request a reboot, in which case it will
 * not return.
 *
 * return code is processed based on input code
 */
static int queue_fs_event(int code) {
    int ret = code;
    if (code == FS_MGR_MNTALL_DEV_NEEDS_ENCRYPTION) {
        ActionManager::GetInstance().QueueEventTrigger("encrypt");
    } else if (code == FS_MGR_MNTALL_DEV_MIGHT_BE_ENCRYPTED) {
        property_set("ro.crypto.state", "encrypted");
        property_set("ro.crypto.type", "block");
        ActionManager::GetInstance().QueueEventTrigger("defaultcrypto");
    } else if (code == FS_MGR_MNTALL_DEV_NOT_ENCRYPTED) {
        property_set("ro.crypto.state", "unencrypted");
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
    } else if (code == FS_MGR_MNTALL_DEV_NOT_ENCRYPTABLE) {
        property_set("ro.crypto.state", "unsupported");
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
    } else if (code == FS_MGR_MNTALL_DEV_NEEDS_RECOVERY) {
        /* Setup a wipe via recovery, and reboot into recovery */
        PLOG(ERROR) << "fs_mgr_mount_all suggested recovery, so wiping data via recovery.";
        const std::vector<std::string> options = {"--wipe_data", "--reason=fs_mgr_mount_all" };
        ret = reboot_into_recovery(options);
        /* If reboot worked, there is no return. */
    } else if (code == FS_MGR_MNTALL_DEV_FILE_ENCRYPTED) {
        if (e4crypt_install_keyring()) {
            return -1;
        }
        property_set("ro.crypto.state", "encrypted");
        property_set("ro.crypto.type", "file");

        // Although encrypted, we have device key, so we do not need to
        // do anything different from the nonencrypted case.
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
    } else if (code > 0) {
        PLOG(ERROR) << "fs_mgr_mount_all returned unexpected error " << code;
    }
    /* else ... < 0: error */

    return ret;
}

/* mount_all <fstab> [ <path> ]* [--<options>]*
 *
 * This function might request a reboot, in which case it will
 * not return.
 */
static int do_mount_all(const std::vector<std::string>& args) {
    std::size_t na = 0;
    bool import_rc = true;
    bool queue_event = true;
    int mount_mode = MOUNT_MODE_DEFAULT;
    const char* fstabfile = args[1].c_str();
    std::size_t path_arg_end = args.size();
    const char* prop_post_fix = "default";

    for (na = args.size() - 1; na > 1; --na) {
        if (args[na] == "--early") {
            path_arg_end = na;
            queue_event = false;
            mount_mode = MOUNT_MODE_EARLY;
            prop_post_fix = "early";
        } else if (args[na] == "--late") {
            path_arg_end = na;
            import_rc = false;
            mount_mode = MOUNT_MODE_LATE;
            prop_post_fix = "late";
        }
    }

    std::string prop_name = android::base::StringPrintf("ro.boottime.init.mount_all.%s",
                                                        prop_post_fix);
    Timer t;
    int ret =  mount_fstab(fstabfile, mount_mode);
    property_set(prop_name.c_str(), std::to_string(t.duration_ms()).c_str());

    if (import_rc) {
        /* Paths of .rc files are specified at the 2nd argument and beyond */
        import_late(args, 2, path_arg_end);
    }

    if (queue_event) {
        /* queue_fs_event will queue event based on mount_fstab return code
         * and return processed return code*/
        ret = queue_fs_event(ret);
    }

    return ret;
}

static int do_swapon_all(const std::vector<std::string>& args) {
    struct fstab *fstab;
    int ret;

    fstab = fs_mgr_read_fstab(args[1].c_str());
    ret = fs_mgr_swapon_all(fstab);
    fs_mgr_free_fstab(fstab);

    return ret;
}

static int do_setprop(const std::vector<std::string>& args) {
    const char* name = args[1].c_str();
    const char* value = args[2].c_str();
    property_set(name, value);
    return 0;
}

static int do_setrlimit(const std::vector<std::string>& args) {
    struct rlimit limit;
    int resource;
    if (android::base::ParseInt(args[1], &resource) &&
        android::base::ParseUint(args[2], &limit.rlim_cur) &&
        android::base::ParseUint(args[3], &limit.rlim_max)) {
        return setrlimit(resource, &limit);
    }
    LOG(WARNING) << "ignoring setrlimit " << args[1] << " " << args[2] << " " << args[3];
    return -1;
}

static int do_start(const std::vector<std::string>& args) {
    Service* svc = ServiceManager::GetInstance().FindServiceByName(args[1]);
    if (!svc) {
        LOG(ERROR) << "do_start: Service " << args[1] << " not found";
        return -1;
    }
    if (!svc->Start())
        return -1;
    return 0;
}

static int do_stop(const std::vector<std::string>& args) {
    Service* svc = ServiceManager::GetInstance().FindServiceByName(args[1]);
    if (!svc) {
        LOG(ERROR) << "do_stop: Service " << args[1] << " not found";
        return -1;
    }
    svc->Stop();
    return 0;
}

static int do_restart(const std::vector<std::string>& args) {
    Service* svc = ServiceManager::GetInstance().FindServiceByName(args[1]);
    if (!svc) {
        LOG(ERROR) << "do_restart: Service " << args[1] << " not found";
        return -1;
    }
    svc->Restart();
    return 0;
}

static int do_trigger(const std::vector<std::string>& args) {
    ActionManager::GetInstance().QueueEventTrigger(args[1]);
    return 0;
}

static int do_symlink(const std::vector<std::string>& args) {
    return symlink(args[1].c_str(), args[2].c_str());
}

static int do_rm(const std::vector<std::string>& args) {
    return unlink(args[1].c_str());
}

static int do_rmdir(const std::vector<std::string>& args) {
    return rmdir(args[1].c_str());
}

static int do_sysclktz(const std::vector<std::string>& args) {
    struct timezone tz = {};
    if (android::base::ParseInt(args[1], &tz.tz_minuteswest) && settimeofday(NULL, &tz) != -1) {
        return 0;
    }
    return -1;
}

static int do_verity_load_state(const std::vector<std::string>& args) {
    int mode = -1;
    bool loaded = fs_mgr_load_verity_state(&mode);
    if (loaded && mode != VERITY_MODE_DEFAULT) {
        ActionManager::GetInstance().QueueEventTrigger("verity-logging");
    }
    return loaded ? 0 : 1;
}

static void verity_update_property(fstab_rec *fstab, const char *mount_point,
                                   int mode, int status) {
    property_set(android::base::StringPrintf("partition.%s.verified", mount_point).c_str(),
                 android::base::StringPrintf("%d", mode).c_str());
}

static int do_verity_update_state(const std::vector<std::string>& args) {
    return fs_mgr_update_verity_state(verity_update_property) ? 0 : 1;
}

static int do_write(const std::vector<std::string>& args) {
    return write_file(args[1], args[2]) ? 0 : 1;
}

static int do_copy(const std::vector<std::string>& args) {
    std::string data;
    if (read_file(args[1], &data)) {
        return write_file(args[2], data) ? 0 : 1;
    }
    return 1;
}

static int do_chown(const std::vector<std::string>& args) {
    /* GID is optional. */
    if (args.size() == 3) {
        if (lchown(args[2].c_str(), decode_uid(args[1].c_str()), -1) == -1)
            return -errno;
    } else if (args.size() == 4) {
        if (lchown(args[3].c_str(), decode_uid(args[1].c_str()),
                   decode_uid(args[2].c_str())) == -1)
            return -errno;
    } else {
        return -1;
    }
    return 0;
}

static mode_t get_mode(const char *s) {
    mode_t mode = 0;
    while (*s) {
        if (*s >= '0' && *s <= '7') {
            mode = (mode<<3) | (*s-'0');
        } else {
            return -1;
        }
        s++;
    }
    return mode;
}

static int do_chmod(const std::vector<std::string>& args) {
    mode_t mode = get_mode(args[1].c_str());
    if (fchmodat(AT_FDCWD, args[2].c_str(), mode, AT_SYMLINK_NOFOLLOW) < 0) {
        return -errno;
    }
    return 0;
}

static int do_restorecon(const std::vector<std::string>& args) {
    int ret = 0;

    struct flag_type {const char* name; int value;};
    static const flag_type flags[] = {
        {"--recursive", SELINUX_ANDROID_RESTORECON_RECURSE},
        {"--skip-ce", SELINUX_ANDROID_RESTORECON_SKIPCE},
        {"--cross-filesystems", SELINUX_ANDROID_RESTORECON_CROSS_FILESYSTEMS},
        {0, 0}
    };

    int flag = 0;

    bool in_flags = true;
    for (size_t i = 1; i < args.size(); ++i) {
        if (android::base::StartsWith(args[i], "--")) {
            if (!in_flags) {
                LOG(ERROR) << "restorecon - flags must precede paths";
                return -1;
            }
            bool found = false;
            for (size_t j = 0; flags[j].name; ++j) {
                if (args[i] == flags[j].name) {
                    flag |= flags[j].value;
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG(ERROR) << "restorecon - bad flag " << args[i];
                return -1;
            }
        } else {
            in_flags = false;
            if (restorecon(args[i].c_str(), flag) < 0) {
                ret = -errno;
            }
        }
    }
    return ret;
}

static int do_restorecon_recursive(const std::vector<std::string>& args) {
    std::vector<std::string> non_const_args(args);
    non_const_args.insert(std::next(non_const_args.begin()), "--recursive");
    return do_restorecon(non_const_args);
}

static int do_loglevel(const std::vector<std::string>& args) {
    // TODO: support names instead/as well?
    int log_level = -1;
    android::base::ParseInt(args[1], &log_level);
    android::base::LogSeverity severity;
    switch (log_level) {
        case 7: severity = android::base::DEBUG; break;
        case 6: severity = android::base::INFO; break;
        case 5:
        case 4: severity = android::base::WARNING; break;
        case 3: severity = android::base::ERROR; break;
        case 2:
        case 1:
        case 0: severity = android::base::FATAL; break;
        default:
            LOG(ERROR) << "loglevel: invalid log level " << log_level;
            return -EINVAL;
    }
    android::base::SetMinimumLogSeverity(severity);
    return 0;
}

static int do_load_persist_props(const std::vector<std::string>& args) {
    load_persist_props();
    return 0;
}

static int do_load_system_props(const std::vector<std::string>& args) {
    load_system_props();
    return 0;
}

static int do_wait(const std::vector<std::string>& args) {
    if (args.size() == 2) {
        return wait_for_file(args[1].c_str(), kCommandRetryTimeout);
    } else if (args.size() == 3) {
        int timeout;
        if (android::base::ParseInt(args[2], &timeout)) {
            return wait_for_file(args[1].c_str(), std::chrono::seconds(timeout));
        }
    }
    return -1;
}

static int do_wait_for_prop(const std::vector<std::string>& args) {
    const char* name = args[1].c_str();
    const char* value = args[2].c_str();
    size_t value_len = strlen(value);

    if (!is_legal_property_name(name)) {
        LOG(ERROR) << "do_wait_for_prop(\"" << name << "\", \"" << value
                   << "\") failed: bad name";
        return -1;
    }
    if (value_len >= PROP_VALUE_MAX) {
        LOG(ERROR) << "do_wait_for_prop(\"" << name << "\", \"" << value
                   << "\") failed: value too long";
        return -1;
    }
    if (!start_waiting_for_property(name, value)) {
        LOG(ERROR) << "do_wait_for_prop(\"" << name << "\", \"" << value
                   << "\") failed: init already in waiting";
        return -1;
    }
    return 0;
}

/*
 * Callback to make a directory from the ext4 code
 */
static int do_installkeys_ensure_dir_exists(const char* dir) {
    if (make_dir(dir, 0700) && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static bool is_file_crypto() {
    return android::base::GetProperty("ro.crypto.type", "") == "file";
}

static int do_installkey(const std::vector<std::string>& args) {
    if (!is_file_crypto()) {
        return 0;
    }
    auto unencrypted_dir = args[1] + e4crypt_unencrypted_folder;
    if (do_installkeys_ensure_dir_exists(unencrypted_dir.c_str())) {
        PLOG(ERROR) << "Failed to create " << unencrypted_dir;
        return -1;
    }
    std::vector<std::string> exec_args = {"exec", "/system/bin/vdc", "--wait", "cryptfs",
                                          "enablefilecrypto"};
    return do_exec(exec_args);
}

static int do_init_user0(const std::vector<std::string>& args) {
    return e4crypt_do_init_user0();
}

BuiltinFunctionMap::Map& BuiltinFunctionMap::map() const {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    // clang-format off
    static const Map builtin_functions = {
        {"bootchart",               {1,     1,    do_bootchart}},
        {"chmod",                   {2,     2,    do_chmod}},
        {"chown",                   {2,     3,    do_chown}},
        {"class_reset",             {1,     1,    do_class_reset}},
        {"class_restart",           {1,     1,    do_class_restart}},
        {"class_start",             {1,     1,    do_class_start}},
        {"class_stop",              {1,     1,    do_class_stop}},
        {"copy",                    {2,     2,    do_copy}},
        {"domainname",              {1,     1,    do_domainname}},
        {"enable",                  {1,     1,    do_enable}},
        {"exec",                    {1,     kMax, do_exec}},
        {"exec_start",              {1,     1,    do_exec_start}},
        {"export",                  {2,     2,    do_export}},
        {"hostname",                {1,     1,    do_hostname}},
        {"ifup",                    {1,     1,    do_ifup}},
        {"init_user0",              {0,     0,    do_init_user0}},
        {"insmod",                  {1,     kMax, do_insmod}},
        {"installkey",              {1,     1,    do_installkey}},
        {"load_persist_props",      {0,     0,    do_load_persist_props}},
        {"load_system_props",       {0,     0,    do_load_system_props}},
        {"loglevel",                {1,     1,    do_loglevel}},
        {"mkdir",                   {1,     4,    do_mkdir}},
        {"mount_all",               {1,     kMax, do_mount_all}},
        {"mount",                   {3,     kMax, do_mount}},
        {"umount",                  {1,     1,    do_umount}},
        {"restart",                 {1,     1,    do_restart}},
        {"restorecon",              {1,     kMax, do_restorecon}},
        {"restorecon_recursive",    {1,     kMax, do_restorecon_recursive}},
        {"rm",                      {1,     1,    do_rm}},
        {"rmdir",                   {1,     1,    do_rmdir}},
        {"setprop",                 {2,     2,    do_setprop}},
        {"setrlimit",               {3,     3,    do_setrlimit}},
        {"start",                   {1,     1,    do_start}},
        {"stop",                    {1,     1,    do_stop}},
        {"swapon_all",              {1,     1,    do_swapon_all}},
        {"symlink",                 {2,     2,    do_symlink}},
        {"sysclktz",                {1,     1,    do_sysclktz}},
        {"trigger",                 {1,     1,    do_trigger}},
        {"verity_load_state",       {0,     0,    do_verity_load_state}},
        {"verity_update_state",     {0,     0,    do_verity_update_state}},
        {"wait",                    {1,     2,    do_wait}},
        {"wait_for_prop",           {2,     2,    do_wait_for_prop}},
        {"write",                   {2,     2,    do_write}},
    };
    // clang-format on
    return builtin_functions;
}
