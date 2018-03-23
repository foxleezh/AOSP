/*
 * Copyright (C) 2007 The Android Open Source Project
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

#ifndef _INIT_PROPERTY_H
#define _INIT_PROPERTY_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <string>

struct property_audit_data {
    ucred *cr;
    const char* name;
};

void property_init(void);
void property_load_boot_defaults(void);
void load_persist_props(void);
void load_system_props(void);
void start_property_service(void);
uint32_t property_set(const std::string& name, const std::string& value);
bool is_legal_property_name(const std::string& name);


#endif  /* _INIT_PROPERTY_H */
