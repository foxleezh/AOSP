/*
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_TAG "Linux"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/capability.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <termios.h>
#include <unistd.h>

#include <memory>

#include <android-base/file.h>
#include <android-base/strings.h>
#include <log/log.h>

#include "AsynchronousCloseMonitor.h"
#include "ExecStrings.h"
#include "JNIHelp.h"
#include "JniConstants.h"
#include "JniException.h"
#include "NetworkUtilities.h"
#include "Portability.h"
#include "ScopedBytes.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "ScopedUtfChars.h"
#include "toStringArray.h"

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

#define TO_JAVA_STRING(NAME, EXP) \
        jstring NAME = env->NewStringUTF(EXP); \
        if ((NAME) == NULL) return NULL;

struct addrinfo_deleter {
    void operator()(addrinfo* p) const {
        if (p != NULL) { // bionic's freeaddrinfo(3) crashes when passed NULL.
            freeaddrinfo(p);
        }
    }
};

struct c_deleter {
    void operator()(void* p) const {
        free(p);
    }
};

static bool isIPv4MappedAddress(const sockaddr *sa) {
    const sockaddr_in6 *sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
    return sa != NULL && sa->sa_family == AF_INET6 &&
           (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr) ||
            IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr));  // We map 0.0.0.0 to ::, so :: is mapped.
}

/**
 * Perform a socket operation that specifies an IP address, possibly falling back from specifying
 * the address as an IPv4-mapped IPv6 address in a struct sockaddr_in6 to specifying it as an IPv4
 * address in a struct sockaddr_in.
 *
 * This is needed because all sockets created by the java.net APIs are IPv6 sockets, and on those
 * sockets, IPv4 operations use IPv4-mapped addresses stored in a struct sockaddr_in6. But sockets
 * created using Linux.socket(AF_INET, ...) are IPv4 sockets and only support operations using IPv4
 * socket addresses structures.
 */
#define NET_IPV4_FALLBACK(jni_env, return_type, syscall_name, java_fd, java_addr, port, null_addr_ok, args...) ({ \
    return_type _rc = -1; \
    do { \
        sockaddr_storage _ss; \
        socklen_t _salen; \
        if ((java_addr) == NULL && (null_addr_ok)) { \
            /* No IP address specified (e.g., sendto() on a connected socket). */ \
            _salen = 0; \
        } else if (!inetAddressToSockaddr(jni_env, java_addr, port, _ss, _salen)) { \
            /* Invalid socket address, return -1. inetAddressToSockaddr has already thrown. */ \
            break; \
        } \
        sockaddr* _sa = _salen ? reinterpret_cast<sockaddr*>(&_ss) : NULL; \
        /* inetAddressToSockaddr always returns an IPv6 sockaddr. Assume that java_fd was created \
         * by Java API calls, which always create IPv6 socket fds, and pass it in as is. */ \
        _rc = NET_FAILURE_RETRY(jni_env, return_type, syscall_name, java_fd, ##args, _sa, _salen); \
        if (_rc == -1 && errno == EAFNOSUPPORT && _salen && isIPv4MappedAddress(_sa)) { \
            /* We passed in an IPv4 address in an IPv6 sockaddr and the kernel told us that we got \
             * the address family wrong. Pass in the same address in an IPv4 sockaddr. */ \
            (jni_env)->ExceptionClear(); \
            if (!inetAddressToSockaddrVerbatim(jni_env, java_addr, port, _ss, _salen)) { \
                break; \
            } \
            _sa = reinterpret_cast<sockaddr*>(&_ss); \
            _rc = NET_FAILURE_RETRY(jni_env, return_type, syscall_name, java_fd, ##args, _sa, _salen); \
        } \
    } while (0); \
    _rc; }) \

/**
 * Used to retry networking system calls that can be interrupted with a signal. Unlike
 * TEMP_FAILURE_RETRY, this also handles the case where
 * AsynchronousCloseMonitor::signalBlockedThreads(fd) is used to signal a close() or
 * Thread.interrupt(). Other signals that result in an EINTR result are ignored and the system call
 * is retried.
 *
 * Returns the result of the system call though a Java exception will be pending if the result is
 * -1:  a SocketException if signaled via AsynchronousCloseMonitor, or ErrnoException for other
 * failures.
 */
#define NET_FAILURE_RETRY(jni_env, return_type, syscall_name, java_fd, ...) ({ \
    return_type _rc = -1; \
    int _syscallErrno; \
    do { \
        bool _wasSignaled; \
        { \
            int _fd = jniGetFDFromFileDescriptor(jni_env, java_fd); \
            AsynchronousCloseMonitor _monitor(_fd); \
            _rc = syscall_name(_fd, __VA_ARGS__); \
            _syscallErrno = errno; \
            _wasSignaled = _monitor.wasSignaled(); \
        } \
        if (_wasSignaled) { \
            jniThrowException(jni_env, "java/net/SocketException", "Socket closed"); \
            _rc = -1; \
            break; \
        } \
        if (_rc == -1 && _syscallErrno != EINTR) { \
            /* TODO: with a format string we could show the arguments too, like strace(1). */ \
            throwErrnoException(jni_env, # syscall_name); \
            break; \
        } \
    } while (_rc == -1); /* _syscallErrno == EINTR && !_wasSignaled */ \
    if (_rc == -1) { \
        /* If the syscall failed, re-set errno: throwing an exception might have modified it. */ \
        errno = _syscallErrno; \
    } \
    _rc; })

/**
 * Used to retry system calls that can be interrupted with a signal. Unlike TEMP_FAILURE_RETRY, this
 * also handles the case where AsynchronousCloseMonitor::signalBlockedThreads(fd) is used to signal
 * a close() or Thread.interrupt(). Other signals that result in an EINTR result are ignored and the
 * system call is retried.
 *
 * Returns the result of the system call though a Java exception will be pending if the result is
 * -1: an IOException if the file descriptor is already closed, a InterruptedIOException if signaled
 * via AsynchronousCloseMonitor, or ErrnoException for other failures.
 */
#define IO_FAILURE_RETRY(jni_env, return_type, syscall_name, java_fd, ...) ({ \
    return_type _rc = -1; \
    int _syscallErrno; \
    do { \
        bool _wasSignaled; \
        { \
            int _fd = jniGetFDFromFileDescriptor(jni_env, java_fd); \
            AsynchronousCloseMonitor _monitor(_fd); \
            _rc = syscall_name(_fd, __VA_ARGS__); \
            _syscallErrno = errno; \
            _wasSignaled = _monitor.wasSignaled(); \
        } \
        if (_wasSignaled) { \
            jniThrowException(jni_env, "java/io/InterruptedIOException", # syscall_name " interrupted"); \
            _rc = -1; \
            break; \
        } \
        if (_rc == -1 && _syscallErrno != EINTR) { \
            /* TODO: with a format string we could show the arguments too, like strace(1). */ \
            throwErrnoException(jni_env, # syscall_name); \
            break; \
        } \
    } while (_rc == -1); /* && _syscallErrno == EINTR && !_wasSignaled */ \
    if (_rc == -1) { \
        /* If the syscall failed, re-set errno: throwing an exception might have modified it. */ \
        errno = _syscallErrno; \
    } \
    _rc; })

#define NULL_ADDR_OK         true
#define NULL_ADDR_FORBIDDEN  false

static void throwException(JNIEnv* env, jclass exceptionClass, jmethodID ctor3, jmethodID ctor2,
        const char* functionName, int error) {
    jthrowable cause = NULL;
    if (env->ExceptionCheck()) {
        cause = env->ExceptionOccurred();
        env->ExceptionClear();
    }

    ScopedLocalRef<jstring> detailMessage(env, env->NewStringUTF(functionName));
    if (detailMessage.get() == NULL) {
        // Not really much we can do here. We're probably dead in the water,
        // but let's try to stumble on...
        env->ExceptionClear();
    }

    jobject exception;
    if (cause != NULL) {
        exception = env->NewObject(exceptionClass, ctor3, detailMessage.get(), error, cause);
    } else {
        exception = env->NewObject(exceptionClass, ctor2, detailMessage.get(), error);
    }
    env->Throw(reinterpret_cast<jthrowable>(exception));
}

static void throwErrnoException(JNIEnv* env, const char* functionName) {
    int error = errno;
    static jmethodID ctor3 = env->GetMethodID(JniConstants::errnoExceptionClass,
            "<init>", "(Ljava/lang/String;ILjava/lang/Throwable;)V");
    static jmethodID ctor2 = env->GetMethodID(JniConstants::errnoExceptionClass,
            "<init>", "(Ljava/lang/String;I)V");
    throwException(env, JniConstants::errnoExceptionClass, ctor3, ctor2, functionName, error);
}

static void throwGaiException(JNIEnv* env, const char* functionName, int error) {
  // Cache the methods ids before we throw, so we don't call GetMethodID with a pending exception.
  static jmethodID ctor3 = env->GetMethodID(JniConstants::gaiExceptionClass, "<init>",
                                            "(Ljava/lang/String;ILjava/lang/Throwable;)V");
  static jmethodID ctor2 = env->GetMethodID(JniConstants::gaiExceptionClass, "<init>",
                                            "(Ljava/lang/String;I)V");
  if (errno != 0) {
        // EAI_SYSTEM should mean "look at errno instead", but both glibc and bionic seem to
        // mess this up. In particular, if you don't have INTERNET permission, errno will be EACCES
        // but you'll get EAI_NONAME or EAI_NODATA. So we want our GaiException to have a
        // potentially-relevant ErrnoException as its cause even if error != EAI_SYSTEM.
        // http://code.google.com/p/android/issues/detail?id=15722
        throwErrnoException(env, functionName);
        // Deliberately fall through to throw another exception...
    }
    throwException(env, JniConstants::gaiExceptionClass, ctor3, ctor2, functionName, error);
}

template <typename rc_t>
static rc_t throwIfMinusOne(JNIEnv* env, const char* name, rc_t rc) {
    if (rc == rc_t(-1)) {
        throwErrnoException(env, name);
    }
    return rc;
}

template <typename ScopedT>
class IoVec {
public:
    IoVec(JNIEnv* env, size_t bufferCount) : mEnv(env), mBufferCount(bufferCount) {
    }

    bool init(jobjectArray javaBuffers, jintArray javaOffsets, jintArray javaByteCounts) {
        // We can't delete our local references until after the I/O, so make sure we have room.
        if (mEnv->PushLocalFrame(mBufferCount + 16) < 0) {
            return false;
        }
        ScopedIntArrayRO offsets(mEnv, javaOffsets);
        if (offsets.get() == NULL) {
            return false;
        }
        ScopedIntArrayRO byteCounts(mEnv, javaByteCounts);
        if (byteCounts.get() == NULL) {
            return false;
        }
        // TODO: Linux actually has a 1024 buffer limit. glibc works around this, and we should too.
        // TODO: you can query the limit at runtime with sysconf(_SC_IOV_MAX).
        for (size_t i = 0; i < mBufferCount; ++i) {
            jobject buffer = mEnv->GetObjectArrayElement(javaBuffers, i); // We keep this local ref.
            mScopedBuffers.push_back(new ScopedT(mEnv, buffer));
            jbyte* ptr = const_cast<jbyte*>(mScopedBuffers.back()->get());
            if (ptr == NULL) {
                return false;
            }
            struct iovec iov;
            iov.iov_base = reinterpret_cast<void*>(ptr + offsets[i]);
            iov.iov_len = byteCounts[i];
            mIoVec.push_back(iov);
        }
        return true;
    }

    ~IoVec() {
        for (size_t i = 0; i < mScopedBuffers.size(); ++i) {
            delete mScopedBuffers[i];
        }
        mEnv->PopLocalFrame(NULL);
    }

    iovec* get() {
        return &mIoVec[0];
    }

    size_t size() {
        return mBufferCount;
    }

private:
    JNIEnv* mEnv;
    size_t mBufferCount;
    std::vector<iovec> mIoVec;
    std::vector<ScopedT*> mScopedBuffers;
};

/**
 * Returns a jbyteArray containing the sockaddr_un.sun_path from ss. As per unix(7) sa_len should be
 * the length of ss as returned by getsockname(2), getpeername(2), or accept(2).
 * If the returned array is of length 0 the sockaddr_un refers to an unnamed socket.
 * A null pointer is returned in the event of an error. See unix(7) for more information.
 */
static jbyteArray getUnixSocketPath(JNIEnv* env, const sockaddr_storage& ss,
        const socklen_t& sa_len) {
    if (ss.ss_family != AF_UNIX) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
                "getUnixSocketPath unsupported ss_family: %i", ss.ss_family);
        return NULL;
    }

    const struct sockaddr_un* un_addr = reinterpret_cast<const struct sockaddr_un*>(&ss);
    // The length of sun_path is sa_len minus the length of the overhead (ss_family).
    // See unix(7) for details. This calculation must match that of socket_make_sockaddr_un() in
    // socket_local_client.c and javaUnixSocketAddressToSockaddr() to interoperate.
    size_t pathLength = sa_len - offsetof(struct sockaddr_un, sun_path);

    jbyteArray javaSunPath = env->NewByteArray(pathLength);
    if (javaSunPath == NULL) {
        return NULL;
    }

    if (pathLength > 0) {
        env->SetByteArrayRegion(javaSunPath, 0, pathLength,
                reinterpret_cast<const jbyte*>(&un_addr->sun_path));
    }
    return javaSunPath;
}

static jobject makeSocketAddress(JNIEnv* env, const sockaddr_storage& ss, const socklen_t sa_len) {
    if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6) {
        jint port;
        jobject inetAddress = sockaddrToInetAddress(env, ss, &port);
        if (inetAddress == NULL) {
            return NULL;  // Exception already thrown.
        }
        static jmethodID ctor = env->GetMethodID(JniConstants::inetSocketAddressClass,
                "<init>", "(Ljava/net/InetAddress;I)V");
        return env->NewObject(JniConstants::inetSocketAddressClass, ctor, inetAddress, port);
    } else if (ss.ss_family == AF_UNIX) {
        static jmethodID ctor = env->GetMethodID(JniConstants::unixSocketAddressClass,
                "<init>", "([B)V");

        jbyteArray javaSunPath = getUnixSocketPath(env, ss, sa_len);
        if (!javaSunPath) {
            return NULL;
        }
        return env->NewObject(JniConstants::unixSocketAddressClass, ctor, javaSunPath);
    } else if (ss.ss_family == AF_NETLINK) {
        const struct sockaddr_nl* nl_addr = reinterpret_cast<const struct sockaddr_nl*>(&ss);
        static jmethodID ctor = env->GetMethodID(JniConstants::netlinkSocketAddressClass,
                "<init>", "(II)V");
        return env->NewObject(JniConstants::netlinkSocketAddressClass, ctor,
                static_cast<jint>(nl_addr->nl_pid),
                static_cast<jint>(nl_addr->nl_groups));
    } else if (ss.ss_family == AF_PACKET) {
        const struct sockaddr_ll* sll = reinterpret_cast<const struct sockaddr_ll*>(&ss);
        static jmethodID ctor = env->GetMethodID(JniConstants::packetSocketAddressClass,
                "<init>", "(SISB[B)V");
        ScopedLocalRef<jbyteArray> byteArray(env, env->NewByteArray(sll->sll_halen));
        if (byteArray.get() == NULL) {
            return NULL;
        }
        env->SetByteArrayRegion(byteArray.get(), 0, sll->sll_halen,
                reinterpret_cast<const jbyte*>(sll->sll_addr));
        jobject packetSocketAddress = env->NewObject(JniConstants::packetSocketAddressClass, ctor,
                static_cast<jshort>(ntohs(sll->sll_protocol)),
                static_cast<jint>(sll->sll_ifindex),
                static_cast<jshort>(sll->sll_hatype),
                static_cast<jbyte>(sll->sll_pkttype),
                byteArray.get());
        return packetSocketAddress;
    }
    jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "unsupported ss_family: %d",
            ss.ss_family);
    return NULL;
}

static jobject makeStructPasswd(JNIEnv* env, const struct passwd& pw) {
    TO_JAVA_STRING(pw_name, pw.pw_name);
    TO_JAVA_STRING(pw_dir, pw.pw_dir);
    TO_JAVA_STRING(pw_shell, pw.pw_shell);
    static jmethodID ctor = env->GetMethodID(JniConstants::structPasswdClass, "<init>",
            "(Ljava/lang/String;IILjava/lang/String;Ljava/lang/String;)V");
    return env->NewObject(JniConstants::structPasswdClass, ctor,
            pw_name, static_cast<jint>(pw.pw_uid), static_cast<jint>(pw.pw_gid), pw_dir, pw_shell);
}

static jobject makeStructStat(JNIEnv* env, const struct stat64& sb) {
    static jmethodID ctor = env->GetMethodID(JniConstants::structStatClass, "<init>",
            "(JJIJIIJJJJJJJ)V");
    return env->NewObject(JniConstants::structStatClass, ctor,
            static_cast<jlong>(sb.st_dev), static_cast<jlong>(sb.st_ino),
            static_cast<jint>(sb.st_mode), static_cast<jlong>(sb.st_nlink),
            static_cast<jint>(sb.st_uid), static_cast<jint>(sb.st_gid),
            static_cast<jlong>(sb.st_rdev), static_cast<jlong>(sb.st_size),
            static_cast<jlong>(sb.st_atime), static_cast<jlong>(sb.st_mtime),
            static_cast<jlong>(sb.st_ctime), static_cast<jlong>(sb.st_blksize),
            static_cast<jlong>(sb.st_blocks));
}

static jobject makeStructStatVfs(JNIEnv* env, const struct statvfs& sb) {
    static jmethodID ctor = env->GetMethodID(JniConstants::structStatVfsClass, "<init>",
            "(JJJJJJJJJJJ)V");
    return env->NewObject(JniConstants::structStatVfsClass, ctor,
                          static_cast<jlong>(sb.f_bsize),
                          static_cast<jlong>(sb.f_frsize),
                          static_cast<jlong>(sb.f_blocks),
                          static_cast<jlong>(sb.f_bfree),
                          static_cast<jlong>(sb.f_bavail),
                          static_cast<jlong>(sb.f_files),
                          static_cast<jlong>(sb.f_ffree),
                          static_cast<jlong>(sb.f_favail),
                          static_cast<jlong>(sb.f_fsid),
                          static_cast<jlong>(sb.f_flag),
                          static_cast<jlong>(sb.f_namemax));
}

static jobject makeStructLinger(JNIEnv* env, const struct linger& l) {
    static jmethodID ctor = env->GetMethodID(JniConstants::structLingerClass, "<init>", "(II)V");
    return env->NewObject(JniConstants::structLingerClass, ctor, l.l_onoff, l.l_linger);
}

static jobject makeStructTimeval(JNIEnv* env, const struct timeval& tv) {
    static jmethodID ctor = env->GetMethodID(JniConstants::structTimevalClass, "<init>", "(JJ)V");
    return env->NewObject(JniConstants::structTimevalClass, ctor,
            static_cast<jlong>(tv.tv_sec), static_cast<jlong>(tv.tv_usec));
}

static jobject makeStructUcred(JNIEnv* env, const struct ucred& u __unused) {
  static jmethodID ctor = env->GetMethodID(JniConstants::structUcredClass, "<init>", "(III)V");
  return env->NewObject(JniConstants::structUcredClass, ctor, u.pid, u.uid, u.gid);
}

static jobject makeStructUtsname(JNIEnv* env, const struct utsname& buf) {
    TO_JAVA_STRING(sysname, buf.sysname);
    TO_JAVA_STRING(nodename, buf.nodename);
    TO_JAVA_STRING(release, buf.release);
    TO_JAVA_STRING(version, buf.version);
    TO_JAVA_STRING(machine, buf.machine);
    static jmethodID ctor = env->GetMethodID(JniConstants::structUtsnameClass, "<init>",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    return env->NewObject(JniConstants::structUtsnameClass, ctor,
            sysname, nodename, release, version, machine);
};

static bool fillIfreq(JNIEnv* env, jstring javaInterfaceName, struct ifreq& req) {
    ScopedUtfChars interfaceName(env, javaInterfaceName);
    if (interfaceName.c_str() == NULL) {
        return false;
    }
    memset(&req, 0, sizeof(req));
    strncpy(req.ifr_name, interfaceName.c_str(), sizeof(req.ifr_name));
    req.ifr_name[sizeof(req.ifr_name) - 1] = '\0';
    return true;
}

static bool fillUnixSocketAddress(JNIEnv* env, jobject javaUnixSocketAddress,
        const sockaddr_storage& ss, const socklen_t& sa_len) {
    if (javaUnixSocketAddress == NULL) {
        return true;
    }
    jbyteArray javaSunPath = getUnixSocketPath(env, ss, sa_len);
    if (!javaSunPath) {
        return false;
    }

    static jfieldID sunPathFid =
            env->GetFieldID(JniConstants::unixSocketAddressClass, "sun_path", "[B");
    env->SetObjectField(javaUnixSocketAddress, sunPathFid, javaSunPath);
    return true;
}

static bool fillInetSocketAddress(JNIEnv* env, jobject javaInetSocketAddress,
        const sockaddr_storage& ss) {
    if (javaInetSocketAddress == NULL) {
        return true;
    }
    // Fill out the passed-in InetSocketAddress with the sender's IP address and port number.
    jint port;
    jobject sender = sockaddrToInetAddress(env, ss, &port);
    if (sender == NULL) {
        return false;
    }
    static jfieldID holderFid = env->GetFieldID(JniConstants::inetSocketAddressClass, "holder",
                                                "Ljava/net/InetSocketAddress$InetSocketAddressHolder;");
    jobject holder = env->GetObjectField(javaInetSocketAddress, holderFid);

    static jfieldID addressFid = env->GetFieldID(JniConstants::inetSocketAddressHolderClass,
                                                 "addr", "Ljava/net/InetAddress;");
    static jfieldID portFid = env->GetFieldID(JniConstants::inetSocketAddressHolderClass, "port", "I");
    env->SetObjectField(holder, addressFid, sender);
    env->SetIntField(holder, portFid, port);
    return true;
}

static bool fillSocketAddress(JNIEnv* env, jobject javaSocketAddress, const sockaddr_storage& ss,
        const socklen_t& sa_len) {
    if (javaSocketAddress == NULL) {
        return true;
    }

    if (env->IsInstanceOf(javaSocketAddress, JniConstants::inetSocketAddressClass)) {
        return fillInetSocketAddress(env, javaSocketAddress, ss);
    } else if (env->IsInstanceOf(javaSocketAddress, JniConstants::unixSocketAddressClass)) {
        return fillUnixSocketAddress(env, javaSocketAddress, ss, sa_len);
    }
    jniThrowException(env, "java/lang/UnsupportedOperationException",
            "unsupported SocketAddress subclass");
    return false;

}

static void javaInetSocketAddressToInetAddressAndPort(
        JNIEnv* env, jobject javaInetSocketAddress, jobject& javaInetAddress, jint& port) {
    static jfieldID holderFid = env->GetFieldID(JniConstants::inetSocketAddressClass, "holder",
                                                "Ljava/net/InetSocketAddress$InetSocketAddressHolder;");
    jobject holder = env->GetObjectField(javaInetSocketAddress, holderFid);

    static jfieldID addressFid = env->GetFieldID(
            JniConstants::inetSocketAddressHolderClass, "addr", "Ljava/net/InetAddress;");
    static jfieldID portFid = env->GetFieldID(JniConstants::inetSocketAddressHolderClass, "port", "I");

    javaInetAddress = env->GetObjectField(holder, addressFid);
    port = env->GetIntField(holder, portFid);
}

static bool javaInetSocketAddressToSockaddr(
        JNIEnv* env, jobject javaSocketAddress, sockaddr_storage& ss, socklen_t& sa_len) {
    jobject javaInetAddress;
    jint port;
    javaInetSocketAddressToInetAddressAndPort(env, javaSocketAddress, javaInetAddress, port);
    return inetAddressToSockaddr(env, javaInetAddress, port, ss, sa_len);
}

static bool javaNetlinkSocketAddressToSockaddr(
        JNIEnv* env, jobject javaSocketAddress, sockaddr_storage& ss, socklen_t& sa_len) {
    static jfieldID nlPidFid = env->GetFieldID(
            JniConstants::netlinkSocketAddressClass, "nlPortId", "I");
    static jfieldID nlGroupsFid = env->GetFieldID(
            JniConstants::netlinkSocketAddressClass, "nlGroupsMask", "I");

    sockaddr_nl *nlAddr = reinterpret_cast<sockaddr_nl *>(&ss);
    nlAddr->nl_family = AF_NETLINK;
    nlAddr->nl_pid = env->GetIntField(javaSocketAddress, nlPidFid);
    nlAddr->nl_groups = env->GetIntField(javaSocketAddress, nlGroupsFid);
    sa_len = sizeof(sockaddr_nl);
    return true;
}

static bool javaUnixSocketAddressToSockaddr(
        JNIEnv* env, jobject javaUnixSocketAddress, sockaddr_storage& ss, socklen_t& sa_len) {
    static jfieldID sunPathFid = env->GetFieldID(
            JniConstants::unixSocketAddressClass, "sun_path", "[B");

    struct sockaddr_un* un_addr = reinterpret_cast<struct sockaddr_un*>(&ss);
    memset (un_addr, 0, sizeof(sockaddr_un));
    un_addr->sun_family = AF_UNIX;

    jbyteArray javaSunPath = (jbyteArray) env->GetObjectField(javaUnixSocketAddress, sunPathFid);
    jsize pathLength = env->GetArrayLength(javaSunPath);
    if ((size_t) pathLength > sizeof(sockaddr_un::sun_path)) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
                "sun_path too long: max=%i, is=%i",
                sizeof(sockaddr_un::sun_path), pathLength);
        return false;
    }
    env->GetByteArrayRegion(javaSunPath, 0, pathLength, (jbyte*) un_addr->sun_path);
    // sa_len is sun_path plus the length of the overhead (ss_family_t). See unix(7) for
    // details. This calculation must match that of socket_make_sockaddr_un() in
    // socket_local_client.c and getUnixSocketPath() to interoperate.
    sa_len = offsetof(struct sockaddr_un, sun_path) + pathLength;
    return true;
}

static bool javaPacketSocketAddressToSockaddr(
        JNIEnv* env, jobject javaSocketAddress, sockaddr_storage& ss, socklen_t& sa_len) {
    static jfieldID protocolFid = env->GetFieldID(
            JniConstants::packetSocketAddressClass, "sll_protocol", "S");
    static jfieldID ifindexFid = env->GetFieldID(
            JniConstants::packetSocketAddressClass, "sll_ifindex", "I");
    static jfieldID hatypeFid = env->GetFieldID(
            JniConstants::packetSocketAddressClass, "sll_hatype", "S");
    static jfieldID pkttypeFid = env->GetFieldID(
            JniConstants::packetSocketAddressClass, "sll_pkttype", "B");
    static jfieldID addrFid = env->GetFieldID(
            JniConstants::packetSocketAddressClass, "sll_addr", "[B");

    sockaddr_ll *sll = reinterpret_cast<sockaddr_ll *>(&ss);
    sll->sll_family = AF_PACKET;
    sll->sll_protocol = htons(env->GetShortField(javaSocketAddress, protocolFid));
    sll->sll_ifindex = env->GetIntField(javaSocketAddress, ifindexFid);
    sll->sll_hatype = env->GetShortField(javaSocketAddress, hatypeFid);
    sll->sll_pkttype = env->GetByteField(javaSocketAddress, pkttypeFid);

    jbyteArray sllAddr = (jbyteArray) env->GetObjectField(javaSocketAddress, addrFid);
    if (sllAddr == NULL) {
        sll->sll_halen = 0;
        memset(&sll->sll_addr, 0, sizeof(sll->sll_addr));
    } else {
        jsize len = env->GetArrayLength(sllAddr);
        if ((size_t) len > sizeof(sll->sll_addr)) {
            len = sizeof(sll->sll_addr);
        }
        sll->sll_halen = len;
        env->GetByteArrayRegion(sllAddr, 0, len, (jbyte*) sll->sll_addr);
    }
    sa_len = sizeof(sockaddr_ll);
    return true;
}

static bool javaSocketAddressToSockaddr(
        JNIEnv* env, jobject javaSocketAddress, sockaddr_storage& ss, socklen_t& sa_len) {
    if (javaSocketAddress == NULL) {
        jniThrowNullPointerException(env, NULL);
        return false;
    }

    if (env->IsInstanceOf(javaSocketAddress, JniConstants::netlinkSocketAddressClass)) {
        return javaNetlinkSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len);
    } else if (env->IsInstanceOf(javaSocketAddress, JniConstants::inetSocketAddressClass)) {
        return javaInetSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len);
    } else if (env->IsInstanceOf(javaSocketAddress, JniConstants::packetSocketAddressClass)) {
        return javaPacketSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len);
    } else if (env->IsInstanceOf(javaSocketAddress, JniConstants::unixSocketAddressClass)) {
        return javaUnixSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len);
    }
    jniThrowException(env, "java/lang/UnsupportedOperationException",
            "unsupported SocketAddress subclass");
    return false;
}

static jobject doStat(JNIEnv* env, jstring javaPath, bool isLstat) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }
    struct stat64 sb;
    int rc = isLstat ? TEMP_FAILURE_RETRY(lstat64(path.c_str(), &sb))
                     : TEMP_FAILURE_RETRY(stat64(path.c_str(), &sb));
    if (rc == -1) {
        throwErrnoException(env, isLstat ? "lstat" : "stat");
        return NULL;
    }
    return makeStructStat(env, sb);
}

static jobject doGetSockName(JNIEnv* env, jobject javaFd, bool is_sockname) {
  int fd = jniGetFDFromFileDescriptor(env, javaFd);
  sockaddr_storage ss;
  sockaddr* sa = reinterpret_cast<sockaddr*>(&ss);
  socklen_t byteCount = sizeof(ss);
  memset(&ss, 0, byteCount);
  int rc = is_sockname ? TEMP_FAILURE_RETRY(getsockname(fd, sa, &byteCount))
      : TEMP_FAILURE_RETRY(getpeername(fd, sa, &byteCount));
  if (rc == -1) {
    throwErrnoException(env, is_sockname ? "getsockname" : "getpeername");
    return NULL;
  }
  return makeSocketAddress(env, ss, byteCount);
}

class Passwd {
public:
    explicit Passwd(JNIEnv* env) : mEnv(env), mResult(NULL) {
        mBufferSize = sysconf(_SC_GETPW_R_SIZE_MAX);
        mBuffer.reset(new char[mBufferSize]);
    }

    jobject getpwnam(const char* name) {
        return process("getpwnam_r", getpwnam_r(name, &mPwd, mBuffer.get(), mBufferSize, &mResult));
    }

    jobject getpwuid(uid_t uid) {
        return process("getpwuid_r", getpwuid_r(uid, &mPwd, mBuffer.get(), mBufferSize, &mResult));
    }

    struct passwd* get() {
        return mResult;
    }

private:
    jobject process(const char* syscall, int error) {
        if (mResult == NULL) {
            errno = error;
            throwErrnoException(mEnv, syscall);
            return NULL;
        }
        return makeStructPasswd(mEnv, *mResult);
    }

    JNIEnv* mEnv;
    std::unique_ptr<char[]> mBuffer;
    size_t mBufferSize;
    struct passwd mPwd;
    struct passwd* mResult;
};

static void AssertException(JNIEnv* env) {
    if (env->ExceptionCheck() == JNI_FALSE) {
        env->FatalError("Expected exception");
    }
}

// Note for capabilities functions:
// We assume the calls are rare enough that it does not make sense to cache class objects. The
// advantage is lower maintenance burden.

static bool ReadStructCapUserHeader(
        JNIEnv* env, jobject java_header, __user_cap_header_struct* c_header) {
    if (java_header == nullptr) {
        jniThrowNullPointerException(env, "header is null");
        return false;
    }

    ScopedLocalRef<jclass> header_class(env, env->FindClass("android/system/StructCapUserHeader"));
    if (header_class.get() == nullptr) {
        return false;
    }

    {
        static jfieldID version_fid = env->GetFieldID(header_class.get(), "version", "I");
        if (version_fid == nullptr) {
            return false;
        }
        c_header->version = env->GetIntField(java_header, version_fid);
    }

    {
        static jfieldID pid_fid = env->GetFieldID(header_class.get(), "pid", "I");
        if (pid_fid == nullptr) {
            return false;
        }
        c_header->pid = env->GetIntField(java_header, pid_fid);
    }

    return true;
}

static void SetStructCapUserHeaderVersion(
        JNIEnv* env, jobject java_header, __user_cap_header_struct* c_header) {
    ScopedLocalRef<jclass> header_class(env, env->FindClass("android/system/StructCapUserHeader"));
    if (header_class.get() == nullptr) {
        env->ExceptionClear();
        return;
    }

    static jfieldID version_fid = env->GetFieldID(header_class.get(), "version", "I");
    if (version_fid == nullptr) {
        env->ExceptionClear();
        return;
    }
    env->SetIntField(java_header, version_fid, c_header->version);
}

static jobject CreateStructCapUserData(
        JNIEnv* env, jclass data_class, __user_cap_data_struct* c_data) {
    if (c_data == nullptr) {
        // Should not happen.
        jniThrowNullPointerException(env, "data is null");
        return nullptr;
    }

    static jmethodID data_cons = env->GetMethodID(data_class, "<init>", "(III)V");
    if (data_cons == nullptr) {
        return nullptr;
    }

    jint e = static_cast<jint>(c_data->effective);
    jint p = static_cast<jint>(c_data->permitted);
    jint i = static_cast<jint>(c_data->inheritable);
    return env->NewObject(data_class, data_cons, e, p, i);
}

static bool ReadStructCapUserData(JNIEnv* env, jobject java_data, __user_cap_data_struct* c_data) {
    if (java_data == nullptr) {
        jniThrowNullPointerException(env, "data is null");
        return false;
    }

    ScopedLocalRef<jclass> data_class(env, env->FindClass("android/system/StructCapUserData"));
    if (data_class.get() == nullptr) {
        return false;
    }

    {
        static jfieldID effective_fid = env->GetFieldID(data_class.get(), "effective", "I");
        if (effective_fid == nullptr) {
            return false;
        }
        c_data->effective = env->GetIntField(java_data, effective_fid);
    }

    {
        static jfieldID permitted_fid = env->GetFieldID(data_class.get(), "permitted", "I");
        if (permitted_fid == nullptr) {
            return false;
        }
        c_data->permitted = env->GetIntField(java_data, permitted_fid);
    }


    {
        static jfieldID inheritable_fid = env->GetFieldID(data_class.get(), "inheritable", "I");
        if (inheritable_fid == nullptr) {
            return false;
        }
        c_data->inheritable = env->GetIntField(java_data, inheritable_fid);
    }

    return true;
}

static constexpr size_t kMaxCapUserDataLength = 2U;
#ifdef _LINUX_CAPABILITY_VERSION_1
static_assert(kMaxCapUserDataLength >= _LINUX_CAPABILITY_U32S_1, "Length too small.");
#endif
#ifdef _LINUX_CAPABILITY_VERSION_2
static_assert(kMaxCapUserDataLength >= _LINUX_CAPABILITY_U32S_2, "Length too small.");
#endif
#ifdef _LINUX_CAPABILITY_VERSION_3
static_assert(kMaxCapUserDataLength >= _LINUX_CAPABILITY_U32S_3, "Length too small.");
#endif
#ifdef _LINUX_CAPABILITY_VERSION_4
static_assert(false, "Unsupported capability version, please update.");
#endif

static size_t GetCapUserDataLength(uint32_t version) {
#ifdef _LINUX_CAPABILITY_VERSION_1
    if (version == _LINUX_CAPABILITY_VERSION_1) {
        return _LINUX_CAPABILITY_U32S_1;
    }
#endif
#ifdef _LINUX_CAPABILITY_VERSION_2
    if (version == _LINUX_CAPABILITY_VERSION_2) {
        return _LINUX_CAPABILITY_U32S_2;
    }
#endif
#ifdef _LINUX_CAPABILITY_VERSION_3
    if (version == _LINUX_CAPABILITY_VERSION_3) {
        return _LINUX_CAPABILITY_U32S_3;
    }
#endif
    return 0;
}

static jobject Linux_accept(JNIEnv* env, jobject, jobject javaFd, jobject javaSocketAddress) {
    sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    sockaddr* peer = (javaSocketAddress != NULL) ? reinterpret_cast<sockaddr*>(&ss) : NULL;
    socklen_t* peerLength = (javaSocketAddress != NULL) ? &sl : 0;
    jint clientFd = NET_FAILURE_RETRY(env, int, accept, javaFd, peer, peerLength);
    if (clientFd == -1 || !fillSocketAddress(env, javaSocketAddress, ss, *peerLength)) {
        close(clientFd);
        return NULL;
    }
    return (clientFd != -1) ? jniCreateFileDescriptor(env, clientFd) : NULL;
}

static jboolean Linux_access(JNIEnv* env, jobject, jstring javaPath, jint mode) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return JNI_FALSE;
    }
    int rc = TEMP_FAILURE_RETRY(access(path.c_str(), mode));
    if (rc == -1) {
        throwErrnoException(env, "access");
    }
    return (rc == 0);
}

static void Linux_bind(JNIEnv* env, jobject, jobject javaFd, jobject javaAddress, jint port) {
    // We don't need the return value because we'll already have thrown.
    (void) NET_IPV4_FALLBACK(env, int, bind, javaFd, javaAddress, port, NULL_ADDR_FORBIDDEN);
}

static void Linux_bindSocketAddress(
        JNIEnv* env, jobject, jobject javaFd, jobject javaSocketAddress) {
    sockaddr_storage ss;
    socklen_t sa_len;
    if (!javaSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len)) {
        return;  // Exception already thrown.
    }

    const sockaddr* sa = reinterpret_cast<const sockaddr*>(&ss);
    // We don't need the return value because we'll already have thrown.
    (void) NET_FAILURE_RETRY(env, int, bind, javaFd, sa, sa_len);
}

static jobjectArray Linux_capget(JNIEnv* env, jobject, jobject header) {
    // Convert Java header struct to kernel datastructure.
    __user_cap_header_struct cap_header;
    if (!ReadStructCapUserHeader(env, header, &cap_header)) {
        AssertException(env);
        return nullptr;
    }

    // Call capget.
    __user_cap_data_struct cap_data[kMaxCapUserDataLength];
    if (capget(&cap_header, &cap_data[0]) == -1) {
        // Check for EINVAL. In that case, mutate the header.
        if (errno == EINVAL) {
            int saved_errno = errno;
            SetStructCapUserHeaderVersion(env, header, &cap_header);
            errno = saved_errno;
        }
        throwErrnoException(env, "capget");
        return nullptr;
    }

    // Create the result array.
    ScopedLocalRef<jclass> data_class(env, env->FindClass("android/system/StructCapUserData"));
    if (data_class.get() == nullptr) {
        return nullptr;
    }
    size_t result_size = GetCapUserDataLength(cap_header.version);
    ScopedLocalRef<jobjectArray> result(
            env, env->NewObjectArray(result_size, data_class.get(), nullptr));
    if (result.get() == nullptr) {
        return nullptr;
    }
    // Translate the values we got.
    for (size_t i = 0; i < result_size; ++i) {
        ScopedLocalRef<jobject> value(
                env, CreateStructCapUserData(env, data_class.get(), &cap_data[i]));
        if (value.get() == nullptr) {
            AssertException(env);
            return nullptr;
        }
        env->SetObjectArrayElement(result.get(), i, value.get());
    }
    return result.release();
}

static void Linux_capset(
        JNIEnv* env, jobject, jobject header, jobjectArray data) {
    // Convert Java header struct to kernel datastructure.
    __user_cap_header_struct cap_header;
    if (!ReadStructCapUserHeader(env, header, &cap_header)) {
        AssertException(env);
        return;
    }
    size_t result_size = GetCapUserDataLength(cap_header.version);
    // Ensure that the array has the expected length.
    if (env->GetArrayLength(data) != static_cast<jint>(result_size)) {
        jniThrowExceptionFmt(env,
                             "java/lang/IllegalArgumentException",
                             "Unsupported input length %d (expected %zu)",
                             env->GetArrayLength(data),
                             result_size);
        return;
    }

    __user_cap_data_struct cap_data[kMaxCapUserDataLength];
    // Translate the values we got.
    for (size_t i = 0; i < result_size; ++i) {
        ScopedLocalRef<jobject> value(env, env->GetObjectArrayElement(data, i));
        if (!ReadStructCapUserData(env, value.get(), &cap_data[i])) {
            AssertException(env);
            return;
        }
    }

    throwIfMinusOne(env, "capset", capset(&cap_header, &cap_data[0]));
}

static void Linux_chmod(JNIEnv* env, jobject, jstring javaPath, jint mode) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "chmod", TEMP_FAILURE_RETRY(chmod(path.c_str(), mode)));
}

static void Linux_chown(JNIEnv* env, jobject, jstring javaPath, jint uid, jint gid) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "chown", TEMP_FAILURE_RETRY(chown(path.c_str(), uid, gid)));
}

static void Linux_close(JNIEnv* env, jobject, jobject javaFd) {
    // Get the FileDescriptor's 'fd' field and clear it.
    // We need to do this before we can throw an IOException (http://b/3222087).
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    jniSetFileDescriptorOfFD(env, javaFd, -1);

    // Even if close(2) fails with EINTR, the fd will have been closed.
    // Using TEMP_FAILURE_RETRY will either lead to EBADF or closing someone else's fd.
    // http://lkml.indiana.edu/hypermail/linux/kernel/0509.1/0877.html
    throwIfMinusOne(env, "close", close(fd));
}

static void Linux_connect(JNIEnv* env, jobject, jobject javaFd, jobject javaAddress, jint port) {
    (void) NET_IPV4_FALLBACK(env, int, connect, javaFd, javaAddress, port, NULL_ADDR_FORBIDDEN);
}

static void Linux_connectSocketAddress(
        JNIEnv* env, jobject, jobject javaFd, jobject javaSocketAddress) {
    sockaddr_storage ss;
    socklen_t sa_len;
    if (!javaSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len)) {
        return;  // Exception already thrown.
    }

    const sockaddr* sa = reinterpret_cast<const sockaddr*>(&ss);
    // We don't need the return value because we'll already have thrown.
    (void) NET_FAILURE_RETRY(env, int, connect, javaFd, sa, sa_len);
}

static jobject Linux_dup(JNIEnv* env, jobject, jobject javaOldFd) {
    int oldFd = jniGetFDFromFileDescriptor(env, javaOldFd);
    int newFd = throwIfMinusOne(env, "dup", TEMP_FAILURE_RETRY(dup(oldFd)));
    return (newFd != -1) ? jniCreateFileDescriptor(env, newFd) : NULL;
}

static jobject Linux_dup2(JNIEnv* env, jobject, jobject javaOldFd, jint newFd) {
    int oldFd = jniGetFDFromFileDescriptor(env, javaOldFd);
    int fd = throwIfMinusOne(env, "dup2", TEMP_FAILURE_RETRY(dup2(oldFd, newFd)));
    return (fd != -1) ? jniCreateFileDescriptor(env, fd) : NULL;
}

static jobjectArray Linux_environ(JNIEnv* env, jobject) {
    extern char** environ; // Standard, but not in any header file.
    return toStringArray(env, environ);
}

static void Linux_execve(JNIEnv* env, jobject, jstring javaFilename, jobjectArray javaArgv, jobjectArray javaEnvp) {
    ScopedUtfChars path(env, javaFilename);
    if (path.c_str() == NULL) {
        return;
    }

    ExecStrings argv(env, javaArgv);
    ExecStrings envp(env, javaEnvp);
    TEMP_FAILURE_RETRY(execve(path.c_str(), argv.get(), envp.get()));

    throwErrnoException(env, "execve");
}

static void Linux_execv(JNIEnv* env, jobject, jstring javaFilename, jobjectArray javaArgv) {
    ScopedUtfChars path(env, javaFilename);
    if (path.c_str() == NULL) {
        return;
    }

    ExecStrings argv(env, javaArgv);
    TEMP_FAILURE_RETRY(execv(path.c_str(), argv.get()));

    throwErrnoException(env, "execv");
}

static void Linux_fchmod(JNIEnv* env, jobject, jobject javaFd, jint mode) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "fchmod", TEMP_FAILURE_RETRY(fchmod(fd, mode)));
}

static void Linux_fchown(JNIEnv* env, jobject, jobject javaFd, jint uid, jint gid) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "fchown", TEMP_FAILURE_RETRY(fchown(fd, uid, gid)));
}

static jint Linux_fcntlFlock(JNIEnv* env, jobject, jobject javaFd, jint cmd, jobject javaFlock) {
    static jfieldID typeFid = env->GetFieldID(JniConstants::structFlockClass, "l_type", "S");
    static jfieldID whenceFid = env->GetFieldID(JniConstants::structFlockClass, "l_whence", "S");
    static jfieldID startFid = env->GetFieldID(JniConstants::structFlockClass, "l_start", "J");
    static jfieldID lenFid = env->GetFieldID(JniConstants::structFlockClass, "l_len", "J");
    static jfieldID pidFid = env->GetFieldID(JniConstants::structFlockClass, "l_pid", "I");

    struct flock64 lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = env->GetShortField(javaFlock, typeFid);
    lock.l_whence = env->GetShortField(javaFlock, whenceFid);
    lock.l_start = env->GetLongField(javaFlock, startFid);
    lock.l_len = env->GetLongField(javaFlock, lenFid);
    lock.l_pid = env->GetIntField(javaFlock, pidFid);

    int rc = IO_FAILURE_RETRY(env, int, fcntl, javaFd, cmd, &lock);
    if (rc != -1) {
        env->SetShortField(javaFlock, typeFid, lock.l_type);
        env->SetShortField(javaFlock, whenceFid, lock.l_whence);
        env->SetLongField(javaFlock, startFid, lock.l_start);
        env->SetLongField(javaFlock, lenFid, lock.l_len);
        env->SetIntField(javaFlock, pidFid, lock.l_pid);
    }
    return rc;
}

static jint Linux_fcntlInt(JNIEnv* env, jobject, jobject javaFd, jint cmd, jint arg) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    return throwIfMinusOne(env, "fcntl", TEMP_FAILURE_RETRY(fcntl(fd, cmd, arg)));
}

static jint Linux_fcntlVoid(JNIEnv* env, jobject, jobject javaFd, jint cmd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    return throwIfMinusOne(env, "fcntl", TEMP_FAILURE_RETRY(fcntl(fd, cmd)));
}

static void Linux_fdatasync(JNIEnv* env, jobject, jobject javaFd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "fdatasync", TEMP_FAILURE_RETRY(fdatasync(fd)));
}

static jobject Linux_fstat(JNIEnv* env, jobject, jobject javaFd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    struct stat64 sb;
    int rc = TEMP_FAILURE_RETRY(fstat64(fd, &sb));
    if (rc == -1) {
        throwErrnoException(env, "fstat");
        return NULL;
    }
    return makeStructStat(env, sb);
}

static jobject Linux_fstatvfs(JNIEnv* env, jobject, jobject javaFd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    struct statvfs sb;
    int rc = TEMP_FAILURE_RETRY(fstatvfs(fd, &sb));
    if (rc == -1) {
        throwErrnoException(env, "fstatvfs");
        return NULL;
    }
    return makeStructStatVfs(env, sb);
}

static void Linux_fsync(JNIEnv* env, jobject, jobject javaFd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "fsync", TEMP_FAILURE_RETRY(fsync(fd)));
}

static void Linux_ftruncate(JNIEnv* env, jobject, jobject javaFd, jlong length) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "ftruncate", TEMP_FAILURE_RETRY(ftruncate64(fd, length)));
}

static jstring Linux_gai_strerror(JNIEnv* env, jobject, jint error) {
    return env->NewStringUTF(gai_strerror(error));
}

static jobjectArray Linux_android_getaddrinfo(JNIEnv* env, jobject, jstring javaNode,
        jobject javaHints, jint netId) {
    ScopedUtfChars node(env, javaNode);
    if (node.c_str() == NULL) {
        return NULL;
    }

    static jfieldID flagsFid = env->GetFieldID(JniConstants::structAddrinfoClass, "ai_flags", "I");
    static jfieldID familyFid = env->GetFieldID(JniConstants::structAddrinfoClass, "ai_family", "I");
    static jfieldID socktypeFid = env->GetFieldID(JniConstants::structAddrinfoClass, "ai_socktype", "I");
    static jfieldID protocolFid = env->GetFieldID(JniConstants::structAddrinfoClass, "ai_protocol", "I");

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = env->GetIntField(javaHints, flagsFid);
    hints.ai_family = env->GetIntField(javaHints, familyFid);
    hints.ai_socktype = env->GetIntField(javaHints, socktypeFid);
    hints.ai_protocol = env->GetIntField(javaHints, protocolFid);

    addrinfo* addressList = NULL;
    errno = 0;
    int rc = android_getaddrinfofornet(node.c_str(), NULL, &hints, netId, 0, &addressList);
    std::unique_ptr<addrinfo, addrinfo_deleter> addressListDeleter(addressList);
    if (rc != 0) {
        throwGaiException(env, "android_getaddrinfo", rc);
        return NULL;
    }

    // Count results so we know how to size the output array.
    int addressCount = 0;
    for (addrinfo* ai = addressList; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
            ++addressCount;
        } else {
            ALOGE("android_getaddrinfo unexpected ai_family %i", ai->ai_family);
        }
    }
    if (addressCount == 0) {
        return NULL;
    }

    // Prepare output array.
    jobjectArray result = env->NewObjectArray(addressCount, JniConstants::inetAddressClass, NULL);
    if (result == NULL) {
        return NULL;
    }

    // Examine returned addresses one by one, save them in the output array.
    int index = 0;
    for (addrinfo* ai = addressList; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            // Unknown address family. Skip this address.
            ALOGE("android_getaddrinfo unexpected ai_family %i", ai->ai_family);
            continue;
        }

        // Convert each IP address into a Java byte array.
        sockaddr_storage& address = *reinterpret_cast<sockaddr_storage*>(ai->ai_addr);
        ScopedLocalRef<jobject> inetAddress(env, sockaddrToInetAddress(env, address, NULL));
        if (inetAddress.get() == NULL) {
            return NULL;
        }
        env->SetObjectArrayElement(result, index, inetAddress.get());
        ++index;
    }
    return result;
}

static jint Linux_getegid(JNIEnv*, jobject) {
    return getegid();
}

static jint Linux_geteuid(JNIEnv*, jobject) {
    return geteuid();
}

static jint Linux_getgid(JNIEnv*, jobject) {
    return getgid();
}

static jstring Linux_getenv(JNIEnv* env, jobject, jstring javaName) {
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return NULL;
    }
    return env->NewStringUTF(getenv(name.c_str()));
}

static jstring Linux_getnameinfo(JNIEnv* env, jobject, jobject javaAddress, jint flags) {
    sockaddr_storage ss;
    socklen_t sa_len;
    if (!inetAddressToSockaddrVerbatim(env, javaAddress, 0, ss, sa_len)) {
        return NULL;
    }
    char buf[NI_MAXHOST]; // NI_MAXHOST is longer than INET6_ADDRSTRLEN.
    errno = 0;
    int rc = getnameinfo(reinterpret_cast<sockaddr*>(&ss), sa_len, buf, sizeof(buf), NULL, 0, flags);
    if (rc != 0) {
        throwGaiException(env, "getnameinfo", rc);
        return NULL;
    }
    return env->NewStringUTF(buf);
}

static jobject Linux_getpeername(JNIEnv* env, jobject, jobject javaFd) {
  return doGetSockName(env, javaFd, false);
}

static jint Linux_getpgid(JNIEnv* env, jobject, jint pid) {
    return throwIfMinusOne(env, "getpgid", TEMP_FAILURE_RETRY(getpgid(pid)));
}

static jint Linux_getpid(JNIEnv*, jobject) {
    return TEMP_FAILURE_RETRY(getpid());
}

static jint Linux_getppid(JNIEnv*, jobject) {
    return TEMP_FAILURE_RETRY(getppid());
}

static jobject Linux_getpwnam(JNIEnv* env, jobject, jstring javaName) {
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return NULL;
    }
    return Passwd(env).getpwnam(name.c_str());
}

static jobject Linux_getpwuid(JNIEnv* env, jobject, jint uid) {
    return Passwd(env).getpwuid(uid);
}

static jobject Linux_getsockname(JNIEnv* env, jobject, jobject javaFd) {
  return doGetSockName(env, javaFd, true);
}

static jint Linux_getsockoptByte(JNIEnv* env, jobject, jobject javaFd, jint level, jint option) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    u_char result = 0;
    socklen_t size = sizeof(result);
    throwIfMinusOne(env, "getsockopt", TEMP_FAILURE_RETRY(getsockopt(fd, level, option, &result, &size)));
    return result;
}

static jobject Linux_getsockoptInAddr(JNIEnv* env, jobject, jobject javaFd, jint level, jint option) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_family = AF_INET; // This is only for the IPv4-only IP_MULTICAST_IF.
    sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(&ss);
    socklen_t size = sizeof(sa->sin_addr);
    int rc = TEMP_FAILURE_RETRY(getsockopt(fd, level, option, &sa->sin_addr, &size));
    if (rc == -1) {
        throwErrnoException(env, "getsockopt");
        return NULL;
    }
    return sockaddrToInetAddress(env, ss, NULL);
}

static jint Linux_getsockoptInt(JNIEnv* env, jobject, jobject javaFd, jint level, jint option) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    jint result = 0;
    socklen_t size = sizeof(result);
    throwIfMinusOne(env, "getsockopt", TEMP_FAILURE_RETRY(getsockopt(fd, level, option, &result, &size)));
    return result;
}

static jobject Linux_getsockoptLinger(JNIEnv* env, jobject, jobject javaFd, jint level, jint option) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    struct linger l;
    socklen_t size = sizeof(l);
    memset(&l, 0, size);
    int rc = TEMP_FAILURE_RETRY(getsockopt(fd, level, option, &l, &size));
    if (rc == -1) {
        throwErrnoException(env, "getsockopt");
        return NULL;
    }
    return makeStructLinger(env, l);
}

static jobject Linux_getsockoptTimeval(JNIEnv* env, jobject, jobject javaFd, jint level, jint option) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    struct timeval tv;
    socklen_t size = sizeof(tv);
    memset(&tv, 0, size);
    int rc = TEMP_FAILURE_RETRY(getsockopt(fd, level, option, &tv, &size));
    if (rc == -1) {
        throwErrnoException(env, "getsockopt");
        return NULL;
    }
    return makeStructTimeval(env, tv);
}

static jobject Linux_getsockoptUcred(JNIEnv* env, jobject, jobject javaFd, jint level, jint option) {
  int fd = jniGetFDFromFileDescriptor(env, javaFd);
  struct ucred u;
  socklen_t size = sizeof(u);
  memset(&u, 0, size);
  int rc = TEMP_FAILURE_RETRY(getsockopt(fd, level, option, &u, &size));
  if (rc == -1) {
    throwErrnoException(env, "getsockopt");
    return NULL;
  }
  return makeStructUcred(env, u);
}

static jint Linux_gettid(JNIEnv* env __unused, jobject) {
#if defined(__BIONIC__)
  return TEMP_FAILURE_RETRY(gettid());
#else
  return syscall(__NR_gettid);
#endif
}

static jint Linux_getuid(JNIEnv*, jobject) {
    return getuid();
}

static jbyteArray Linux_getxattr(JNIEnv* env, jobject, jstring javaPath,
        jstring javaName) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return NULL;
    }

    while (true) {
        // Get the current size of the named extended attribute.
        ssize_t valueLength;
        if ((valueLength = getxattr(path.c_str(), name.c_str(), NULL, 0)) < 0) {
            throwErrnoException(env, "getxattr");
            return NULL;
        }

        // Create the actual byte array.
        std::vector<char> buf(valueLength);
        if ((valueLength = getxattr(path.c_str(), name.c_str(), buf.data(), valueLength)) < 0) {
            if (errno == ERANGE) {
                // The attribute value has changed since last getxattr call and buf no longer fits,
                // try again.
                continue;
            }
            throwErrnoException(env, "getxattr");
            return NULL;
        }
        jbyteArray array = env->NewByteArray(valueLength);
        if (array == NULL) {
            return NULL;
        }
        env->SetByteArrayRegion(array, 0, valueLength, reinterpret_cast<const jbyte*>(buf.data()));
        return array;
    }
}

static jobjectArray Linux_getifaddrs(JNIEnv* env, jobject) {
    static jmethodID ctor = env->GetMethodID(JniConstants::structIfaddrs, "<init>",
            "(Ljava/lang/String;ILjava/net/InetAddress;Ljava/net/InetAddress;Ljava/net/InetAddress;[B)V");

    ifaddrs* ifaddr;
    int rc = TEMP_FAILURE_RETRY(getifaddrs(&ifaddr));
    if (rc == -1) {
        throwErrnoException(env, "getifaddrs");
        return NULL;
    }
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> ifaddrPtr(ifaddr, freeifaddrs);

    // Count results so we know how to size the output array.
    jint ifCount = 0;
    for (ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        ++ifCount;
    }

    // Prepare output array.
    jobjectArray result = env->NewObjectArray(ifCount, JniConstants::structIfaddrs, NULL);
    if (result == NULL) {
        return NULL;
    }

    // Traverse the list and populate the output array.
    int index = 0;
    for (ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next, ++index) {
        TO_JAVA_STRING(name, ifa->ifa_name);
        jint flags = ifa->ifa_flags;
        sockaddr_storage* interfaceAddr =
            reinterpret_cast<sockaddr_storage*>(ifa->ifa_addr);
        sockaddr_storage* netmaskAddr =
            reinterpret_cast<sockaddr_storage*>(ifa->ifa_netmask);
        sockaddr_storage* broadAddr =
            reinterpret_cast<sockaddr_storage*>(ifa->ifa_broadaddr);

        jobject addr, netmask, broad;
        jbyteArray hwaddr = NULL;
        if (interfaceAddr != NULL) {
            switch (interfaceAddr->ss_family) {
            case AF_INET:
            case AF_INET6:
                // IPv4 / IPv6.
                // interfaceAddr and netmaskAddr are never null.
                if ((addr = sockaddrToInetAddress(env, *interfaceAddr, NULL)) == NULL) {
                    return NULL;
                }
                if ((netmask = sockaddrToInetAddress(env, *netmaskAddr, NULL)) == NULL) {
                    return NULL;
                }
                if (broadAddr != NULL && (ifa->ifa_flags & IFF_BROADCAST)) {
                    if ((broad = sockaddrToInetAddress(env, *broadAddr, NULL)) == NULL) {
                        return NULL;
                    }
                } else {
                    broad = NULL;
                }
                break;
            case AF_PACKET:
                // Raw Interface.
                sockaddr_ll* sll = reinterpret_cast<sockaddr_ll*>(ifa->ifa_addr);

                bool allZero = true;
                for (int i = 0; i < sll->sll_halen; ++i) {
                    if (sll->sll_addr[i] != 0) {
                        allZero = false;
                        break;
                    }
                }

                if (!allZero) {
                    hwaddr = env->NewByteArray(sll->sll_halen);
                    if (hwaddr == NULL) {
                        return NULL;
                    }
                    env->SetByteArrayRegion(hwaddr, 0, sll->sll_halen,
                                            reinterpret_cast<const jbyte*>(sll->sll_addr));
                }
                addr = netmask = broad = NULL;
                break;
            }
        } else {
            // Preserve the entry even if the interface has no interface address.
            // http://b/29243557/
            addr = netmask = broad = NULL;
        }

        jobject o = env->NewObject(JniConstants::structIfaddrs, ctor, name, flags, addr, netmask,
                                   broad, hwaddr);
        env->SetObjectArrayElement(result, index, o);
    }

    return result;
}

static jstring Linux_if_indextoname(JNIEnv* env, jobject, jint index) {
    char buf[IF_NAMESIZE];
    char* name = if_indextoname(index, buf);
    // if_indextoname(3) returns NULL on failure, which will come out of NewStringUTF unscathed.
    // There's no useful information in errno, so we don't bother throwing. Callers can null-check.
    return env->NewStringUTF(name);
}

static jint Linux_if_nametoindex(JNIEnv* env, jobject, jstring name) {
    ScopedUtfChars cname(env, name);
    if (cname.c_str() == NULL) {
        return 0;
    }

    // There's no useful information in errno, so we don't bother throwing. Callers can zero-check.
    return if_nametoindex(cname.c_str());
}

static jobject Linux_inet_pton(JNIEnv* env, jobject, jint family, jstring javaName) {
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return NULL;
    }
    sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    // sockaddr_in and sockaddr_in6 are at the same address, so we can use either here.
    void* dst = &reinterpret_cast<sockaddr_in*>(&ss)->sin_addr;
    if (inet_pton(family, name.c_str(), dst) != 1) {
        return NULL;
    }
    ss.ss_family = family;
    return sockaddrToInetAddress(env, ss, NULL);
}

static jint Linux_ioctlFlags(JNIEnv* env, jobject, jobject javaFd, jstring javaInterfaceName) {
     struct ifreq req;
     if (!fillIfreq(env, javaInterfaceName, req)) {
        return 0;
     }
     int fd = jniGetFDFromFileDescriptor(env, javaFd);
     throwIfMinusOne(env, "ioctl", TEMP_FAILURE_RETRY(ioctl(fd, SIOCGIFFLAGS, &req)));
     return req.ifr_flags;
}

static jobject Linux_ioctlInetAddress(JNIEnv* env, jobject, jobject javaFd, jint cmd, jstring javaInterfaceName) {
    struct ifreq req;
    if (!fillIfreq(env, javaInterfaceName, req)) {
        return NULL;
    }
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    int rc = throwIfMinusOne(env, "ioctl", TEMP_FAILURE_RETRY(ioctl(fd, cmd, &req)));
    if (rc == -1) {
        return NULL;
    }
    return sockaddrToInetAddress(env, reinterpret_cast<sockaddr_storage&>(req.ifr_addr), NULL);
}

static jint Linux_ioctlInt(JNIEnv* env, jobject, jobject javaFd, jint cmd, jobject javaArg) {
    // This is complicated because ioctls may return their result by updating their argument
    // or via their return value, so we need to support both.
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    static jfieldID valueFid = env->GetFieldID(JniConstants::mutableIntClass, "value", "I");
    jint arg = env->GetIntField(javaArg, valueFid);
    int rc = throwIfMinusOne(env, "ioctl", TEMP_FAILURE_RETRY(ioctl(fd, cmd, &arg)));
    if (!env->ExceptionCheck()) {
        env->SetIntField(javaArg, valueFid, arg);
    }
    return rc;
}

static jint Linux_ioctlMTU(JNIEnv* env, jobject, jobject javaFd, jstring javaInterfaceName) {
     struct ifreq req;
     if (!fillIfreq(env, javaInterfaceName, req)) {
        return 0;
     }
     int fd = jniGetFDFromFileDescriptor(env, javaFd);
     throwIfMinusOne(env, "ioctl", TEMP_FAILURE_RETRY(ioctl(fd, SIOCGIFMTU, &req)));
     return req.ifr_mtu;
}

static jboolean Linux_isatty(JNIEnv* env, jobject, jobject javaFd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    return TEMP_FAILURE_RETRY(isatty(fd)) == 1;
}

static void Linux_kill(JNIEnv* env, jobject, jint pid, jint sig) {
    throwIfMinusOne(env, "kill", TEMP_FAILURE_RETRY(kill(pid, sig)));
}

static void Linux_lchown(JNIEnv* env, jobject, jstring javaPath, jint uid, jint gid) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "lchown", TEMP_FAILURE_RETRY(lchown(path.c_str(), uid, gid)));
}

static void Linux_link(JNIEnv* env, jobject, jstring javaOldPath, jstring javaNewPath) {
    ScopedUtfChars oldPath(env, javaOldPath);
    if (oldPath.c_str() == NULL) {
        return;
    }
    ScopedUtfChars newPath(env, javaNewPath);
    if (newPath.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "link", TEMP_FAILURE_RETRY(link(oldPath.c_str(), newPath.c_str())));
}

static void Linux_listen(JNIEnv* env, jobject, jobject javaFd, jint backlog) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "listen", TEMP_FAILURE_RETRY(listen(fd, backlog)));
}

static jobjectArray Linux_listxattr(JNIEnv* env, jobject, jstring javaPath) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }

    while (true) {
        // Get the current size of the named extended attribute.
        ssize_t valueLength;
        if ((valueLength = listxattr(path.c_str(), NULL, 0)) < 0) {
            throwErrnoException(env, "listxattr");
            return NULL;
        }

        // Create the actual byte array.
        std::string buf(valueLength, '\0');
        if ((valueLength = listxattr(path.c_str(), &buf[0], valueLength)) < 0) {
            if (errno == ERANGE) {
                // The attribute value has changed since last listxattr call and buf no longer fits,
                // try again.
                continue;
            }
            throwErrnoException(env, "listxattr");
            return NULL;
        }

        // Split the output by '\0'.
        buf.resize(valueLength > 0 ? valueLength - 1 : 0); // Remove the trailing NULL character.
        std::string delim("\0", 1);
        auto xattrs = android::base::Split(buf, delim);

        return toStringArray(env, xattrs);
    }
}

static jlong Linux_lseek(JNIEnv* env, jobject, jobject javaFd, jlong offset, jint whence) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    return throwIfMinusOne(env, "lseek", TEMP_FAILURE_RETRY(lseek64(fd, offset, whence)));
}

static jobject Linux_lstat(JNIEnv* env, jobject, jstring javaPath) {
    return doStat(env, javaPath, true);
}

static void Linux_mincore(JNIEnv* env, jobject, jlong address, jlong byteCount, jbyteArray javaVector) {
    ScopedByteArrayRW vector(env, javaVector);
    if (vector.get() == NULL) {
        return;
    }
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    unsigned char* vec = reinterpret_cast<unsigned char*>(vector.get());
    throwIfMinusOne(env, "mincore", TEMP_FAILURE_RETRY(mincore(ptr, byteCount, vec)));
}

static void Linux_mkdir(JNIEnv* env, jobject, jstring javaPath, jint mode) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "mkdir", TEMP_FAILURE_RETRY(mkdir(path.c_str(), mode)));
}

static void Linux_mkfifo(JNIEnv* env, jobject, jstring javaPath, jint mode) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "mkfifo", TEMP_FAILURE_RETRY(mkfifo(path.c_str(), mode)));
}

static void Linux_mlock(JNIEnv* env, jobject, jlong address, jlong byteCount) {
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    throwIfMinusOne(env, "mlock", TEMP_FAILURE_RETRY(mlock(ptr, byteCount)));
}

static jlong Linux_mmap(JNIEnv* env, jobject, jlong address, jlong byteCount, jint prot, jint flags, jobject javaFd, jlong offset) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    void* suggestedPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    void* ptr = mmap64(suggestedPtr, byteCount, prot, flags, fd, offset);
    if (ptr == MAP_FAILED) {
        throwErrnoException(env, "mmap");
    }
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(ptr));
}

static void Linux_msync(JNIEnv* env, jobject, jlong address, jlong byteCount, jint flags) {
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    throwIfMinusOne(env, "msync", TEMP_FAILURE_RETRY(msync(ptr, byteCount, flags)));
}

static void Linux_munlock(JNIEnv* env, jobject, jlong address, jlong byteCount) {
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    throwIfMinusOne(env, "munlock", TEMP_FAILURE_RETRY(munlock(ptr, byteCount)));
}

static void Linux_munmap(JNIEnv* env, jobject, jlong address, jlong byteCount) {
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    throwIfMinusOne(env, "munmap", TEMP_FAILURE_RETRY(munmap(ptr, byteCount)));
}

static jobject Linux_open(JNIEnv* env, jobject, jstring javaPath, jint flags, jint mode) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }
    int fd = throwIfMinusOne(env, "open", TEMP_FAILURE_RETRY(open(path.c_str(), flags, mode)));
    return fd != -1 ? jniCreateFileDescriptor(env, fd) : NULL;
}

static jobjectArray Linux_pipe2(JNIEnv* env, jobject, jint flags __unused) {
    int fds[2];
    throwIfMinusOne(env, "pipe2", TEMP_FAILURE_RETRY(pipe2(&fds[0], flags)));
    jobjectArray result = env->NewObjectArray(2, JniConstants::fileDescriptorClass, NULL);
    if (result == NULL) {
        return NULL;
    }
    for (int i = 0; i < 2; ++i) {
        ScopedLocalRef<jobject> fd(env, jniCreateFileDescriptor(env, fds[i]));
        if (fd.get() == NULL) {
            return NULL;
        }
        env->SetObjectArrayElement(result, i, fd.get());
        if (env->ExceptionCheck()) {
            return NULL;
        }
    }
    return result;
}

static jint Linux_poll(JNIEnv* env, jobject, jobjectArray javaStructs, jint timeoutMs) {
    static jfieldID fdFid = env->GetFieldID(JniConstants::structPollfdClass, "fd", "Ljava/io/FileDescriptor;");
    static jfieldID eventsFid = env->GetFieldID(JniConstants::structPollfdClass, "events", "S");
    static jfieldID reventsFid = env->GetFieldID(JniConstants::structPollfdClass, "revents", "S");

    // Turn the Java android.system.StructPollfd[] into a C++ struct pollfd[].
    size_t arrayLength = env->GetArrayLength(javaStructs);
    std::unique_ptr<struct pollfd[]> fds(new struct pollfd[arrayLength]);
    memset(fds.get(), 0, sizeof(struct pollfd) * arrayLength);
    size_t count = 0; // Some trailing array elements may be irrelevant. (See below.)
    for (size_t i = 0; i < arrayLength; ++i) {
        ScopedLocalRef<jobject> javaStruct(env, env->GetObjectArrayElement(javaStructs, i));
        if (javaStruct.get() == NULL) {
            break; // We allow trailing nulls in the array for caller convenience.
        }
        ScopedLocalRef<jobject> javaFd(env, env->GetObjectField(javaStruct.get(), fdFid));
        if (javaFd.get() == NULL) {
            break; // We also allow callers to just clear the fd field (this is what Selector does).
        }
        fds[count].fd = jniGetFDFromFileDescriptor(env, javaFd.get());
        fds[count].events = env->GetShortField(javaStruct.get(), eventsFid);
        ++count;
    }

    std::vector<AsynchronousCloseMonitor*> monitors;
    for (size_t i = 0; i < count; ++i) {
        monitors.push_back(new AsynchronousCloseMonitor(fds[i].fd));
    }

    int rc;
    while (true) {
        timespec before;
        clock_gettime(CLOCK_MONOTONIC, &before);

        rc = poll(fds.get(), count, timeoutMs);
        if (rc >= 0 || errno != EINTR) {
            break;
        }

        // We got EINTR. Work out how much of the original timeout is still left.
        if (timeoutMs > 0) {
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            timespec diff;
            diff.tv_sec = now.tv_sec - before.tv_sec;
            diff.tv_nsec = now.tv_nsec - before.tv_nsec;
            if (diff.tv_nsec < 0) {
                --diff.tv_sec;
                diff.tv_nsec += 1000000000;
            }

            jint diffMs = diff.tv_sec * 1000 + diff.tv_nsec / 1000000;
            if (diffMs >= timeoutMs) {
                rc = 0; // We have less than 1ms left anyway, so just time out.
                break;
            }

            timeoutMs -= diffMs;
        }
    }

    for (size_t i = 0; i < monitors.size(); ++i) {
        delete monitors[i];
    }
    if (rc == -1) {
        throwErrnoException(env, "poll");
        return -1;
    }

    // Update the revents fields in the Java android.system.StructPollfd[].
    for (size_t i = 0; i < count; ++i) {
        ScopedLocalRef<jobject> javaStruct(env, env->GetObjectArrayElement(javaStructs, i));
        if (javaStruct.get() == NULL) {
            return -1;
        }
        env->SetShortField(javaStruct.get(), reventsFid, fds[i].revents);
    }
    return rc;
}

static void Linux_posix_fallocate(JNIEnv* env, jobject, jobject javaFd __unused,
                                  jlong offset __unused, jlong length __unused) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    while ((errno = posix_fallocate64(fd, offset, length)) == EINTR) {
    }
    if (errno != 0) {
        throwErrnoException(env, "posix_fallocate");
    }
}

static jint Linux_prctl(JNIEnv* env, jobject, jint option __unused, jlong arg2 __unused,
                        jlong arg3 __unused, jlong arg4 __unused, jlong arg5 __unused) {
    int result = TEMP_FAILURE_RETRY(prctl(static_cast<int>(option),
                                          static_cast<unsigned long>(arg2),
                                          static_cast<unsigned long>(arg3),
                                          static_cast<unsigned long>(arg4),
                                          static_cast<unsigned long>(arg5)));
    return throwIfMinusOne(env, "prctl", result);
}

static jint Linux_preadBytes(JNIEnv* env, jobject, jobject javaFd, jobject javaBytes, jint byteOffset, jint byteCount, jlong offset) {
    ScopedBytesRW bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }
    return IO_FAILURE_RETRY(env, ssize_t, pread64, javaFd, bytes.get() + byteOffset, byteCount, offset);
}

static jint Linux_pwriteBytes(JNIEnv* env, jobject, jobject javaFd, jbyteArray javaBytes, jint byteOffset, jint byteCount, jlong offset) {
    ScopedBytesRO bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }
    return IO_FAILURE_RETRY(env, ssize_t, pwrite64, javaFd, bytes.get() + byteOffset, byteCount, offset);
}

static jint Linux_readBytes(JNIEnv* env, jobject, jobject javaFd, jobject javaBytes, jint byteOffset, jint byteCount) {
    ScopedBytesRW bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }
    return IO_FAILURE_RETRY(env, ssize_t, read, javaFd, bytes.get() + byteOffset, byteCount);
}

static jstring Linux_readlink(JNIEnv* env, jobject, jstring javaPath) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }

    std::string result;
    if (!android::base::Readlink(path.c_str(), &result)) {
        throwErrnoException(env, "readlink");
        return NULL;
    }
    return env->NewStringUTF(result.c_str());
}

static jstring Linux_realpath(JNIEnv* env, jobject, jstring javaPath) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }

    std::unique_ptr<char, c_deleter> real_path(realpath(path.c_str(), nullptr));
    if (real_path.get() == nullptr) {
        throwErrnoException(env, "realpath");
        return NULL;
    }

    return env->NewStringUTF(real_path.get());
}

static jint Linux_readv(JNIEnv* env, jobject, jobject javaFd, jobjectArray buffers, jintArray offsets, jintArray byteCounts) {
    IoVec<ScopedBytesRW> ioVec(env, env->GetArrayLength(buffers));
    if (!ioVec.init(buffers, offsets, byteCounts)) {
        return -1;
    }
    return IO_FAILURE_RETRY(env, ssize_t, readv, javaFd, ioVec.get(), ioVec.size());
}

static jint Linux_recvfromBytes(JNIEnv* env, jobject, jobject javaFd, jobject javaBytes, jint byteOffset, jint byteCount, jint flags, jobject javaInetSocketAddress) {
    ScopedBytesRW bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }
    sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    sockaddr* from = (javaInetSocketAddress != NULL) ? reinterpret_cast<sockaddr*>(&ss) : NULL;
    socklen_t* fromLength = (javaInetSocketAddress != NULL) ? &sl : 0;
    jint recvCount = NET_FAILURE_RETRY(env, ssize_t, recvfrom, javaFd, bytes.get() + byteOffset, byteCount, flags, from, fromLength);
    if (recvCount >= 0) {
        // The socket may have performed orderly shutdown and recvCount would return 0 (see man 2
        // recvfrom), in which case ss.ss_family == AF_UNIX and fillInetSocketAddress would fail.
        // Don't fill in the address if recvfrom didn't succeed. http://b/33483694
        if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6) {
            fillInetSocketAddress(env, javaInetSocketAddress, ss);
        }
    }
    return recvCount;
}

static void Linux_remove(JNIEnv* env, jobject, jstring javaPath) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "remove", TEMP_FAILURE_RETRY(remove(path.c_str())));
}

static void Linux_removexattr(JNIEnv* env, jobject, jstring javaPath, jstring javaName) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return;
    }

    int res = removexattr(path.c_str(), name.c_str());
    if (res < 0) {
        throwErrnoException(env, "removexattr");
    }
}

static void Linux_rename(JNIEnv* env, jobject, jstring javaOldPath, jstring javaNewPath) {
    ScopedUtfChars oldPath(env, javaOldPath);
    if (oldPath.c_str() == NULL) {
        return;
    }
    ScopedUtfChars newPath(env, javaNewPath);
    if (newPath.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "rename", TEMP_FAILURE_RETRY(rename(oldPath.c_str(), newPath.c_str())));
}

static jlong Linux_sendfile(JNIEnv* env, jobject, jobject javaOutFd, jobject javaInFd, jobject javaOffset, jlong byteCount) {
    int outFd = jniGetFDFromFileDescriptor(env, javaOutFd);
    int inFd = jniGetFDFromFileDescriptor(env, javaInFd);
    static jfieldID valueFid = env->GetFieldID(JniConstants::mutableLongClass, "value", "J");
    off_t offset = 0;
    off_t* offsetPtr = NULL;
    if (javaOffset != NULL) {
        // TODO: fix bionic so we can have a 64-bit off_t!
        offset = env->GetLongField(javaOffset, valueFid);
        offsetPtr = &offset;
    }
    jlong result = throwIfMinusOne(env, "sendfile", TEMP_FAILURE_RETRY(sendfile(outFd, inFd, offsetPtr, byteCount)));
    if (javaOffset != NULL) {
        env->SetLongField(javaOffset, valueFid, offset);
    }
    return result;
}

static jint Linux_sendtoBytes(JNIEnv* env, jobject, jobject javaFd, jobject javaBytes, jint byteOffset, jint byteCount, jint flags, jobject javaInetAddress, jint port) {
    ScopedBytesRO bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }

    return NET_IPV4_FALLBACK(env, ssize_t, sendto, javaFd, javaInetAddress, port,
                             NULL_ADDR_OK, bytes.get() + byteOffset, byteCount, flags);
}

static jint Linux_sendtoBytesSocketAddress(JNIEnv* env, jobject, jobject javaFd, jobject javaBytes, jint byteOffset, jint byteCount, jint flags, jobject javaSocketAddress) {
    if (env->IsInstanceOf(javaSocketAddress, JniConstants::inetSocketAddressClass)) {
        // Use the InetAddress version so we get the benefit of NET_IPV4_FALLBACK.
        jobject javaInetAddress;
        jint port;
        javaInetSocketAddressToInetAddressAndPort(env, javaSocketAddress, javaInetAddress, port);
        return Linux_sendtoBytes(env, NULL, javaFd, javaBytes, byteOffset, byteCount, flags,
                                 javaInetAddress, port);
    }

    ScopedBytesRO bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }

    sockaddr_storage ss;
    socklen_t sa_len;
    if (!javaSocketAddressToSockaddr(env, javaSocketAddress, ss, sa_len)) {
        return -1;
    }

    const sockaddr* sa = reinterpret_cast<const sockaddr*>(&ss);
    // We don't need the return value because we'll already have thrown.
    return NET_FAILURE_RETRY(env, ssize_t, sendto, javaFd, bytes.get() + byteOffset, byteCount, flags, sa, sa_len);
}

static void Linux_setegid(JNIEnv* env, jobject, jint egid) {
    throwIfMinusOne(env, "setegid", TEMP_FAILURE_RETRY(setegid(egid)));
}

static void Linux_setenv(JNIEnv* env, jobject, jstring javaName, jstring javaValue, jboolean overwrite) {
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return;
    }
    ScopedUtfChars value(env, javaValue);
    if (value.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "setenv", setenv(name.c_str(), value.c_str(), overwrite));
}

static void Linux_seteuid(JNIEnv* env, jobject, jint euid) {
    throwIfMinusOne(env, "seteuid", TEMP_FAILURE_RETRY(seteuid(euid)));
}

static void Linux_setgid(JNIEnv* env, jobject, jint gid) {
    throwIfMinusOne(env, "setgid", TEMP_FAILURE_RETRY(setgid(gid)));
}

static void Linux_setpgid(JNIEnv* env, jobject, jint pid, int pgid) {
    throwIfMinusOne(env, "setpgid", TEMP_FAILURE_RETRY(setpgid(pid, pgid)));
}

static void Linux_setregid(JNIEnv* env, jobject, jint rgid, int egid) {
    throwIfMinusOne(env, "setregid", TEMP_FAILURE_RETRY(setregid(rgid, egid)));
}

static void Linux_setreuid(JNIEnv* env, jobject, jint ruid, int euid) {
    throwIfMinusOne(env, "setreuid", TEMP_FAILURE_RETRY(setreuid(ruid, euid)));
}

static jint Linux_setsid(JNIEnv* env, jobject) {
    return throwIfMinusOne(env, "setsid", TEMP_FAILURE_RETRY(setsid()));
}

static void Linux_setsockoptByte(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jint value) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    u_char byte = value;
    throwIfMinusOne(env, "setsockopt", TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &byte, sizeof(byte))));
}

static void Linux_setsockoptIfreq(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jstring javaInterfaceName) {
    struct ifreq req;
    if (!fillIfreq(env, javaInterfaceName, req)) {
        return;
    }
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "setsockopt", TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &req, sizeof(req))));
}

static void Linux_setsockoptInt(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jint value) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "setsockopt", TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &value, sizeof(value))));
}

static void Linux_setsockoptIpMreqn(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jint value) {
    ip_mreqn req;
    memset(&req, 0, sizeof(req));
    req.imr_ifindex = value;
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "setsockopt", TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &req, sizeof(req))));
}

static void Linux_setsockoptGroupReq(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jobject javaGroupReq) {
    struct group_req req;
    memset(&req, 0, sizeof(req));

    static jfieldID grInterfaceFid = env->GetFieldID(JniConstants::structGroupReqClass, "gr_interface", "I");
    req.gr_interface = env->GetIntField(javaGroupReq, grInterfaceFid);
    // Get the IPv4 or IPv6 multicast address to join or leave.
    static jfieldID grGroupFid = env->GetFieldID(JniConstants::structGroupReqClass, "gr_group", "Ljava/net/InetAddress;");
    ScopedLocalRef<jobject> javaGroup(env, env->GetObjectField(javaGroupReq, grGroupFid));
    socklen_t sa_len;
    if (!inetAddressToSockaddrVerbatim(env, javaGroup.get(), 0, req.gr_group, sa_len)) {
        return;
    }

    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    int rc = TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &req, sizeof(req)));
    if (rc == -1 && errno == EINVAL) {
        // Maybe we're a 32-bit binary talking to a 64-bit kernel?
        // glibc doesn't automatically handle this.
        // http://sourceware.org/bugzilla/show_bug.cgi?id=12080
        struct group_req64 {
            uint32_t gr_interface;
            uint32_t my_padding;
            sockaddr_storage gr_group;
        };
        group_req64 req64;
        req64.gr_interface = req.gr_interface;
        memcpy(&req64.gr_group, &req.gr_group, sizeof(req.gr_group));
        rc = TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &req64, sizeof(req64)));
    }
    throwIfMinusOne(env, "setsockopt", rc);
}

static void Linux_setsockoptGroupSourceReq(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jobject javaGroupSourceReq) {
    socklen_t sa_len;
    struct group_source_req req;
    memset(&req, 0, sizeof(req));

    static jfieldID gsrInterfaceFid = env->GetFieldID(JniConstants::structGroupSourceReqClass, "gsr_interface", "I");
    req.gsr_interface = env->GetIntField(javaGroupSourceReq, gsrInterfaceFid);
    // Get the IPv4 or IPv6 multicast address to join or leave.
    static jfieldID gsrGroupFid = env->GetFieldID(JniConstants::structGroupSourceReqClass, "gsr_group", "Ljava/net/InetAddress;");
    ScopedLocalRef<jobject> javaGroup(env, env->GetObjectField(javaGroupSourceReq, gsrGroupFid));
    if (!inetAddressToSockaddrVerbatim(env, javaGroup.get(), 0, req.gsr_group, sa_len)) {
        return;
    }

    // Get the IPv4 or IPv6 multicast address to add to the filter.
    static jfieldID gsrSourceFid = env->GetFieldID(JniConstants::structGroupSourceReqClass, "gsr_source", "Ljava/net/InetAddress;");
    ScopedLocalRef<jobject> javaSource(env, env->GetObjectField(javaGroupSourceReq, gsrSourceFid));
    if (!inetAddressToSockaddrVerbatim(env, javaSource.get(), 0, req.gsr_source, sa_len)) {
        return;
    }

    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    int rc = TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &req, sizeof(req)));
    if (rc == -1 && errno == EINVAL) {
        // Maybe we're a 32-bit binary talking to a 64-bit kernel?
        // glibc doesn't automatically handle this.
        // http://sourceware.org/bugzilla/show_bug.cgi?id=12080
        struct group_source_req64 {
            uint32_t gsr_interface;
            uint32_t my_padding;
            sockaddr_storage gsr_group;
            sockaddr_storage gsr_source;
        };
        group_source_req64 req64;
        req64.gsr_interface = req.gsr_interface;
        memcpy(&req64.gsr_group, &req.gsr_group, sizeof(req.gsr_group));
        memcpy(&req64.gsr_source, &req.gsr_source, sizeof(req.gsr_source));
        rc = TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &req64, sizeof(req64)));
    }
    throwIfMinusOne(env, "setsockopt", rc);
}

static void Linux_setsockoptLinger(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jobject javaLinger) {
    static jfieldID lOnoffFid = env->GetFieldID(JniConstants::structLingerClass, "l_onoff", "I");
    static jfieldID lLingerFid = env->GetFieldID(JniConstants::structLingerClass, "l_linger", "I");
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    struct linger value;
    value.l_onoff = env->GetIntField(javaLinger, lOnoffFid);
    value.l_linger = env->GetIntField(javaLinger, lLingerFid);
    throwIfMinusOne(env, "setsockopt", TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &value, sizeof(value))));
}

static void Linux_setsockoptTimeval(JNIEnv* env, jobject, jobject javaFd, jint level, jint option, jobject javaTimeval) {
    static jfieldID tvSecFid = env->GetFieldID(JniConstants::structTimevalClass, "tv_sec", "J");
    static jfieldID tvUsecFid = env->GetFieldID(JniConstants::structTimevalClass, "tv_usec", "J");
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    struct timeval value;
    value.tv_sec = env->GetLongField(javaTimeval, tvSecFid);
    value.tv_usec = env->GetLongField(javaTimeval, tvUsecFid);
    throwIfMinusOne(env, "setsockopt", TEMP_FAILURE_RETRY(setsockopt(fd, level, option, &value, sizeof(value))));
}

static void Linux_setuid(JNIEnv* env, jobject, jint uid) {
    throwIfMinusOne(env, "setuid", TEMP_FAILURE_RETRY(setuid(uid)));
}

static void Linux_setxattr(JNIEnv* env, jobject, jstring javaPath, jstring javaName,
        jbyteArray javaValue, jint flags) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return;
    }
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return;
    }
    ScopedBytesRO value(env, javaValue);
    if (value.get() == NULL) {
        return;
    }
    size_t valueLength = env->GetArrayLength(javaValue);
    int res = setxattr(path.c_str(), name.c_str(), value.get(), valueLength, flags);
    if (res < 0) {
        throwErrnoException(env, "setxattr");
    }
}

static void Linux_shutdown(JNIEnv* env, jobject, jobject javaFd, jint how) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "shutdown", TEMP_FAILURE_RETRY(shutdown(fd, how)));
}

static jobject Linux_socket(JNIEnv* env, jobject, jint domain, jint type, jint protocol) {
    if (domain == AF_PACKET) {
        protocol = htons(protocol);  // Packet sockets specify the protocol in host byte order.
    }
    int fd = throwIfMinusOne(env, "socket", TEMP_FAILURE_RETRY(socket(domain, type, protocol)));
    return fd != -1 ? jniCreateFileDescriptor(env, fd) : NULL;
}

static void Linux_socketpair(JNIEnv* env, jobject, jint domain, jint type, jint protocol, jobject javaFd1, jobject javaFd2) {
    int fds[2];
    int rc = throwIfMinusOne(env, "socketpair", TEMP_FAILURE_RETRY(socketpair(domain, type, protocol, fds)));
    if (rc != -1) {
        jniSetFileDescriptorOfFD(env, javaFd1, fds[0]);
        jniSetFileDescriptorOfFD(env, javaFd2, fds[1]);
    }
}

static jobject Linux_stat(JNIEnv* env, jobject, jstring javaPath) {
    return doStat(env, javaPath, false);
}

static jobject Linux_statvfs(JNIEnv* env, jobject, jstring javaPath) {
    ScopedUtfChars path(env, javaPath);
    if (path.c_str() == NULL) {
        return NULL;
    }
    struct statvfs sb;
    int rc = TEMP_FAILURE_RETRY(statvfs(path.c_str(), &sb));
    if (rc == -1) {
        throwErrnoException(env, "statvfs");
        return NULL;
    }
    return makeStructStatVfs(env, sb);
}

static jstring Linux_strerror(JNIEnv* env, jobject, jint errnum) {
    char buffer[BUFSIZ];
    const char* message = jniStrError(errnum, buffer, sizeof(buffer));
    return env->NewStringUTF(message);
}

static jstring Linux_strsignal(JNIEnv* env, jobject, jint signal) {
    return env->NewStringUTF(strsignal(signal));
}

static void Linux_symlink(JNIEnv* env, jobject, jstring javaOldPath, jstring javaNewPath) {
    ScopedUtfChars oldPath(env, javaOldPath);
    if (oldPath.c_str() == NULL) {
        return;
    }
    ScopedUtfChars newPath(env, javaNewPath);
    if (newPath.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "symlink", TEMP_FAILURE_RETRY(symlink(oldPath.c_str(), newPath.c_str())));
}

static jlong Linux_sysconf(JNIEnv* env, jobject, jint name) {
    // Since -1 is a valid result from sysconf(3), detecting failure is a little more awkward.
    errno = 0;
    long result = sysconf(name);
    if (result == -1L && errno == EINVAL) {
        throwErrnoException(env, "sysconf");
    }
    return result;
}

static void Linux_tcdrain(JNIEnv* env, jobject, jobject javaFd) {
    int fd = jniGetFDFromFileDescriptor(env, javaFd);
    throwIfMinusOne(env, "tcdrain", TEMP_FAILURE_RETRY(tcdrain(fd)));
}

static void Linux_tcsendbreak(JNIEnv* env, jobject, jobject javaFd, jint duration) {
  int fd = jniGetFDFromFileDescriptor(env, javaFd);
  throwIfMinusOne(env, "tcsendbreak", TEMP_FAILURE_RETRY(tcsendbreak(fd, duration)));
}

static jint Linux_umaskImpl(JNIEnv*, jobject, jint mask) {
    return umask(mask);
}

static jobject Linux_uname(JNIEnv* env, jobject) {
    struct utsname buf;
    if (TEMP_FAILURE_RETRY(uname(&buf)) == -1) {
        return NULL; // Can't happen.
    }
    return makeStructUtsname(env, buf);
}

static void Linux_unlink(JNIEnv* env, jobject, jstring javaPathname) {
    ScopedUtfChars pathname(env, javaPathname);
    if (pathname.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "unlink", unlink(pathname.c_str()));
}

static void Linux_unsetenv(JNIEnv* env, jobject, jstring javaName) {
    ScopedUtfChars name(env, javaName);
    if (name.c_str() == NULL) {
        return;
    }
    throwIfMinusOne(env, "unsetenv", unsetenv(name.c_str()));
}

static jint Linux_waitpid(JNIEnv* env, jobject, jint pid, jobject javaStatus, jint options) {
    int status;
    int rc = throwIfMinusOne(env, "waitpid", TEMP_FAILURE_RETRY(waitpid(pid, &status, options)));
    if (rc != -1) {
        static jfieldID valueFid = env->GetFieldID(JniConstants::mutableIntClass, "value", "I");
        env->SetIntField(javaStatus, valueFid, status);
    }
    return rc;
}

static jint Linux_writeBytes(JNIEnv* env, jobject, jobject javaFd, jbyteArray javaBytes, jint byteOffset, jint byteCount) {
    ScopedBytesRO bytes(env, javaBytes);
    if (bytes.get() == NULL) {
        return -1;
    }
    return IO_FAILURE_RETRY(env, ssize_t, write, javaFd, bytes.get() + byteOffset, byteCount);
}

static jint Linux_writev(JNIEnv* env, jobject, jobject javaFd, jobjectArray buffers, jintArray offsets, jintArray byteCounts) {
    IoVec<ScopedBytesRO> ioVec(env, env->GetArrayLength(buffers));
    if (!ioVec.init(buffers, offsets, byteCounts)) {
        return -1;
    }
    return IO_FAILURE_RETRY(env, ssize_t, writev, javaFd, ioVec.get(), ioVec.size());
}

#define NATIVE_METHOD_OVERLOAD(className, functionName, signature, variant) \
    { #functionName, signature, reinterpret_cast<void*>(className ## _ ## functionName ## variant) }

static JNINativeMethod gMethods[] = {
    NATIVE_METHOD(Linux, accept, "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)Ljava/io/FileDescriptor;"),
    NATIVE_METHOD(Linux, access, "(Ljava/lang/String;I)Z"),
    NATIVE_METHOD(Linux, android_getaddrinfo, "(Ljava/lang/String;Landroid/system/StructAddrinfo;I)[Ljava/net/InetAddress;"),
    NATIVE_METHOD(Linux, bind, "(Ljava/io/FileDescriptor;Ljava/net/InetAddress;I)V"),
    NATIVE_METHOD_OVERLOAD(Linux, bind, "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)V", SocketAddress),
    NATIVE_METHOD(Linux, capget,
                  "(Landroid/system/StructCapUserHeader;)[Landroid/system/StructCapUserData;"),
    NATIVE_METHOD(Linux, capset,
                  "(Landroid/system/StructCapUserHeader;[Landroid/system/StructCapUserData;)V"),
    NATIVE_METHOD(Linux, chmod, "(Ljava/lang/String;I)V"),
    NATIVE_METHOD(Linux, chown, "(Ljava/lang/String;II)V"),
    NATIVE_METHOD(Linux, close, "(Ljava/io/FileDescriptor;)V"),
    NATIVE_METHOD(Linux, connect, "(Ljava/io/FileDescriptor;Ljava/net/InetAddress;I)V"),
    NATIVE_METHOD_OVERLOAD(Linux, connect, "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)V", SocketAddress),
    NATIVE_METHOD(Linux, dup, "(Ljava/io/FileDescriptor;)Ljava/io/FileDescriptor;"),
    NATIVE_METHOD(Linux, dup2, "(Ljava/io/FileDescriptor;I)Ljava/io/FileDescriptor;"),
    NATIVE_METHOD(Linux, environ, "()[Ljava/lang/String;"),
    NATIVE_METHOD(Linux, execv, "(Ljava/lang/String;[Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, execve, "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, fchmod, "(Ljava/io/FileDescriptor;I)V"),
    NATIVE_METHOD(Linux, fchown, "(Ljava/io/FileDescriptor;II)V"),
    NATIVE_METHOD(Linux, fcntlFlock, "(Ljava/io/FileDescriptor;ILandroid/system/StructFlock;)I"),
    NATIVE_METHOD(Linux, fcntlInt, "(Ljava/io/FileDescriptor;II)I"),
    NATIVE_METHOD(Linux, fcntlVoid, "(Ljava/io/FileDescriptor;I)I"),
    NATIVE_METHOD(Linux, fdatasync, "(Ljava/io/FileDescriptor;)V"),
    NATIVE_METHOD(Linux, fstat, "(Ljava/io/FileDescriptor;)Landroid/system/StructStat;"),
    NATIVE_METHOD(Linux, fstatvfs, "(Ljava/io/FileDescriptor;)Landroid/system/StructStatVfs;"),
    NATIVE_METHOD(Linux, fsync, "(Ljava/io/FileDescriptor;)V"),
    NATIVE_METHOD(Linux, ftruncate, "(Ljava/io/FileDescriptor;J)V"),
    NATIVE_METHOD(Linux, gai_strerror, "(I)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, getegid, "()I"),
    NATIVE_METHOD(Linux, geteuid, "()I"),
    NATIVE_METHOD(Linux, getgid, "()I"),
    NATIVE_METHOD(Linux, getenv, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, getnameinfo, "(Ljava/net/InetAddress;I)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, getpeername, "(Ljava/io/FileDescriptor;)Ljava/net/SocketAddress;"),
    NATIVE_METHOD(Linux, getpgid, "(I)I"),
    NATIVE_METHOD(Linux, getpid, "()I"),
    NATIVE_METHOD(Linux, getppid, "()I"),
    NATIVE_METHOD(Linux, getpwnam, "(Ljava/lang/String;)Landroid/system/StructPasswd;"),
    NATIVE_METHOD(Linux, getpwuid, "(I)Landroid/system/StructPasswd;"),
    NATIVE_METHOD(Linux, getsockname, "(Ljava/io/FileDescriptor;)Ljava/net/SocketAddress;"),
    NATIVE_METHOD(Linux, getsockoptByte, "(Ljava/io/FileDescriptor;II)I"),
    NATIVE_METHOD(Linux, getsockoptInAddr, "(Ljava/io/FileDescriptor;II)Ljava/net/InetAddress;"),
    NATIVE_METHOD(Linux, getsockoptInt, "(Ljava/io/FileDescriptor;II)I"),
    NATIVE_METHOD(Linux, getsockoptLinger, "(Ljava/io/FileDescriptor;II)Landroid/system/StructLinger;"),
    NATIVE_METHOD(Linux, getsockoptTimeval, "(Ljava/io/FileDescriptor;II)Landroid/system/StructTimeval;"),
    NATIVE_METHOD(Linux, getsockoptUcred, "(Ljava/io/FileDescriptor;II)Landroid/system/StructUcred;"),
    NATIVE_METHOD(Linux, gettid, "()I"),
    NATIVE_METHOD(Linux, getuid, "()I"),
    NATIVE_METHOD(Linux, getxattr, "(Ljava/lang/String;Ljava/lang/String;)[B"),
    NATIVE_METHOD(Linux, getifaddrs, "()[Landroid/system/StructIfaddrs;"),
    NATIVE_METHOD(Linux, if_indextoname, "(I)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, if_nametoindex, "(Ljava/lang/String;)I"),
    NATIVE_METHOD(Linux, inet_pton, "(ILjava/lang/String;)Ljava/net/InetAddress;"),
    NATIVE_METHOD(Linux, ioctlFlags, "(Ljava/io/FileDescriptor;Ljava/lang/String;)I"),
    NATIVE_METHOD(Linux, ioctlInetAddress, "(Ljava/io/FileDescriptor;ILjava/lang/String;)Ljava/net/InetAddress;"),
    NATIVE_METHOD(Linux, ioctlInt, "(Ljava/io/FileDescriptor;ILandroid/util/MutableInt;)I"),
    NATIVE_METHOD(Linux, ioctlMTU, "(Ljava/io/FileDescriptor;Ljava/lang/String;)I"),
    NATIVE_METHOD(Linux, isatty, "(Ljava/io/FileDescriptor;)Z"),
    NATIVE_METHOD(Linux, kill, "(II)V"),
    NATIVE_METHOD(Linux, lchown, "(Ljava/lang/String;II)V"),
    NATIVE_METHOD(Linux, link, "(Ljava/lang/String;Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, listen, "(Ljava/io/FileDescriptor;I)V"),
    NATIVE_METHOD(Linux, listxattr, "(Ljava/lang/String;)[Ljava/lang/String;"),
    NATIVE_METHOD(Linux, lseek, "(Ljava/io/FileDescriptor;JI)J"),
    NATIVE_METHOD(Linux, lstat, "(Ljava/lang/String;)Landroid/system/StructStat;"),
    NATIVE_METHOD(Linux, mincore, "(JJ[B)V"),
    NATIVE_METHOD(Linux, mkdir, "(Ljava/lang/String;I)V"),
    NATIVE_METHOD(Linux, mkfifo, "(Ljava/lang/String;I)V"),
    NATIVE_METHOD(Linux, mlock, "(JJ)V"),
    NATIVE_METHOD(Linux, mmap, "(JJIILjava/io/FileDescriptor;J)J"),
    NATIVE_METHOD(Linux, msync, "(JJI)V"),
    NATIVE_METHOD(Linux, munlock, "(JJ)V"),
    NATIVE_METHOD(Linux, munmap, "(JJ)V"),
    NATIVE_METHOD(Linux, open, "(Ljava/lang/String;II)Ljava/io/FileDescriptor;"),
    NATIVE_METHOD(Linux, pipe2, "(I)[Ljava/io/FileDescriptor;"),
    NATIVE_METHOD(Linux, poll, "([Landroid/system/StructPollfd;I)I"),
    NATIVE_METHOD(Linux, posix_fallocate, "(Ljava/io/FileDescriptor;JJ)V"),
    NATIVE_METHOD(Linux, prctl, "(IJJJJ)I"),
    NATIVE_METHOD(Linux, preadBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIJ)I"),
    NATIVE_METHOD(Linux, pwriteBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIJ)I"),
    NATIVE_METHOD(Linux, readBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I"),
    NATIVE_METHOD(Linux, readlink, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, realpath, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, readv, "(Ljava/io/FileDescriptor;[Ljava/lang/Object;[I[I)I"),
    NATIVE_METHOD(Linux, recvfromBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/InetSocketAddress;)I"),
    NATIVE_METHOD(Linux, remove, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, removexattr, "(Ljava/lang/String;Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, rename, "(Ljava/lang/String;Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, sendfile, "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;Landroid/util/MutableLong;J)J"),
    NATIVE_METHOD(Linux, sendtoBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/InetAddress;I)I"),
    NATIVE_METHOD_OVERLOAD(Linux, sendtoBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/SocketAddress;)I", SocketAddress),
    NATIVE_METHOD(Linux, setegid, "(I)V"),
    NATIVE_METHOD(Linux, setenv, "(Ljava/lang/String;Ljava/lang/String;Z)V"),
    NATIVE_METHOD(Linux, seteuid, "(I)V"),
    NATIVE_METHOD(Linux, setgid, "(I)V"),
    NATIVE_METHOD(Linux, setpgid, "(II)V"),
    NATIVE_METHOD(Linux, setregid, "(II)V"),
    NATIVE_METHOD(Linux, setreuid, "(II)V"),
    NATIVE_METHOD(Linux, setsid, "()I"),
    NATIVE_METHOD(Linux, setsockoptByte, "(Ljava/io/FileDescriptor;III)V"),
    NATIVE_METHOD(Linux, setsockoptIfreq, "(Ljava/io/FileDescriptor;IILjava/lang/String;)V"),
    NATIVE_METHOD(Linux, setsockoptInt, "(Ljava/io/FileDescriptor;III)V"),
    NATIVE_METHOD(Linux, setsockoptIpMreqn, "(Ljava/io/FileDescriptor;III)V"),
    NATIVE_METHOD(Linux, setsockoptGroupReq, "(Ljava/io/FileDescriptor;IILandroid/system/StructGroupReq;)V"),
    NATIVE_METHOD(Linux, setsockoptGroupSourceReq, "(Ljava/io/FileDescriptor;IILandroid/system/StructGroupSourceReq;)V"),
    NATIVE_METHOD(Linux, setsockoptLinger, "(Ljava/io/FileDescriptor;IILandroid/system/StructLinger;)V"),
    NATIVE_METHOD(Linux, setsockoptTimeval, "(Ljava/io/FileDescriptor;IILandroid/system/StructTimeval;)V"),
    NATIVE_METHOD(Linux, setuid, "(I)V"),
    NATIVE_METHOD(Linux, setxattr, "(Ljava/lang/String;Ljava/lang/String;[BI)V"),
    NATIVE_METHOD(Linux, shutdown, "(Ljava/io/FileDescriptor;I)V"),
    NATIVE_METHOD(Linux, socket, "(III)Ljava/io/FileDescriptor;"),
    NATIVE_METHOD(Linux, socketpair, "(IIILjava/io/FileDescriptor;Ljava/io/FileDescriptor;)V"),
    NATIVE_METHOD(Linux, stat, "(Ljava/lang/String;)Landroid/system/StructStat;"),
    NATIVE_METHOD(Linux, statvfs, "(Ljava/lang/String;)Landroid/system/StructStatVfs;"),
    NATIVE_METHOD(Linux, strerror, "(I)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, strsignal, "(I)Ljava/lang/String;"),
    NATIVE_METHOD(Linux, symlink, "(Ljava/lang/String;Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, sysconf, "(I)J"),
    NATIVE_METHOD(Linux, tcdrain, "(Ljava/io/FileDescriptor;)V"),
    NATIVE_METHOD(Linux, tcsendbreak, "(Ljava/io/FileDescriptor;I)V"),
    NATIVE_METHOD(Linux, umaskImpl, "(I)I"),
    NATIVE_METHOD(Linux, uname, "()Landroid/system/StructUtsname;"),
    NATIVE_METHOD(Linux, unlink, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, unsetenv, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(Linux, waitpid, "(ILandroid/util/MutableInt;I)I"),
    NATIVE_METHOD(Linux, writeBytes, "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I"),
    NATIVE_METHOD(Linux, writev, "(Ljava/io/FileDescriptor;[Ljava/lang/Object;[I[I)I"),
};
void register_libcore_io_Linux(JNIEnv* env) {
    jniRegisterNativeMethods(env, "libcore/io/Linux", gMethods, NELEM(gMethods));
}
