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
import android.system.OsConstants;
import android.system.StructLinger;
import android.system.StructPollfd;
import android.system.StructStat;
import android.system.StructStatVfs;
import android.util.MutableLong;
import dalvik.system.BlockGuard;
import dalvik.system.SocketTagger;
import java.io.FileDescriptor;
import java.io.InterruptedIOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketException;
import java.nio.ByteBuffer;
import static android.system.OsConstants.*;

/**
 * Informs BlockGuard of any activity it should be aware of.
 */
public class BlockGuardOs extends ForwardingOs {
    public BlockGuardOs(Os os) {
        super(os);
    }

    private FileDescriptor tagSocket(FileDescriptor fd) throws ErrnoException {
        try {
            SocketTagger.get().tag(fd);
            return fd;
        } catch (SocketException e) {
            throw new ErrnoException("socket", EINVAL, e);
        }
    }

    private void untagSocket(FileDescriptor fd) throws ErrnoException {
        try {
            SocketTagger.get().untag(fd);
        } catch (SocketException e) {
            throw new ErrnoException("socket", EINVAL, e);
        }
    }

    @Override public FileDescriptor accept(FileDescriptor fd, SocketAddress peerAddress) throws ErrnoException, SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        final FileDescriptor acceptFd = os.accept(fd, peerAddress);
        if (isInetSocket(acceptFd)) {
            tagSocket(acceptFd);
        }
        return acceptFd;
    }

    @Override public boolean access(String path, int mode) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.access(path, mode);
    }

    @Override public void chmod(String path, int mode) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.chmod(path, mode);
    }

    @Override public void chown(String path, int uid, int gid) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.chown(path, uid, gid);
    }

    @Override public void close(FileDescriptor fd) throws ErrnoException {
        try {
            // The usual case is that this _isn't_ a socket, so the getsockopt(2) call in
            // isLingerSocket will throw, and that's really expensive. Try to avoid asking
            // if we don't care.
            if (fd.isSocket$()) {
                if (isLingerSocket(fd)) {
                    // If the fd is a socket with SO_LINGER set, we might block indefinitely.
                    // We allow non-linger sockets so that apps can close their network
                    // connections in methods like onDestroy which will run on the UI thread.
                    BlockGuard.getThreadPolicy().onNetwork();
                }
                if (isInetSocket(fd)) {
                    untagSocket(fd);
                }
            }
        } catch (ErrnoException ignored) {
            // We're called via Socket.close (which doesn't ask for us to be called), so we
            // must not throw here, because Socket.close must not throw if asked to close an
            // already-closed socket. Also, the passed-in FileDescriptor isn't necessarily
            // a socket at all.
        }
        os.close(fd);
    }

    private static boolean isInetSocket(FileDescriptor fd) throws ErrnoException{
        return isInetDomain(Libcore.os.getsockoptInt(fd, SOL_SOCKET, SO_DOMAIN));
    }

    private static boolean isInetDomain(int domain) {
        return (domain == AF_INET) || (domain == AF_INET6);
    }

    private static boolean isLingerSocket(FileDescriptor fd) throws ErrnoException {
        StructLinger linger = Libcore.os.getsockoptLinger(fd, SOL_SOCKET, SO_LINGER);
        return linger.isOn() && linger.l_linger > 0;
    }

    @Override public void connect(FileDescriptor fd, InetAddress address, int port) throws ErrnoException, SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        os.connect(fd, address, port);
    }

    @Override public void connect(FileDescriptor fd, SocketAddress address) throws ErrnoException,
            SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        os.connect(fd, address);
    }

    @Override public void fchmod(FileDescriptor fd, int mode) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.fchmod(fd, mode);
    }

    @Override public void fchown(FileDescriptor fd, int uid, int gid) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.fchown(fd, uid, gid);
    }

    // TODO: Untag newFd when needed for dup2(FileDescriptor oldFd, int newFd)

    @Override public void fdatasync(FileDescriptor fd) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.fdatasync(fd);
    }

    @Override public StructStat fstat(FileDescriptor fd) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.fstat(fd);
    }

    @Override public StructStatVfs fstatvfs(FileDescriptor fd) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.fstatvfs(fd);
    }

    @Override public void fsync(FileDescriptor fd) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.fsync(fd);
    }

    @Override public void ftruncate(FileDescriptor fd, long length) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.ftruncate(fd, length);
    }

    @Override public void lchown(String path, int uid, int gid) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.lchown(path, uid, gid);
    }

    @Override public void link(String oldPath, String newPath) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.link(oldPath, newPath);
    }

    @Override public long lseek(FileDescriptor fd, long offset, int whence) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.lseek(fd, offset, whence);
    }

    @Override public StructStat lstat(String path) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.lstat(path);
    }

    @Override public void mkdir(String path, int mode) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.mkdir(path, mode);
    }

    @Override public void mkfifo(String path, int mode) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.mkfifo(path, mode);
    }

    @Override public FileDescriptor open(String path, int flags, int mode) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        if ((flags & O_ACCMODE) != O_RDONLY) {
            BlockGuard.getThreadPolicy().onWriteToDisk();
        }
        return os.open(path, flags, mode);
    }

    @Override public int poll(StructPollfd[] fds, int timeoutMs) throws ErrnoException {
        // Greater than 0 is a timeout in milliseconds and -1 means "block forever",
        // but 0 means "poll and return immediately", which shouldn't be subject to BlockGuard.
        if (timeoutMs != 0) {
            BlockGuard.getThreadPolicy().onNetwork();
        }
        return os.poll(fds, timeoutMs);
    }

    @Override public void posix_fallocate(FileDescriptor fd, long offset, long length) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.posix_fallocate(fd, offset, length);
    }

    @Override public int pread(FileDescriptor fd, ByteBuffer buffer, long offset) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.pread(fd, buffer, offset);
    }

    @Override public int pread(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, long offset) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.pread(fd, bytes, byteOffset, byteCount, offset);
    }

    @Override public int pwrite(FileDescriptor fd, ByteBuffer buffer, long offset) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        return os.pwrite(fd, buffer, offset);
    }

    @Override public int pwrite(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, long offset) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        return os.pwrite(fd, bytes, byteOffset, byteCount, offset);
    }

    @Override public int read(FileDescriptor fd, ByteBuffer buffer) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.read(fd, buffer);
    }

    @Override public int read(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.read(fd, bytes, byteOffset, byteCount);
    }

    @Override public String readlink(String path) throws ErrnoException {
      BlockGuard.getThreadPolicy().onReadFromDisk();
      return os.readlink(path);
    }

    @Override public String realpath(String path) throws ErrnoException {
      BlockGuard.getThreadPolicy().onReadFromDisk();
      return os.realpath(path);
    }

    @Override public int readv(FileDescriptor fd, Object[] buffers, int[] offsets, int[] byteCounts) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.readv(fd, buffers, offsets, byteCounts);
    }

    @Override public int recvfrom(FileDescriptor fd, ByteBuffer buffer, int flags, InetSocketAddress srcAddress) throws ErrnoException, SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        return os.recvfrom(fd, buffer, flags, srcAddress);
    }

    @Override public int recvfrom(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, int flags, InetSocketAddress srcAddress) throws ErrnoException, SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        return os.recvfrom(fd, bytes, byteOffset, byteCount, flags, srcAddress);
    }

    @Override public void remove(String path) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.remove(path);
    }

    @Override public void rename(String oldPath, String newPath) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.rename(oldPath, newPath);
    }

    @Override public long sendfile(FileDescriptor outFd, FileDescriptor inFd, MutableLong inOffset, long byteCount) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        return os.sendfile(outFd, inFd, inOffset, byteCount);
    }

    @Override public int sendto(FileDescriptor fd, ByteBuffer buffer, int flags, InetAddress inetAddress, int port) throws ErrnoException, SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        return os.sendto(fd, buffer, flags, inetAddress, port);
    }

    @Override public int sendto(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount, int flags, InetAddress inetAddress, int port) throws ErrnoException, SocketException {
        // We permit datagrams without hostname lookups.
        if (inetAddress != null) {
            BlockGuard.getThreadPolicy().onNetwork();
        }
        return os.sendto(fd, bytes, byteOffset, byteCount, flags, inetAddress, port);
    }

    @Override public FileDescriptor socket(int domain, int type, int protocol) throws ErrnoException {
        final FileDescriptor fd = os.socket(domain, type, protocol);
        if (isInetDomain(domain)) {
            tagSocket(fd);
        }
        return fd;
    }

    @Override public void socketpair(int domain, int type, int protocol, FileDescriptor fd1, FileDescriptor fd2) throws ErrnoException {
        os.socketpair(domain, type, protocol, fd1, fd2);
        if (isInetDomain(domain)) {
            tagSocket(fd1);
            tagSocket(fd2);
        }
    }

    @Override public StructStat stat(String path) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.stat(path);
    }

    @Override public StructStatVfs statvfs(String path) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.statvfs(path);
    }

    @Override public void symlink(String oldPath, String newPath) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.symlink(oldPath, newPath);
    }

    @Override public int write(FileDescriptor fd, ByteBuffer buffer) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        return os.write(fd, buffer);
    }

    @Override public int write(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        return os.write(fd, bytes, byteOffset, byteCount);
    }

    @Override public int writev(FileDescriptor fd, Object[] buffers, int[] offsets, int[] byteCounts) throws ErrnoException, InterruptedIOException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        return os.writev(fd, buffers, offsets, byteCounts);
    }

    @Override public void execv(String filename, String[] argv) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        os.execv(filename, argv);
    }

    @Override public void execve(String filename, String[] argv, String[] envp)
            throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        os.execve(filename, argv, envp);
    }

    @Override public byte[] getxattr(String path, String name) throws ErrnoException {
        BlockGuard.getThreadPolicy().onReadFromDisk();
        return os.getxattr(path, name);
    }

    @Override public void msync(long address, long byteCount, int flags) throws ErrnoException {
        if ((flags & OsConstants.MS_SYNC) != 0) {
            BlockGuard.getThreadPolicy().onWriteToDisk();
        }
        os.msync(address, byteCount, flags);
    }

    @Override public void removexattr(String path, String name) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.removexattr(path, name);
    }

    @Override public void setxattr(String path, String name, byte[] value, int flags)
            throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.setxattr(path, name, value, flags);
    }

    @Override public int sendto(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount,
            int flags, SocketAddress address) throws ErrnoException, SocketException {
        BlockGuard.getThreadPolicy().onNetwork();
        return os.sendto(fd, bytes, byteOffset, byteCount, flags, address);
    }

    @Override public void unlink(String pathname) throws ErrnoException {
        BlockGuard.getThreadPolicy().onWriteToDisk();
        os.unlink(pathname);
    }
}
