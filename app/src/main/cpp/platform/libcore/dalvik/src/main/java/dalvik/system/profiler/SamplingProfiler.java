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

package platform.libcore.dalvik.src.main.java.dalvik.system.profiler;

import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.Timer;
import java.util.TimerTask;

/**
 * A sampling profiler. It currently is implemented without any
 * virtual machine support, relying solely on {@code
 * Thread.getStackTrace} to collect samples. As such, the overhead is
 * higher than a native approach and it does not provide insight into
 * where time is spent within native code, but it can still provide
 * useful insight into where a program is spending time.
 *
 * <h3>Usage Example</h3>
 *
 * The following example shows how to use the {@code
 * SamplingProfiler}. It samples the current thread's stack to a depth
 * of 12 stack frame elements over two different measurement periods
 * with samples taken every 100 milliseconds. In then prints the
 * results in hprof format to the standard output.
 *
 * <pre> {@code
 * ThreadSet threadSet = SamplingProfiler.newArrayThreadSet(Thread.currentThread());
 * SamplingProfiler profiler = new SamplingProfiler(12, threadSet);
 * profiler.start(100);
 * // period of measurement
 * profiler.stop();
 * // period of non-measurement
 * profiler.start(100);
 * // another period of measurement
 * profiler.stop();
 * profiler.shutdown();
 * AsciiHprofWriter.write(profiler.getHprofData(), System.out);
 * }</pre>
 */
public final class SamplingProfiler {

    /**
     * Map of stack traces to a mutable sample count.
     */
    private final Map<HprofData.StackTrace, int[]> stackTraces
            = new HashMap<HprofData.StackTrace, int[]>();

    /**
     * Data collected by the sampling profiler
     */
    private final HprofData hprofData = new HprofData(stackTraces);

    /**
     * Timer that is used for the lifetime of the profiler
     */
    private final Timer timer = new Timer("SamplingProfiler", true);

    /**
     * A sampler is created every time profiling starts and cleared
     * every time profiling stops because once a {@code TimerTask} is
     * canceled it cannot be reused.
     */
    private Sampler sampler;

    /**
     * The maximum number of {@code StackTraceElements} to retain in
     * each stack.
     */
    private final int depth;

    /**
     * The {@code ThreadSet} that identifies which threads to sample.
     */
    private final ThreadSet threadSet;

    /*
     *  Real hprof output examples don't start the thread and trace
     *  identifiers at one but seem to start at these arbitrary
     *  constants. It certainly seems useful to have relatively unique
     *  identifiers when manual searching hprof output.
     */
    private int nextThreadId = 200001;
    private int nextStackTraceId = 300001;
    private int nextObjectId = 1;

    /**
     * The threads currently known to the profiler for detecting
     * thread start and end events.
     */
    private Thread[] currentThreads = new Thread[0];

    /**
     * Map of currently active threads to their identifiers. When
     * threads disappear they are removed and only referenced by their
     * identifiers to prevent retaining garbage threads.
     */
    private final Map<Thread, Integer> threadIds = new HashMap<Thread, Integer>();

    /**
     * Mutable {@code StackTrace} that is used for probing the {@link
     * #stackTraces stackTraces} map without allocating a {@code
     * StackTrace}. If {@link #addStackTrace addStackTrace} needs to
     * be thread safe, have a single mutable instance would need to be
     * reconsidered.
     */
    private final HprofData.StackTrace mutableStackTrace = new HprofData.StackTrace();

    /**
     * The {@code ThreadSampler} is used to produce a {@code
     * StackTraceElement} array for a given thread. The array is
     * expected to be {@link #depth depth} or less in length.
     */
    private final ThreadSampler threadSampler;

    /**
     * Create a sampling profiler that collects stacks with the
     * specified depth from the threads specified by the specified
     * thread collector.
     *
     * @param depth The maximum stack depth to retain for each sample
     * similar to the hprof option of the same name. Any stack deeper
     * than this will be truncated to this depth. A good starting
     * value is 4 although it is not uncommon to need to raise this to
     * get enough context to understand program behavior. While
     * programs with extensive recursion may require a high value for
     * depth, simply passing in a value for Integer.MAX_VALUE is not
     * advised because of the significant memory need to retain such
     * stacks and runtime overhead to compare stacks.
     *
     * @param threadSet The thread set specifies which threads to
     * sample. In a general purpose program, all threads typically
     * should be sample with a ThreadSet such as provided by {@link
     * #newThreadGroupThreadSet newThreadGroupThreadSet}. For a
     * benchmark a fixed set such as provided by {@link
     * #newArrayThreadSet newArrayThreadSet} can reduce the overhead
     * of profiling.
     */
    public SamplingProfiler(int depth, ThreadSet threadSet) {
        this.depth = depth;
        this.threadSet = threadSet;
        this.threadSampler = findDefaultThreadSampler();
        threadSampler.setDepth(depth);
        hprofData.setFlags(BinaryHprof.ControlSettings.CPU_SAMPLING.bitmask);
        hprofData.setDepth(depth);
    }

    private static ThreadSampler findDefaultThreadSampler() {
        if ("Dalvik Core Library".equals(System.getProperty("java.specification.name"))) {
            String className = "dalvik.system.profiler.DalvikThreadSampler";
            try {
                return (ThreadSampler) Class.forName(className).newInstance();
            } catch (Exception e) {
                System.out.println("Problem creating " + className + ": " + e);
            }
        }
        return new PortableThreadSampler();
    }

    /**
     * A ThreadSet specifies the set of threads to sample.
     */
    public static interface ThreadSet {
        /**
         * Returns an array containing the threads to be sampled. The
         * array may be longer than the number of threads to be
         * sampled, in which case the extra elements must be null.
         */
        public Thread[] threads();
    }

    /**
     * Returns a ThreadSet for a fixed set of threads that will not
     * vary at runtime. This has less overhead than a dynamically
     * calculated set, such as {@link #newThreadGroupThreadSet}, which has
     * to enumerate the threads each time profiler wants to collect
     * samples.
     */
    public static ThreadSet newArrayThreadSet(Thread... threads) {
        return new ArrayThreadSet(threads);
    }

    /**
     * An ArrayThreadSet samples a fixed set of threads that does not
     * vary over the life of the profiler.
     */
    private static class ArrayThreadSet implements ThreadSet {
        private final Thread[] threads;
        public ArrayThreadSet(Thread... threads) {
            if (threads == null) {
                throw new NullPointerException("threads == null");
            }
            this.threads = threads;
        }
        public Thread[] threads() {
            return threads;
        }
    }

    /**
     * Returns a ThreadSet that is dynamically computed based on the
     * threads found in the specified ThreadGroup and that
     * ThreadGroup's children.
     */
    public static ThreadSet newThreadGroupThreadSet(ThreadGroup threadGroup) {
        return new ThreadGroupThreadSet(threadGroup);
    }

    /**
     * An ThreadGroupThreadSet sample the threads from the specified
     * ThreadGroup and the ThreadGroup's children
     */
    private static class ThreadGroupThreadSet implements ThreadSet {
        private final ThreadGroup threadGroup;
        private Thread[] threads;
        private int lastThread;

        public ThreadGroupThreadSet(ThreadGroup threadGroup) {
            if (threadGroup == null) {
                throw new NullPointerException("threadGroup == null");
            }
            this.threadGroup = threadGroup;
            resize();
        }

        private void resize() {
            int count = threadGroup.activeCount();
            // we can only tell if we had enough room for all active
            // threads if we actually are larger than the the number of
            // active threads. making it larger also leaves us room to
            // tolerate additional threads without resizing.
            threads = new Thread[count*2];
            lastThread = 0;
        }

        public Thread[] threads() {
            int threadCount;
            while (true) {
                threadCount = threadGroup.enumerate(threads);
                if (threadCount == threads.length) {
                    resize();
                } else {
                    break;
                }
            }
            if (threadCount < lastThread) {
                // avoid retaining pointers to threads that have ended
                Arrays.fill(threads, threadCount, lastThread, null);
            }
            lastThread = threadCount;
            return threads;
        }
    }

    /**
     * Starts profiler sampling at the specified rate.
     *
     * @param interval The number of milliseconds between samples
     */
    public void start(int interval) {
        if (interval < 1) {
            throw new IllegalArgumentException("interval < 1");
        }
        if (sampler != null) {
            throw new IllegalStateException("profiling already started");
        }
        sampler = new Sampler();
        hprofData.setStartMillis(System.currentTimeMillis());
        timer.scheduleAtFixedRate(sampler, 0, interval);
    }

    /**
     * Stops profiler sampling. It can be restarted with {@link
     * #start(int)} to continue sampling.
     */
    public void stop() {
        if (sampler == null) {
            return;
        }
        synchronized(sampler) {
            sampler.stop = true;
            while (!sampler.stopped) {
                try {
                    sampler.wait();
                } catch (InterruptedException ignored) {
                }
            }
        }
        sampler = null;
    }

    /**
     * Shuts down profiling after which it can not be restarted. It is
     * important to shut down profiling when done to free resources
     * used by the profiler. Shutting down the profiler also stops the
     * profiling if that has not already been done.
     */
    public void shutdown() {
        stop();
        timer.cancel();
    }

    /**
     * Returns the hprof data accumulated by the profiler since it was
     * created. The profiler needs to be stopped, but not necessarily
     * shut down, in order to access the data. If the profiler is
     * restarted, there is no thread safe way to access the data.
     */
    public HprofData getHprofData() {
        if (sampler != null) {
            throw new IllegalStateException("cannot access hprof data while sampling");
        }
        return hprofData;
    }

    /**
     * The Sampler does the real work of the profiler.
     *
     * At every sample time, it asks the thread set for the set
     * of threads to sample. It maintains a history of thread creation
     * and death events based on changes observed to the threads
     * returned by the {@code ThreadSet}.
     *
     * For each thread to be sampled, a stack is collected and used to
     * update the set of collected samples. Stacks are truncated to a
     * maximum depth. There is no way to tell if a stack has been truncated.
     */
    private class Sampler extends TimerTask {

        private boolean stop;
        private boolean stopped;

        private Thread timerThread;

        public void run() {
            synchronized(this) {
                if (stop) {
                    cancel();
                    stopped = true;
                    notifyAll();
                    return;
                }
            }

            if (timerThread == null) {
                timerThread = Thread.currentThread();
            }

            // process thread creation and death first so that we
            // assign thread ids to any new threads before allocating
            // new stacks for them
            Thread[] newThreads = threadSet.threads();
            if (!Arrays.equals(currentThreads, newThreads)) {
                updateThreadHistory(currentThreads, newThreads);
                currentThreads = newThreads.clone();
            }

            for (Thread thread : currentThreads) {
                if (thread == null) {
                    break;
                }
                if (thread == timerThread) {
                    continue;
                }

                StackTraceElement[] stackFrames = threadSampler.getStackTrace(thread);
                if (stackFrames == null) {
                    continue;
                }
                recordStackTrace(thread, stackFrames);
            }
        }

        /**
         * Record a new stack trace. The thread should have been
         * previously registered with addStartThread.
         */
        private void recordStackTrace(Thread thread, StackTraceElement[] stackFrames) {
            Integer threadId = threadIds.get(thread);
            if (threadId == null) {
                throw new IllegalArgumentException("Unknown thread " + thread);
            }
            mutableStackTrace.threadId = threadId;
            mutableStackTrace.stackFrames = stackFrames;

            int[] countCell = stackTraces.get(mutableStackTrace);
            if (countCell == null) {
                countCell = new int[1];
                // cloned because the ThreadSampler may reuse the array
                StackTraceElement[] stackFramesCopy = stackFrames.clone();
                HprofData.StackTrace stackTrace
                        = new HprofData.StackTrace(nextStackTraceId++, threadId, stackFramesCopy);
                hprofData.addStackTrace(stackTrace, countCell);
            }
            countCell[0]++;
        }

        private void updateThreadHistory(Thread[] oldThreads, Thread[] newThreads) {
            // thread start/stop shouldn't happen too often and
            // these aren't too big, so hopefully this approach
            // won't be too slow...
            Set<Thread> n = new HashSet<Thread>(Arrays.asList(newThreads));
            Set<Thread> o = new HashSet<Thread>(Arrays.asList(oldThreads));

            // added = new-old
            Set<Thread> added = new HashSet<Thread>(n);
            added.removeAll(o);

            // removed = old-new
            Set<Thread> removed = new HashSet<Thread>(o);
            removed.removeAll(n);

            for (Thread thread : added) {
                if (thread == null) {
                    continue;
                }
                if (thread == timerThread) {
                    continue;
                }
                addStartThread(thread);
            }
            for (Thread thread : removed) {
                if (thread == null) {
                    continue;
                }
                if (thread == timerThread) {
                    continue;
                }
                addEndThread(thread);
            }
        }

        /**
         * Record that a newly noticed thread.
         */
        private void addStartThread(Thread thread) {
            if (thread == null) {
                throw new NullPointerException("thread == null");
            }
            int threadId = nextThreadId++;
            Integer old = threadIds.put(thread, threadId);
            if (old != null) {
                throw new IllegalArgumentException("Thread already registered as " + old);
            }

            String threadName = thread.getName();
            // group will become null when thread is terminated
            ThreadGroup group = thread.getThreadGroup();
            String groupName = group == null ? null : group.getName();
            ThreadGroup parentGroup = group == null ? null : group.getParent();
            String parentGroupName = parentGroup == null ? null : parentGroup.getName();

            HprofData.ThreadEvent event
                    = HprofData.ThreadEvent.start(nextObjectId++, threadId,
                                                  threadName, groupName, parentGroupName);
            hprofData.addThreadEvent(event);
        }

        /**
         * Record that a thread has disappeared.
         */
        private void addEndThread(Thread thread) {
            if (thread == null) {
                throw new NullPointerException("thread == null");
            }
            Integer threadId = threadIds.remove(thread);
            if (threadId == null) {
                throw new IllegalArgumentException("Unknown thread " + thread);
            }
            HprofData.ThreadEvent event = HprofData.ThreadEvent.end(threadId);
            hprofData.addThreadEvent(event);
        }
    }
}
