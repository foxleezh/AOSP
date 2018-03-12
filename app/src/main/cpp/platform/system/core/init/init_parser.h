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

#ifndef _INIT_INIT_PARSER_H_
#define _INIT_INIT_PARSER_H_

#include <map>
#include <string>
#include <vector>

class SectionParser {
public:
    virtual ~SectionParser() {
    }
    virtual bool ParseSection(const std::vector<std::string>& args,
                              std::string* err) = 0;
    virtual bool ParseLineSection(const std::vector<std::string>& args,
                                  const std::string& filename, int line,
                                  std::string* err) const = 0;
    virtual void EndSection() = 0;
    virtual void EndFile(const std::string& filename) = 0;
};

class Parser {
public:
    static Parser& GetInstance();
    void DumpState() const;
    bool ParseConfig(const std::string& path);
    void AddSectionParser(const std::string& name,
                          std::unique_ptr<SectionParser> parser);
    void set_is_system_etc_init_loaded(bool loaded) {
        is_system_etc_init_loaded_ = loaded;
    }
    void set_is_vendor_etc_init_loaded(bool loaded) {
        is_vendor_etc_init_loaded_ = loaded;
    }
    void set_is_odm_etc_init_loaded(bool loaded) {
        is_odm_etc_init_loaded_ = loaded;
    }
    bool is_system_etc_init_loaded() { return is_system_etc_init_loaded_; }
    bool is_vendor_etc_init_loaded() { return is_vendor_etc_init_loaded_; }
    bool is_odm_etc_init_loaded() { return is_odm_etc_init_loaded_; }

private:
    Parser();

    void ParseData(const std::string& filename, const std::string& data);
    bool ParseConfigFile(const std::string& path);
    bool ParseConfigDir(const std::string& path);

    std::map<std::string, std::unique_ptr<SectionParser>> section_parsers_;
    bool is_system_etc_init_loaded_ = false;
    bool is_vendor_etc_init_loaded_ = false;
    bool is_odm_etc_init_loaded_ = false;
};

#endif
