/*
 * Copyright (C) 2006 The Android Open Source Project
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

package dalvik.system;

import java.io.File;

/**
 * Provides hooks for the zygote to call back into the runtime to perform
 * parent or child specific initialization..
 *
 * @hide
 */
public final class ZygoteHooks {
    private long token;

    /**
     * Called by the zygote when starting up. It marks the point when any thread
     * start should be an error, as only internal daemon threads are allowed there.
     */
    public static native void startZygoteNoThreadCreation();

    /**
     * Called by the zygote when startup is finished. It marks the point when it is
     * conceivable that threads would be started again, e.g., restarting daemons.
     */
    public static native void stopZygoteNoThreadCreation();

    /**
     * Called by the zygote prior to every fork. Each call to {@code preFork}
     * is followed by a matching call to {@link #postForkChild(int, String)} on the child
     * process and {@link #postForkCommon()} on both the parent and the child
     * process. {@code postForkCommon} is called after {@code postForkChild} in
     * the child process.
     */
    public void preFork() {
        Daemons.stop();
        waitUntilAllThreadsStopped();
        token = nativePreFork();
    }

    /**
     * Called by the zygote in the child process after every fork. The debug
     * flags from {@code debugFlags} are applied to the child process. The string
     * {@code instructionSet} determines whether to use a native bridge.
     */
    public void postForkChild(int debugFlags, boolean isSystemServer, String instructionSet) {
        nativePostForkChild(token, debugFlags, isSystemServer, instructionSet);

        Math.setRandomSeedInternal(System.currentTimeMillis());
    }

    /**
     * Called by the zygote in both the parent and child processes after
     * every fork. In the child process, this method is called after
     * {@code postForkChild}.
     */
    public void postForkCommon() {
        Daemons.startPostZygoteFork();
    }

    private static native long nativePreFork();
    private static native void nativePostForkChild(long token, int debugFlags,
                                                   boolean isSystemServer, String instructionSet);

    /**
     * We must not fork until we're single-threaded again. Wait until /proc shows we're
     * down to just one thread.
     */
    private static void waitUntilAllThreadsStopped() {
        File tasks = new File("/proc/self/task");
        // All Java daemons are stopped already. We're just waiting for their OS counterparts to
        // finish as well. This shouldn't take much time so spinning is ok here.
        while (tasks.list().length > 1) {
          Thread.yield();
        }
    }
}
