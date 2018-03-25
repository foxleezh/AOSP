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

package libcore.io;

import android.system.ErrnoException;
import android.system.GaiException;
import android.system.StructAddrinfo;
import android.system.StructCapUserData;
import android.system.StructCapUserHeader;
import android.system.StructFlock;
import android.system.StructGroupReq;
import android.system.StructGroupSourceReq;
import android.system.StructIfaddrs;
import android.system.StructLinger;
import android.system.StructPasswd;
import android.system.StructPollfd;
import android.system.StructStat;
import android.system.StructStatVfs;
import android.system.StructTimeval;
import android.system.StructUcred;
import android.system.StructUtsname;
import android.util.MutableInt;
import android.util.MutableLong;
import java.io.FileDescriptor;
import java.io.InterruptedIOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketException;
import java.nio.ByteBuffer;
import java.nio.NioUtils;

public final class Linux implements Os {
    Linux() { }

    public native FileDescriptor accept(FileDescriptor fd, SocketAddress peerAddress) throws ErrnoException, SocketException;
    public native boolean access(String path, int mode) throws ErrnoException;
    public native InetAddress[] android_getaddrinfo(String node, StructAddrinfo hints, int netId) throws GaiException;
    public native void bind(FileDescriptor fd, InetAddress address, int port) throws ErrnoException, SocketException;
    public native void bind(FileDescriptor fd, SocketAddress address) throws ErrnoException, SocketException;
    @Override
    public native StructCapUserData[] capget(StructCapUserHeader hdr) throws ErrnoException;
    @Override
    public native void capset(StructCapUserHeader hdr, StructCapUserData[] data)
            throws ErrnoException;
    public native void chmod(String path, int mode) throws ErrnoException;
    public native void chown(String path, int uid, int gid) throws ErrnoException;
    public native void close(FileDescriptor fd) throws ErrnoException;
    public native void connect(FileDescriptor fd, InetAddress address, int port) throws ErrnoException, SocketException;
    public native void connect(FileDescriptor fd, SocketAddress address) throws ErrnoException, SocketException;
    public native FileDescriptor dup(FileDescriptor oldFd) throws ErrnoException;
    public native FileDescriptor dup2(FileDescriptor oldFd, int newFd) throws ErrnoException;
    public native String[] environ();
    public native void execv(String filename, String[] argv) throws ErrnoException;
    public native void execve(String filename, String[] argv, String[] envp) throws ErrnoException;
    public native void fchmod(FileDescriptor fd, int mode) throws ErrnoException;
    public native void fchown(FileDescriptor fd, int uid, int gid) throws ErrnoException;
    public native int fcntlFlock(FileDescriptor fd, int cmd, StructFlock arg) throws ErrnoException, InterruptedIOException;
    public native int fcntlInt(FileDescriptor fd, int cmd, int arg) throws ErrnoException;
    public native int fcntlVoid(FileDescriptor fd, int cmd) throws ErrnoException;
    public native void fdatasync(FileDescriptor fd) throws ErrnoException;
    public native StructStat fstat(FileDescriptor fd) throws ErrnoException;
    public native StructStatVfs fstatvfs(FileDescriptor fd) throws ErrnoException;
    public native void fsync(FileDescriptor fd) throws ErrnoException;
    public native void ftruncate(FileDescriptor fd, long length) throws ErrnoException;
    public native String gai_strerror(int error);
    public native int getegid();
    public native int geteuid();
    public native int getgid();
    public native String getenv(String name);
    public native String getnameinfo(InetAddress address, int flags) throws GaiException;
    public native SocketAddress getpeername(FileDescriptor fd) throws ErrnoException;
    public native int getpgid(int pid);
    public native int getpid();
    public native int getppid();
    public native StructPasswd getpwnam(String name) throws ErrnoException;
    public native StructPasswd getpwuid(int uid) throws ErrnoException;
    public native SocketAddress getsockname(FileDescriptor fd) throws ErrnoException;
    public native int getsockoptByte(FileDescriptor fd, int level, int option) throws ErrnoException;
    public native InetAddress getsockoptInAddr(FileDescriptor fd, int level, int option) throws ErrnoException;
    public native int getsockoptInt(FileDescriptor fd, int level, int option) throws ErrnoException;
    public native StructLinger getsockoptLinger(FileDescriptor fd, int level, int option) throws ErrnoException;
    public native StructTimeval getsockoptTimeval(FileDescriptor fd, int level, int option) throws ErrnoException;
    public native StructUcred getsockoptUcred(FileDescriptor fd, int level, int option) throws ErrnoException;
    public native int gettid();
    public native int getuid();
    public native byte[] getxattr(String path, String name) throws ErrnoException;
    public native StructIfaddrs[] getifaddrs() throws ErrnoException;
    public native String if_indextoname(int index);
    public native int if_nametoindex(String name);
    public native InetAddress inet_pton(int family, String address);
    public native int ioctlFlags(FileDescriptor fd, String interfaceName) throws ErrnoException;
    public native InetAddress ioctlInetAddress(FileDescriptor fd, int cmd, String interfaceName) throws ErrnoException;
    public native int ioctlInt(FileDescriptor fd, int cmd, MutableInt arg) throws ErrnoException;
    public native int ioctlMTU(FileDescriptor fd, String interfaceName) throws ErrnoException;
    public native boolean isatty(FileDescriptor fd);
    public native void kill(int pid, int signal) throws ErrnoException;
    public native void lchown(String path, int uid, int gid) throws ErrnoException;
    public native void link(String oldPath, String newPath) throws ErrnoException;
    public native void listen(FileDescriptor fd, int backlog) throws ErrnoException;
    public native String[] listxattr(String path) throws ErrnoException;
    public native long lseek(FileDescriptor fd, long offset, int whence) throws ErrnoException;
    public native StructStat lstat(String path) throws ErrnoException;
    public native void mincore(long address, long byteCount, byte[] vector) throws ErrnoException;
    public native void mkdir(String path, int mode) throws ErrnoException;
    public native void mkfifo(String path, int mode) throws ErrnoException;
    public native void mlock(long address, long byteCount) throws ErrnoException;
    public native long mmap(long address, long byteCount, int prot, int flags, FileDescriptor fd, long offset) throws ErrnoException;
    public native void msync(long address, long byteCount, int flags) throws ErrnoException;
    public native void munlock(long address, long byteCount) throws ErrnoException;
    public native void munmap(long address, long byteCount) throws ErrnoException;
    public native FileDescriptor open(String path, int flags, int mode) throws ErrnoException;
    public native FileDescriptor[] pipe2(int flags) throws ErrnoException;
    public native int poll(StructPollfd[] fds, int timeoutMs) throws ErrnoException;
    public native void posix_fallocate(FileDescriptor fd, long offset, long length) throws ErrnoException;
    public native int prctl(int option, long arg2, long arg3, long arg4, long arg5) throws ErrnoException;
    public int pread(FileDescriptor fd, ByteBuffer buffer, long offset) throws ErrnoException, InterruptedIOException {
        final int bytesRead;
        final int position = buffer.position();

        if (buffer.isDirect()) {
            bytesRead = preadBytes(fd, buffer, position, buffer.remaining(), offset);
        } else {
            bytesRead = preadBytes(fd, NioUtils.unsafeArray(buffer), NioUtils.unsafeArrayOffset(buffer) + position, buffer.remaining(), offset);
        }

        maybeUpdateBufferPosition(buffer, position, bytesRead);
        return bytesRead;
    }
    public int pread(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, long offset) throws ErrnoException, InterruptedIOException {
        // This indirection isn't strictly necessary, but ensures that our public interface is type safe.
        return preadBytes(fd, bytes, byteOffset, byteCount, offset);
    }
    private native int preadBytes(FileDescriptor fd, Object buffer, int bufferOffset, int byteCount, long offset) throws ErrnoException, InterruptedIOException;
    public int pwrite(FileDescriptor fd, ByteBuffer buffer, long offset) throws ErrnoException, InterruptedIOException {
        final int bytesWritten;
        final int position = buffer.position();

        if (buffer.isDirect()) {
            bytesWritten = pwriteBytes(fd, buffer, position, buffer.remaining(), offset);
        } else {
            bytesWritten = pwriteBytes(fd, NioUtils.unsafeArray(buffer), NioUtils.unsafeArrayOffset(buffer) + position, buffer.remaining(), offset);
        }

        maybeUpdateBufferPosition(buffer, position, bytesWritten);
        return bytesWritten;
    }
    public int pwrite(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, long offset) throws ErrnoException, InterruptedIOException {
        // This indirection isn't strictly necessary, but ensures that our public interface is type safe.
        return pwriteBytes(fd, bytes, byteOffset, byteCount, offset);
    }
    private native int pwriteBytes(FileDescriptor fd, Object buffer, int bufferOffset, int byteCount, long offset) throws ErrnoException, InterruptedIOException;
    public int read(FileDescriptor fd, ByteBuffer buffer) throws ErrnoException, InterruptedIOException {
        final int bytesRead;
        final int position = buffer.position();

        if (buffer.isDirect()) {
            bytesRead = readBytes(fd, buffer, position, buffer.remaining());
        } else {
            bytesRead = readBytes(fd, NioUtils.unsafeArray(buffer), NioUtils.unsafeArrayOffset(buffer) + position, buffer.remaining());
        }

        maybeUpdateBufferPosition(buffer, position, bytesRead);
        return bytesRead;
    }
    public int read(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount) throws ErrnoException, InterruptedIOException {
        // This indirection isn't strictly necessary, but ensures that our public interface is type safe.
        return readBytes(fd, bytes, byteOffset, byteCount);
    }
    private native int readBytes(FileDescriptor fd, Object buffer, int offset, int byteCount) throws ErrnoException, InterruptedIOException;
    public native String readlink(String path) throws ErrnoException;
    public native String realpath(String path) throws ErrnoException;
    public native int readv(FileDescriptor fd, Object[] buffers, int[] offsets, int[] byteCounts) throws ErrnoException, InterruptedIOException;
    public int recvfrom(FileDescriptor fd, ByteBuffer buffer, int flags, InetSocketAddress srcAddress) throws ErrnoException, SocketException {
        final int bytesReceived;
        final int position = buffer.position();

        if (buffer.isDirect()) {
            bytesReceived = recvfromBytes(fd, buffer, position, buffer.remaining(), flags, srcAddress);
        } else {
            bytesReceived = recvfromBytes(fd, NioUtils.unsafeArray(buffer), NioUtils.unsafeArrayOffset(buffer) + position, buffer.remaining(), flags, srcAddress);
        }

        maybeUpdateBufferPosition(buffer, position, bytesReceived);
        return bytesReceived;
    }
    public int recvfrom(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, int flags, InetSocketAddress srcAddress) throws ErrnoException, SocketException {
        // This indirection isn't strictly necessary, but ensures that our public interface is type safe.
        return recvfromBytes(fd, bytes, byteOffset, byteCount, flags, srcAddress);
    }
    private native int recvfromBytes(FileDescriptor fd, Object buffer, int byteOffset, int byteCount, int flags, InetSocketAddress srcAddress) throws ErrnoException, SocketException;
    public native void remove(String path) throws ErrnoException;
    public native void removexattr(String path, String name) throws ErrnoException;
    public native void rename(String oldPath, String newPath) throws ErrnoException;
    public native long sendfile(FileDescriptor outFd, FileDescriptor inFd, MutableLong inOffset, long byteCount) throws ErrnoException;
    public int sendto(FileDescriptor fd, ByteBuffer buffer, int flags, InetAddress inetAddress, int port) throws ErrnoException, SocketException {
        final int bytesSent;
        final int position = buffer.position();

        if (buffer.isDirect()) {
            bytesSent = sendtoBytes(fd, buffer, position, buffer.remaining(), flags, inetAddress, port);
        } else {
            bytesSent = sendtoBytes(fd, NioUtils.unsafeArray(buffer), NioUtils.unsafeArrayOffset(buffer) + position, buffer.remaining(), flags, inetAddress, port);
        }

        maybeUpdateBufferPosition(buffer, position, bytesSent);
        return bytesSent;
    }
    public int sendto(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, int flags, InetAddress inetAddress, int port) throws ErrnoException, SocketException {
        // This indirection isn't strictly necessary, but ensures that our public interface is type safe.
        return sendtoBytes(fd, bytes, byteOffset, byteCount, flags, inetAddress, port);
    }
    public int sendto(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, int flags, SocketAddress address) throws ErrnoException, SocketException {
        return sendtoBytes(fd, bytes, byteOffset, byteCount, flags, address);
    }
    private native int sendtoBytes(FileDescriptor fd, Object buffer, int byteOffset, int byteCount, int flags, InetAddress inetAddress, int port) throws ErrnoException, SocketException;
    private native int sendtoBytes(FileDescriptor fd, Object buffer, int byteOffset, int byteCount, int flags, SocketAddress address) throws ErrnoException, SocketException;
    public native void setegid(int egid) throws ErrnoException;
    public native void setenv(String name, String value, boolean overwrite) throws ErrnoException;
    public native void seteuid(int euid) throws ErrnoException;
    public native void setgid(int gid) throws ErrnoException;
    public native void setpgid(int pid, int pgid) throws ErrnoException;
    public native void setregid(int rgid, int egid) throws ErrnoException;
    public native void setreuid(int ruid, int euid) throws ErrnoException;
    public native int setsid() throws ErrnoException;
    public native void setsockoptByte(FileDescriptor fd, int level, int option, int value) throws ErrnoException;
    public native void setsockoptIfreq(FileDescriptor fd, int level, int option, String value) throws ErrnoException;
    public native void setsockoptInt(FileDescriptor fd, int level, int option, int value) throws ErrnoException;
    public native void setsockoptIpMreqn(FileDescriptor fd, int level, int option, int value) throws ErrnoException;
    public native void setsockoptGroupReq(FileDescriptor fd, int level, int option, StructGroupReq value) throws ErrnoException;
    public native void setsockoptGroupSourceReq(FileDescriptor fd, int level, int option, StructGroupSourceReq value) throws ErrnoException;
    public native void setsockoptLinger(FileDescriptor fd, int level, int option, StructLinger value) throws ErrnoException;
    public native void setsockoptTimeval(FileDescriptor fd, int level, int option, StructTimeval value) throws ErrnoException;
    public native void setuid(int uid) throws ErrnoException;
    public native void setxattr(String path, String name, byte[] value, int flags) throws ErrnoException;
    public native void shutdown(FileDescriptor fd, int how) throws ErrnoException;
    public native FileDescriptor socket(int domain, int type, int protocol) throws ErrnoException;
    public native void socketpair(int domain, int type, int protocol, FileDescriptor fd1, FileDescriptor fd2) throws ErrnoException;
    public native StructStat stat(String path) throws ErrnoException;
    public native StructStatVfs statvfs(String path) throws ErrnoException;
    public native String strerror(int errno);
    public native String strsignal(int signal);
    public native void symlink(String oldPath, String newPath) throws ErrnoException;
    public native long sysconf(int name);
    public native void tcdrain(FileDescriptor fd) throws ErrnoException;
    public native void tcsendbreak(FileDescriptor fd, int duration) throws ErrnoException;
    public int umask(int mask) {
        if ((mask & 0777) != mask) {
            throw new IllegalArgumentException("Invalid umask: " + mask);
        }
        return umaskImpl(mask);
    }
    private native int umaskImpl(int mask);
    public native StructUtsname uname();
    public native void unlink(String pathname) throws ErrnoException;
    public native void unsetenv(String name) throws ErrnoException;
    public native int waitpid(int pid, MutableInt status, int options) throws ErrnoException;
    public int write(FileDescriptor fd, ByteBuffer buffer) throws ErrnoException, InterruptedIOException {
        final int bytesWritten;
        final int position = buffer.position();
        if (buffer.isDirect()) {
            bytesWritten = writeBytes(fd, buffer, position, buffer.remaining());
        } else {
            bytesWritten = writeBytes(fd, NioUtils.unsafeArray(buffer), NioUtils.unsafeArrayOffset(buffer) + position, buffer.remaining());
        }

        maybeUpdateBufferPosition(buffer, position, bytesWritten);
        return bytesWritten;
    }
    public int write(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount) throws ErrnoException, InterruptedIOException {
        // This indirection isn't strictly necessary, but ensures that our public interface is type safe.
        return writeBytes(fd, bytes, byteOffset, byteCount);
    }
    private native int writeBytes(FileDescriptor fd, Object buffer, int offset, int byteCount) throws ErrnoException, InterruptedIOException;
    public native int writev(FileDescriptor fd, Object[] buffers, int[] offsets, int[] byteCounts) throws ErrnoException, InterruptedIOException;

    private static void maybeUpdateBufferPosition(ByteBuffer buffer, int originalPosition, int bytesReadOrWritten) {
        if (bytesReadOrWritten > 0) {
            buffer.position(bytesReadOrWritten + originalPosition);
        }
    }
}
