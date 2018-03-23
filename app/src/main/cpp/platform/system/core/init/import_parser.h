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

#ifndef _INIT_IMPORT_PARSER_H
#define _INIT_IMPORT_PARSER_H

#include "init_parser.h"

#include <string>
#include <vector>

class ImportParser : public SectionParser {
public:
    ImportParser()  {
    }
    bool ParseSection(const std::vector<std::string>& args,
                      std::string* err) override;
    bool ParseLineSection(const std::vector<std::string>& args,
                          const std::string& filename, int line,
                          std::string* err) const override {
        return true;
    }
    void EndSection() override {
    }
    void EndFile(const std::string& filename) override;
private:
    std::vector<std::string> imports_;
};

#endif
