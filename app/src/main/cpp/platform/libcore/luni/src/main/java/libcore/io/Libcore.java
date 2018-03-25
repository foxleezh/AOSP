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

public final class Libcore {
    private Libcore() { }

    /**
     * Direct access to syscalls. Code should strongly prefer using {@link #os}
     * unless it has a strong reason to bypass the helpful checks/guards that it
     * provides.
     */
    public static Os rawOs = new Linux();

    /**
     * Access to syscalls with helpful checks/guards.
     */
    public static Os os = new BlockGuardOs(rawOs);
}
