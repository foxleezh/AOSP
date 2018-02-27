/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "service.h"

#include <fcntl.h>
#include <inttypes.h>
#include <linux/securebits.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <selinux/selinux.h>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <system/thread_defs.h>

#include <processgroup/processgroup.h>

#include "action.h"
#include "init.h"
#include "init_parser.h"
#include "log.h"
#include "property_service.h"
#include "util.h"

using android::base::ParseInt;
using android::base::StringPrintf;
using android::base::WriteStringToFile;

static std::string ComputeContextFromExecutable(std::string& service_name,
                                                const std::string& service_path) {
    std::string computed_context;

    char* raw_con = nullptr;
    char* raw_filecon = nullptr;

    if (getcon(&raw_con) == -1) {
        LOG(ERROR) << "could not get context while starting '" << service_name << "'";
        return "";
    }
    std::unique_ptr<char> mycon(raw_con);

    if (getfilecon(service_path.c_str(), &raw_filecon) == -1) {
        LOG(ERROR) << "could not get file context while starting '" << service_name << "'";
        return "";
    }
    std::unique_ptr<char> filecon(raw_filecon);

    char* new_con = nullptr;
    int rc = security_compute_create(mycon.get(), filecon.get(),
                                     string_to_security_class("process"), &new_con);
    if (rc == 0) {
        computed_context = new_con;
        free(new_con);
    }
    if (rc == 0 && computed_context == mycon.get()) {
        LOG(ERROR) << "service " << service_name << " does not have a SELinux domain defined";
        return "";
    }
    if (rc < 0) {
        LOG(ERROR) << "could not get context while starting '" << service_name << "'";
        return "";
    }
    return computed_context;
}

static void SetUpPidNamespace(const std::string& service_name) {
    constexpr unsigned int kSafeFlags = MS_NODEV | MS_NOEXEC | MS_NOSUID;

    // It's OK to LOG(FATAL) in this function since it's running in the first
    // child process.
    if (mount("", "/proc", "proc", kSafeFlags | MS_REMOUNT, "") == -1) {
        PLOG(FATAL) << "couldn't remount(/proc) for " << service_name;
    }

    if (prctl(PR_SET_NAME, service_name.c_str()) == -1) {
        PLOG(FATAL) << "couldn't set name for " << service_name;
    }

    pid_t child_pid = fork();
    if (child_pid == -1) {
        PLOG(FATAL) << "couldn't fork init inside the PID namespace for " << service_name;
    }

    if (child_pid > 0) {
        // So that we exit with the right status.
        static int init_exitstatus = 0;
        signal(SIGTERM, [](int) { _exit(init_exitstatus); });

        pid_t waited_pid;
        int status;
        while ((waited_pid = wait(&status)) > 0) {
             // This loop will end when there are no processes left inside the
             // PID namespace or when the init process inside the PID namespace
             // gets a signal.
            if (waited_pid == child_pid) {
                init_exitstatus = status;
            }
        }
        if (!WIFEXITED(init_exitstatus)) {
            _exit(EXIT_FAILURE);
        }
        _exit(WEXITSTATUS(init_exitstatus));
    }
}

static void ExpandArgs(const std::vector<std::string>& args, std::vector<char*>* strs) {
    std::vector<std::string> expanded_args;
    expanded_args.resize(args.size());
    strs->push_back(const_cast<char*>(args[0].c_str()));
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (!expand_props(args[i], &expanded_args[i])) {
            LOG(FATAL) << args[0] << ": cannot expand '" << args[i] << "'";
        }
        strs->push_back(const_cast<char*>(expanded_args[i].c_str()));
    }
    strs->push_back(nullptr);
}

ServiceEnvironmentInfo::ServiceEnvironmentInfo() {
}

ServiceEnvironmentInfo::ServiceEnvironmentInfo(const std::string& name,
                                               const std::string& value)
    : name(name), value(value) {
}

Service::Service(const std::string& name, const std::vector<std::string>& args)
    : name_(name),
      classnames_({"default"}),
      flags_(0),
      pid_(0),
      crash_count_(0),
      uid_(0),
      gid_(0),
      namespace_flags_(0),
      seclabel_(""),
      keychord_id_(0),
      ioprio_class_(IoSchedClass_NONE),
      ioprio_pri_(0),
      priority_(0),
      oom_score_adjust_(-1000),
      args_(args) {
    onrestart_.InitSingleTrigger("onrestart");
}

Service::Service(const std::string& name, unsigned flags, uid_t uid, gid_t gid,
                 const std::vector<gid_t>& supp_gids, const CapSet& capabilities,
                 unsigned namespace_flags, const std::string& seclabel,
                 const std::vector<std::string>& args)
    : name_(name),
      classnames_({"default"}),
      flags_(flags),
      pid_(0),
      crash_count_(0),
      uid_(uid),
      gid_(gid),
      supp_gids_(supp_gids),
      capabilities_(capabilities),
      namespace_flags_(namespace_flags),
      seclabel_(seclabel),
      keychord_id_(0),
      ioprio_class_(IoSchedClass_NONE),
      ioprio_pri_(0),
      priority_(0),
      oom_score_adjust_(-1000),
      args_(args) {
    onrestart_.InitSingleTrigger("onrestart");
}

void Service::NotifyStateChange(const std::string& new_state) const {
    if ((flags_ & SVC_TEMPORARY) != 0) {
        // Services created by 'exec' are temporary and don't have properties tracking their state.
        return;
    }

    std::string prop_name = StringPrintf("init.svc.%s", name_.c_str());
    property_set(prop_name.c_str(), new_state.c_str());

    if (new_state == "running") {
        uint64_t start_ns = time_started_.time_since_epoch().count();
        property_set(StringPrintf("ro.boottime.%s", name_.c_str()).c_str(),
                     StringPrintf("%" PRIu64, start_ns).c_str());
    }
}

void Service::KillProcessGroup(int signal) {
    LOG(INFO) << "Sending signal " << signal
              << " to service '" << name_
              << "' (pid " << pid_ << ") process group...";
    int r;
    if (signal == SIGTERM) {
        r = killProcessGroupOnce(uid_, pid_, signal);
    } else {
        r = killProcessGroup(uid_, pid_, signal);
    }
    if (r == -1) {
        PLOG(ERROR) << "killProcessGroup(" << uid_ << ", " << pid_ << ", " << signal << ") failed";
    }
    if (kill(-pid_, signal) == -1) {
        PLOG(ERROR) << "kill(" << pid_ << ", " << signal << ") failed";
    }
}

void Service::SetProcessAttributes() {
    // Keep capabilites on uid change.
    if (capabilities_.any() && uid_) {
        if (prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS | SECBIT_KEEP_CAPS_LOCKED) != 0) {
            PLOG(FATAL) << "prtcl(PR_SET_KEEPCAPS) failed for " << name_;
        }
    }

    // TODO: work out why this fails for `console` then upgrade to FATAL.
    if (setpgid(0, getpid()) == -1) PLOG(ERROR) << "setpgid failed for " << name_;

    if (gid_) {
        if (setgid(gid_) != 0) {
            PLOG(FATAL) << "setgid failed for " << name_;
        }
    }
    if (setgroups(supp_gids_.size(), &supp_gids_[0]) != 0) {
        PLOG(FATAL) << "setgroups failed for " << name_;
    }
    if (uid_) {
        if (setuid(uid_) != 0) {
            PLOG(FATAL) << "setuid failed for " << name_;
        }
    }
    if (!seclabel_.empty()) {
        if (setexeccon(seclabel_.c_str()) < 0) {
            PLOG(FATAL) << "cannot setexeccon('" << seclabel_ << "') for " << name_;
        }
    }
    if (priority_ != 0) {
        if (setpriority(PRIO_PROCESS, 0, priority_) != 0) {
            PLOG(FATAL) << "setpriority failed for " << name_;
        }
    }
    if (capabilities_.any()) {
        if (!SetCapsForExec(capabilities_)) {
            LOG(FATAL) << "cannot set capabilities for " << name_;
        }
    }
}

void Service::Reap() {
    if (!(flags_ & SVC_ONESHOT) || (flags_ & SVC_RESTART)) {
        KillProcessGroup(SIGKILL);
    }

    // Remove any descriptor resources we may have created.
    std::for_each(descriptors_.begin(), descriptors_.end(),
                  std::bind(&DescriptorInfo::Clean, std::placeholders::_1));

    if (flags_ & SVC_TEMPORARY) {
        return;
    }

    pid_ = 0;
    flags_ &= (~SVC_RUNNING);

    // Oneshot processes go into the disabled state on exit,
    // except when manually restarted.
    if ((flags_ & SVC_ONESHOT) && !(flags_ & SVC_RESTART)) {
        flags_ |= SVC_DISABLED;
    }

    // Disabled and reset processes do not get restarted automatically.
    if (flags_ & (SVC_DISABLED | SVC_RESET))  {
        NotifyStateChange("stopped");
        return;
    }

    // If we crash > 4 times in 4 minutes, reboot into recovery.
    boot_clock::time_point now = boot_clock::now();
    if ((flags_ & SVC_CRITICAL) && !(flags_ & SVC_RESTART)) {
        if (now < time_crashed_ + 4min) {
            if (++crash_count_ > 4) {
                LOG(ERROR) << "critical process '" << name_ << "' exited 4 times in 4 minutes";
                panic();
            }
        } else {
            time_crashed_ = now;
            crash_count_ = 1;
        }
    }

    flags_ &= (~SVC_RESTART);
    flags_ |= SVC_RESTARTING;

    // Execute all onrestart commands for this service.
    onrestart_.ExecuteAllCommands();

    NotifyStateChange("restarting");
    return;
}

void Service::DumpState() const {
    LOG(INFO) << "service " << name_;
    LOG(INFO) << "  class '" << android::base::Join(classnames_, " ") << "'";
    LOG(INFO) << "  exec "<< android::base::Join(args_, " ");
    std::for_each(descriptors_.begin(), descriptors_.end(),
                  [] (const auto& info) { LOG(INFO) << *info; });
}

bool Service::ParseCapabilities(const std::vector<std::string>& args, std::string* err) {
    capabilities_ = 0;

    if (!CapAmbientSupported()) {
        *err = "capabilities requested but the kernel does not support ambient capabilities";
        return false;
    }

    unsigned int last_valid_cap = GetLastValidCap();
    if (last_valid_cap >= capabilities_.size()) {
        LOG(WARNING) << "last valid run-time capability is larger than CAP_LAST_CAP";
    }

    for (size_t i = 1; i < args.size(); i++) {
        const std::string& arg = args[i];
        int res = LookupCap(arg);
        if (res < 0) {
            *err = StringPrintf("invalid capability '%s'", arg.c_str());
            return false;
        }
        unsigned int cap = static_cast<unsigned int>(res);  // |res| is >= 0.
        if (cap > last_valid_cap) {
            *err = StringPrintf("capability '%s' not supported by the kernel", arg.c_str());
            return false;
        }
        capabilities_[cap] = true;
    }
    return true;
}

bool Service::ParseClass(const std::vector<std::string>& args, std::string* err) {
    classnames_ = std::set<std::string>(args.begin() + 1, args.end());
    return true;
}

bool Service::ParseConsole(const std::vector<std::string>& args, std::string* err) {
    flags_ |= SVC_CONSOLE;
    console_ = args.size() > 1 ? "/dev/" + args[1] : "";
    return true;
}

bool Service::ParseCritical(const std::vector<std::string>& args, std::string* err) {
    flags_ |= SVC_CRITICAL;
    return true;
}

bool Service::ParseDisabled(const std::vector<std::string>& args, std::string* err) {
    flags_ |= SVC_DISABLED;
    flags_ |= SVC_RC_DISABLED;
    return true;
}

bool Service::ParseGroup(const std::vector<std::string>& args, std::string* err) {
    gid_ = decode_uid(args[1].c_str());
    for (std::size_t n = 2; n < args.size(); n++) {
        supp_gids_.emplace_back(decode_uid(args[n].c_str()));
    }
    return true;
}

bool Service::ParsePriority(const std::vector<std::string>& args, std::string* err) {
    priority_ = 0;
    if (!ParseInt(args[1], &priority_,
                  static_cast<int>(ANDROID_PRIORITY_HIGHEST), // highest is negative
                  static_cast<int>(ANDROID_PRIORITY_LOWEST))) {
        *err = StringPrintf("process priority value must be range %d - %d",
                ANDROID_PRIORITY_HIGHEST, ANDROID_PRIORITY_LOWEST);
        return false;
    }
    return true;
}

bool Service::ParseIoprio(const std::vector<std::string>& args, std::string* err) {
    if (!ParseInt(args[2], &ioprio_pri_, 0, 7)) {
        *err = "priority value must be range 0 - 7";
        return false;
    }

    if (args[1] == "rt") {
        ioprio_class_ = IoSchedClass_RT;
    } else if (args[1] == "be") {
        ioprio_class_ = IoSchedClass_BE;
    } else if (args[1] == "idle") {
        ioprio_class_ = IoSchedClass_IDLE;
    } else {
        *err = "ioprio option usage: ioprio <rt|be|idle> <0-7>";
        return false;
    }

    return true;
}

bool Service::ParseKeycodes(const std::vector<std::string>& args, std::string* err) {
    for (std::size_t i = 1; i < args.size(); i++) {
        int code;
        if (ParseInt(args[i], &code)) {
            keycodes_.emplace_back(code);
        } else {
            LOG(WARNING) << "ignoring invalid keycode: " << args[i];
        }
    }
    return true;
}

bool Service::ParseOneshot(const std::vector<std::string>& args, std::string* err) {
    flags_ |= SVC_ONESHOT;
    return true;
}

bool Service::ParseOnrestart(const std::vector<std::string>& args, std::string* err) {
    std::vector<std::string> str_args(args.begin() + 1, args.end());
    onrestart_.AddCommand(str_args, "", 0, err);
    return true;
}

bool Service::ParseNamespace(const std::vector<std::string>& args, std::string* err) {
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "pid") {
            namespace_flags_ |= CLONE_NEWPID;
            // PID namespaces require mount namespaces.
            namespace_flags_ |= CLONE_NEWNS;
        } else if (args[i] == "mnt") {
            namespace_flags_ |= CLONE_NEWNS;
        } else {
            *err = "namespace must be 'pid' or 'mnt'";
            return false;
        }
    }
    return true;
}

bool Service::ParseOomScoreAdjust(const std::vector<std::string>& args, std::string* err) {
    if (!ParseInt(args[1], &oom_score_adjust_, -1000, 1000)) {
        *err = "oom_score_adjust value must be in range -1000 - +1000";
        return false;
    }
    return true;
}

bool Service::ParseSeclabel(const std::vector<std::string>& args, std::string* err) {
    seclabel_ = args[1];
    return true;
}

bool Service::ParseSetenv(const std::vector<std::string>& args, std::string* err) {
    envvars_.emplace_back(args[1], args[2]);
    return true;
}

template <typename T>
bool Service::AddDescriptor(const std::vector<std::string>& args, std::string* err) {
    int perm = args.size() > 3 ? std::strtoul(args[3].c_str(), 0, 8) : -1;
    uid_t uid = args.size() > 4 ? decode_uid(args[4].c_str()) : 0;
    gid_t gid = args.size() > 5 ? decode_uid(args[5].c_str()) : 0;
    std::string context = args.size() > 6 ? args[6] : "";

    auto descriptor = std::make_unique<T>(args[1], args[2], uid, gid, perm, context);

    auto old =
        std::find_if(descriptors_.begin(), descriptors_.end(),
                     [&descriptor] (const auto& other) { return descriptor.get() == other.get(); });

    if (old != descriptors_.end()) {
        *err = "duplicate descriptor " + args[1] + " " + args[2];
        return false;
    }

    descriptors_.emplace_back(std::move(descriptor));
    return true;
}

// name type perm [ uid gid context ]
bool Service::ParseSocket(const std::vector<std::string>& args, std::string* err) {
    if (args[2] != "dgram" && args[2] != "stream" && args[2] != "seqpacket") {
        *err = "socket type must be 'dgram', 'stream' or 'seqpacket'";
        return false;
    }
    return AddDescriptor<SocketInfo>(args, err);
}

// name type perm [ uid gid context ]
bool Service::ParseFile(const std::vector<std::string>& args, std::string* err) {
    if (args[2] != "r" && args[2] != "w" && args[2] != "rw") {
        *err = "file type must be 'r', 'w' or 'rw'";
        return false;
    }
    if ((args[1][0] != '/') || (args[1].find("../") != std::string::npos)) {
        *err = "file name must not be relative";
        return false;
    }
    return AddDescriptor<FileInfo>(args, err);
}

bool Service::ParseUser(const std::vector<std::string>& args, std::string* err) {
    uid_ = decode_uid(args[1].c_str());
    return true;
}

bool Service::ParseWritepid(const std::vector<std::string>& args, std::string* err) {
    writepid_files_.assign(args.begin() + 1, args.end());
    return true;
}

class Service::OptionParserMap : public KeywordMap<OptionParser> {
public:
    OptionParserMap() {
    }
private:
    Map& map() const override;
};

Service::OptionParserMap::Map& Service::OptionParserMap::map() const {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    // clang-format off
    static const Map option_parsers = {
        {"capabilities",
                        {1,     kMax, &Service::ParseCapabilities}},
        {"class",       {1,     kMax, &Service::ParseClass}},
        {"console",     {0,     1,    &Service::ParseConsole}},
        {"critical",    {0,     0,    &Service::ParseCritical}},
        {"disabled",    {0,     0,    &Service::ParseDisabled}},
        {"group",       {1,     NR_SVC_SUPP_GIDS + 1, &Service::ParseGroup}},
        {"ioprio",      {2,     2,    &Service::ParseIoprio}},
        {"priority",    {1,     1,    &Service::ParsePriority}},
        {"keycodes",    {1,     kMax, &Service::ParseKeycodes}},
        {"oneshot",     {0,     0,    &Service::ParseOneshot}},
        {"onrestart",   {1,     kMax, &Service::ParseOnrestart}},
        {"oom_score_adjust",
                        {1,     1,    &Service::ParseOomScoreAdjust}},
        {"namespace",   {1,     2,    &Service::ParseNamespace}},
        {"seclabel",    {1,     1,    &Service::ParseSeclabel}},
        {"setenv",      {2,     2,    &Service::ParseSetenv}},
        {"socket",      {3,     6,    &Service::ParseSocket}},
        {"file",        {2,     2,    &Service::ParseFile}},
        {"user",        {1,     1,    &Service::ParseUser}},
        {"writepid",    {1,     kMax, &Service::ParseWritepid}},
    };
    // clang-format on
    return option_parsers;
}

bool Service::ParseLine(const std::vector<std::string>& args, std::string* err) {
    if (args.empty()) {
        *err = "option needed, but not provided";
        return false;
    }

    static const OptionParserMap parser_map;
    auto parser = parser_map.FindFunction(args[0], args.size() - 1, err);

    if (!parser) {
        return false;
    }

    return (this->*parser)(args, err);
}

bool Service::ExecStart(std::unique_ptr<Timer>* exec_waiter) {
    flags_ |= SVC_EXEC | SVC_ONESHOT;

    exec_waiter->reset(new Timer);

    if (!Start()) {
        exec_waiter->reset();
        return false;
    }
    return true;
}

bool Service::Start() {
    // Starting a service removes it from the disabled or reset state and
    // immediately takes it out of the restarting state if it was in there.
    flags_ &= (~(SVC_DISABLED|SVC_RESTARTING|SVC_RESET|SVC_RESTART|SVC_DISABLED_START));

    // Running processes require no additional work --- if they're in the
    // process of exiting, we've ensured that they will immediately restart
    // on exit, unless they are ONESHOT.
    if (flags_ & SVC_RUNNING) {
        return false;
    }

    bool needs_console = (flags_ & SVC_CONSOLE);
    if (needs_console) {
        if (console_.empty()) {
            console_ = default_console;
        }

        // Make sure that open call succeeds to ensure a console driver is
        // properly registered for the device node
        int console_fd = open(console_.c_str(), O_RDWR | O_CLOEXEC);
        if (console_fd < 0) {
            PLOG(ERROR) << "service '" << name_ << "' couldn't open console '" << console_ << "'";
            flags_ |= SVC_DISABLED;
            return false;
        }
        close(console_fd);
    }

    struct stat sb;
    if (stat(args_[0].c_str(), &sb) == -1) {
        PLOG(ERROR) << "cannot find '" << args_[0] << "', disabling '" << name_ << "'";
        flags_ |= SVC_DISABLED;
        return false;
    }

    std::string scon;
    if (!seclabel_.empty()) {
        scon = seclabel_;
    } else {
        LOG(INFO) << "computing context for service '" << name_ << "'";
        scon = ComputeContextFromExecutable(name_, args_[0]);
        if (scon == "") {
            return false;
        }
    }

    LOG(INFO) << "starting service '" << name_ << "'...";

    pid_t pid = -1;
    if (namespace_flags_) {
        pid = clone(nullptr, nullptr, namespace_flags_ | SIGCHLD, nullptr);
    } else {
        pid = fork();
    }

    if (pid == 0) {
        umask(077);

        if (namespace_flags_ & CLONE_NEWPID) {
            // This will fork again to run an init process inside the PID
            // namespace.
            SetUpPidNamespace(name_);
        }

        for (const auto& ei : envvars_) {
            add_environment(ei.name.c_str(), ei.value.c_str());
        }

        std::for_each(descriptors_.begin(), descriptors_.end(),
                      std::bind(&DescriptorInfo::CreateAndPublish, std::placeholders::_1, scon));

        // See if there were "writepid" instructions to write to files under /dev/cpuset/.
        auto cpuset_predicate = [](const std::string& path) {
            return android::base::StartsWith(path, "/dev/cpuset/");
        };
        auto iter = std::find_if(writepid_files_.begin(), writepid_files_.end(), cpuset_predicate);
        if (iter == writepid_files_.end()) {
            // There were no "writepid" instructions for cpusets, check if the system default
            // cpuset is specified to be used for the process.
            std::string default_cpuset = android::base::GetProperty("ro.cpuset.default", "");
            if (!default_cpuset.empty()) {
                // Make sure the cpuset name starts and ends with '/'.
                // A single '/' means the 'root' cpuset.
                if (default_cpuset.front() != '/') {
                    default_cpuset.insert(0, 1, '/');
                }
                if (default_cpuset.back() != '/') {
                    default_cpuset.push_back('/');
                }
                writepid_files_.push_back(
                    StringPrintf("/dev/cpuset%stasks", default_cpuset.c_str()));
            }
        }
        std::string pid_str = StringPrintf("%d", getpid());
        for (const auto& file : writepid_files_) {
            if (!WriteStringToFile(pid_str, file)) {
                PLOG(ERROR) << "couldn't write " << pid_str << " to " << file;
            }
        }

        if (ioprio_class_ != IoSchedClass_NONE) {
            if (android_set_ioprio(getpid(), ioprio_class_, ioprio_pri_)) {
                PLOG(ERROR) << "failed to set pid " << getpid()
                            << " ioprio=" << ioprio_class_ << "," << ioprio_pri_;
            }
        }

        if (needs_console) {
            setsid();
            OpenConsole();
        } else {
            ZapStdio();
        }

        // As requested, set our gid, supplemental gids, uid, context, and
        // priority. Aborts on failure.
        SetProcessAttributes();

        std::vector<char*> strs;
        ExpandArgs(args_, &strs);
        if (execve(strs[0], (char**) &strs[0], (char**) ENV) < 0) {
            PLOG(ERROR) << "cannot execve('" << strs[0] << "')";
        }

        _exit(127);
    }

    if (pid < 0) {
        PLOG(ERROR) << "failed to fork for '" << name_ << "'";
        pid_ = 0;
        return false;
    }

    if (oom_score_adjust_ != -1000) {
        std::string oom_str = StringPrintf("%d", oom_score_adjust_);
        std::string oom_file = StringPrintf("/proc/%d/oom_score_adj", pid);
        if (!WriteStringToFile(oom_str, oom_file)) {
            PLOG(ERROR) << "couldn't write oom_score_adj: " << strerror(errno);
        }
    }

    time_started_ = boot_clock::now();
    pid_ = pid;
    flags_ |= SVC_RUNNING;

    errno = -createProcessGroup(uid_, pid_);
    if (errno != 0) {
        PLOG(ERROR) << "createProcessGroup(" << uid_ << ", " << pid_ << ") failed for service '"
                    << name_ << "'";
    }

    if ((flags_ & SVC_EXEC) != 0) {
        LOG(INFO) << android::base::StringPrintf(
            "SVC_EXEC pid %d (uid %d gid %d+%zu context %s) started; waiting...", pid_, uid_, gid_,
            supp_gids_.size(), !seclabel_.empty() ? seclabel_.c_str() : "default");
    }

    NotifyStateChange("running");
    return true;
}

bool Service::StartIfNotDisabled() {
    if (!(flags_ & SVC_DISABLED)) {
        return Start();
    } else {
        flags_ |= SVC_DISABLED_START;
    }
    return true;
}

bool Service::Enable() {
    flags_ &= ~(SVC_DISABLED | SVC_RC_DISABLED);
    if (flags_ & SVC_DISABLED_START) {
        return Start();
    }
    return true;
}

void Service::Reset() {
    StopOrReset(SVC_RESET);
}

void Service::Stop() {
    StopOrReset(SVC_DISABLED);
}

void Service::Terminate() {
    flags_ &= ~(SVC_RESTARTING | SVC_DISABLED_START);
    flags_ |= SVC_DISABLED;
    if (pid_) {
        KillProcessGroup(SIGTERM);
        NotifyStateChange("stopping");
    }
}

void Service::Restart() {
    if (flags_ & SVC_RUNNING) {
        /* Stop, wait, then start the service. */
        StopOrReset(SVC_RESTART);
    } else if (!(flags_ & SVC_RESTARTING)) {
        /* Just start the service since it's not running. */
        Start();
    } /* else: Service is restarting anyways. */
}

void Service::RestartIfNeeded(time_t* process_needs_restart_at) {
    boot_clock::time_point now = boot_clock::now();
    boot_clock::time_point next_start = time_started_ + 5s;
    if (now > next_start) {
        flags_ &= (~SVC_RESTARTING);
        Start();
        return;
    }

    time_t next_start_time_t = time(nullptr) +
        time_t(std::chrono::duration_cast<std::chrono::seconds>(next_start - now).count());
    if (next_start_time_t < *process_needs_restart_at || *process_needs_restart_at == 0) {
        *process_needs_restart_at = next_start_time_t;
    }
}

// The how field should be either SVC_DISABLED, SVC_RESET, or SVC_RESTART.
void Service::StopOrReset(int how) {
    // The service is still SVC_RUNNING until its process exits, but if it has
    // already exited it shoudn't attempt a restart yet.
    flags_ &= ~(SVC_RESTARTING | SVC_DISABLED_START);

    if ((how != SVC_DISABLED) && (how != SVC_RESET) && (how != SVC_RESTART)) {
        // An illegal flag: default to SVC_DISABLED.
        how = SVC_DISABLED;
    }

    // If the service has not yet started, prevent it from auto-starting with its class.
    if (how == SVC_RESET) {
        flags_ |= (flags_ & SVC_RC_DISABLED) ? SVC_DISABLED : SVC_RESET;
    } else {
        flags_ |= how;
    }

    if (pid_) {
        KillProcessGroup(SIGKILL);
        NotifyStateChange("stopping");
    } else {
        NotifyStateChange("stopped");
    }
}

void Service::ZapStdio() const {
    int fd;
    fd = open("/dev/null", O_RDWR);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

void Service::OpenConsole() const {
    int fd = open(console_.c_str(), O_RDWR);
    if (fd == -1) fd = open("/dev/null", O_RDWR);
    ioctl(fd, TIOCSCTTY, 0);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

int ServiceManager::exec_count_ = 0;

ServiceManager::ServiceManager() {
}

ServiceManager& ServiceManager::GetInstance() {
    static ServiceManager instance;
    return instance;
}

void ServiceManager::AddService(std::unique_ptr<Service> service) {
    Service* old_service = FindServiceByName(service->name());
    if (old_service) {
        LOG(ERROR) << "ignored duplicate definition of service '" << service->name() << "'";
        return;
    }
    services_.emplace_back(std::move(service));
}

bool ServiceManager::Exec(const std::vector<std::string>& args) {
    Service* svc = MakeExecOneshotService(args);
    if (!svc) {
        LOG(ERROR) << "Could not create exec service";
        return false;
    }
    if (!svc->ExecStart(&exec_waiter_)) {
        LOG(ERROR) << "Could not start exec service";
        ServiceManager::GetInstance().RemoveService(*svc);
        return false;
    }
    return true;
}

bool ServiceManager::ExecStart(const std::string& name) {
    Service* svc = FindServiceByName(name);
    if (!svc) {
        LOG(ERROR) << "ExecStart(" << name << "): Service not found";
        return false;
    }
    if (!svc->ExecStart(&exec_waiter_)) {
        LOG(ERROR) << "ExecStart(" << name << "): Could not start Service";
        return false;
    }
    return true;
}

bool ServiceManager::IsWaitingForExec() const { return exec_waiter_ != nullptr; }

Service* ServiceManager::MakeExecOneshotService(const std::vector<std::string>& args) {
    // Parse the arguments: exec [SECLABEL [UID [GID]*] --] COMMAND ARGS...
    // SECLABEL can be a - to denote default
    std::size_t command_arg = 1;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--") {
            command_arg = i + 1;
            break;
        }
    }
    if (command_arg > 4 + NR_SVC_SUPP_GIDS) {
        LOG(ERROR) << "exec called with too many supplementary group ids";
        return nullptr;
    }

    if (command_arg >= args.size()) {
        LOG(ERROR) << "exec called without command";
        return nullptr;
    }
    std::vector<std::string> str_args(args.begin() + command_arg, args.end());

    exec_count_++;
    std::string name = StringPrintf("exec %d (%s)", exec_count_, str_args[0].c_str());
    unsigned flags = SVC_EXEC | SVC_ONESHOT | SVC_TEMPORARY;
    CapSet no_capabilities;
    unsigned namespace_flags = 0;

    std::string seclabel = "";
    if (command_arg > 2 && args[1] != "-") {
        seclabel = args[1];
    }
    uid_t uid = 0;
    if (command_arg > 3) {
        uid = decode_uid(args[2].c_str());
    }
    gid_t gid = 0;
    std::vector<gid_t> supp_gids;
    if (command_arg > 4) {
        gid = decode_uid(args[3].c_str());
        std::size_t nr_supp_gids = command_arg - 1 /* -- */ - 4 /* exec SECLABEL UID GID */;
        for (size_t i = 0; i < nr_supp_gids; ++i) {
            supp_gids.push_back(decode_uid(args[4 + i].c_str()));
        }
    }

    auto svc_p = std::make_unique<Service>(name, flags, uid, gid, supp_gids, no_capabilities,
                                           namespace_flags, seclabel, str_args);
    Service* svc = svc_p.get();
    services_.emplace_back(std::move(svc_p));

    return svc;
}

Service* ServiceManager::FindServiceByName(const std::string& name) const {
    auto svc = std::find_if(services_.begin(), services_.end(),
                            [&name] (const std::unique_ptr<Service>& s) {
                                return name == s->name();
                            });
    if (svc != services_.end()) {
        return svc->get();
    }
    return nullptr;
}

Service* ServiceManager::FindServiceByPid(pid_t pid) const {
    auto svc = std::find_if(services_.begin(), services_.end(),
                            [&pid] (const std::unique_ptr<Service>& s) {
                                return s->pid() == pid;
                            });
    if (svc != services_.end()) {
        return svc->get();
    }
    return nullptr;
}

Service* ServiceManager::FindServiceByKeychord(int keychord_id) const {
    auto svc = std::find_if(services_.begin(), services_.end(),
                            [&keychord_id] (const std::unique_ptr<Service>& s) {
                                return s->keychord_id() == keychord_id;
                            });

    if (svc != services_.end()) {
        return svc->get();
    }
    return nullptr;
}

void ServiceManager::ForEachService(const std::function<void(Service*)>& callback) const {
    for (const auto& s : services_) {
        callback(s.get());
    }
}

void ServiceManager::ForEachServiceInClass(const std::string& classname,
                                           void (*func)(Service* svc)) const {
    for (const auto& s : services_) {
        if (s->classnames().find(classname) != s->classnames().end()) {
            func(s.get());
        }
    }
}

void ServiceManager::ForEachServiceWithFlags(unsigned matchflags,
                                             void (*func)(Service* svc)) const {
    for (const auto& s : services_) {
        if (s->flags() & matchflags) {
            func(s.get());
        }
    }
}

void ServiceManager::RemoveService(const Service& svc) {
    auto svc_it = std::find_if(services_.begin(), services_.end(),
                               [&svc] (const std::unique_ptr<Service>& s) {
                                   return svc.name() == s->name();
                               });
    if (svc_it == services_.end()) {
        return;
    }

    services_.erase(svc_it);
}

void ServiceManager::DumpState() const {
    for (const auto& s : services_) {
        s->DumpState();
    }
}

bool ServiceManager::ReapOneProcess() {
    int status;
    pid_t pid = TEMP_FAILURE_RETRY(waitpid(-1, &status, WNOHANG));
    if (pid == 0) {
        return false;
    } else if (pid == -1) {
        PLOG(ERROR) << "waitpid failed";
        return false;
    }

    Service* svc = FindServiceByPid(pid);

    std::string name;
    std::string wait_string;
    if (svc) {
        name = android::base::StringPrintf("Service '%s' (pid %d)",
                                           svc->name().c_str(), pid);
        if (svc->flags() & SVC_EXEC) {
            wait_string =
                android::base::StringPrintf(" waiting took %f seconds", exec_waiter_->duration_s());
        }
    } else {
        name = android::base::StringPrintf("Untracked pid %d", pid);
    }

    if (WIFEXITED(status)) {
        LOG(INFO) << name << " exited with status " << WEXITSTATUS(status) << wait_string;
    } else if (WIFSIGNALED(status)) {
        LOG(INFO) << name << " killed by signal " << WTERMSIG(status) << wait_string;
    } else if (WIFSTOPPED(status)) {
        LOG(INFO) << name << " stopped by signal " << WSTOPSIG(status) << wait_string;
    } else {
        LOG(INFO) << name << " state changed" << wait_string;
    }

    if (!svc) {
        return true;
    }

    svc->Reap();

    if (svc->flags() & SVC_EXEC) {
        exec_waiter_.reset();
    }
    if (svc->flags() & SVC_TEMPORARY) {
        RemoveService(*svc);
    }

    return true;
}

void ServiceManager::ReapAnyOutstandingChildren() {
    while (ReapOneProcess()) {
    }
}

bool ServiceParser::ParseSection(const std::vector<std::string>& args,
                                 std::string* err) {
    if (args.size() < 3) {
        *err = "services must have a name and a program";
        return false;
    }

    const std::string& name = args[1];
    if (!IsValidName(name)) {
        *err = StringPrintf("invalid service name '%s'", name.c_str());
        return false;
    }

    std::vector<std::string> str_args(args.begin() + 2, args.end());
    service_ = std::make_unique<Service>(name, str_args);
    return true;
}

bool ServiceParser::ParseLineSection(const std::vector<std::string>& args,
                                     const std::string& filename, int line,
                                     std::string* err) const {
    return service_ ? service_->ParseLine(args, err) : false;
}

void ServiceParser::EndSection() {
    if (service_) {
        ServiceManager::GetInstance().AddService(std::move(service_));
    }
}

bool ServiceParser::IsValidName(const std::string& name) const {
    // Property names can be any length, but may only contain certain characters.
    // Property values can contain any characters, but may only be a certain length.
    // (The latter restriction is needed because `start` and `stop` work by writing
    // the service name to the "ctl.start" and "ctl.stop" properties.)
    return is_legal_property_name("init.svc." + name) && name.size() <= PROP_VALUE_MAX;
}
