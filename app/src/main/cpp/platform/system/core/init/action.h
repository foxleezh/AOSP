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

#ifndef _INIT_ACTION_H
#define _INIT_ACTION_H

#include <map>
#include <queue>
#include <string>
#include <vector>

#include "builtins.h"
#include "init_parser.h"
#include "keyword_map.h"

class Command {
public:
    Command(BuiltinFunction f, const std::vector<std::string>& args,
            const std::string& filename, int line);

    int InvokeFunc() const;
    std::string BuildCommandString() const;
    std::string BuildSourceString() const;

private:
    BuiltinFunction func_;
    std::vector<std::string> args_;
    std::string filename_;
    int line_;
};

class Action {
public:
    explicit Action(bool oneshot = false);

    bool AddCommand(const std::vector<std::string>& args,
                    const std::string& filename, int line, std::string* err);
    void AddCommand(BuiltinFunction f,
                    const std::vector<std::string>& args,
                    const std::string& filename = "", int line = 0);
    void CombineAction(const Action& action);
    bool InitTriggers(const std::vector<std::string>& args, std::string* err);
    bool InitSingleTrigger(const std::string& trigger);
    std::size_t NumCommands() const;
    void ExecuteOneCommand(std::size_t command) const;
    void ExecuteAllCommands() const;
    bool CheckEventTrigger(const std::string& trigger) const;
    bool CheckPropertyTrigger(const std::string& name,
                              const std::string& value) const;
    bool TriggersEqual(const Action& other) const;
    std::string BuildTriggersString() const;
    void DumpState() const;

    bool oneshot() const { return oneshot_; }
    static void set_function_map(const KeywordMap<BuiltinFunction>* function_map) {
        function_map_ = function_map;
    }


private:
    void ExecuteCommand(const Command& command) const;
    bool CheckPropertyTriggers(const std::string& name = "",
                               const std::string& value = "") const;
    bool ParsePropertyTrigger(const std::string& trigger, std::string* err);

    std::map<std::string, std::string> property_triggers_;
    std::string event_trigger_;
    std::vector<Command> commands_;
    bool oneshot_;
    static const KeywordMap<BuiltinFunction>* function_map_;
};

class Trigger {
public:
    virtual ~Trigger() { }
    virtual bool CheckTriggers(const Action& action) const = 0;
};

class ActionManager {
public:
    static ActionManager& GetInstance();

    void AddAction(std::unique_ptr<Action> action);
    void QueueEventTrigger(const std::string& trigger);
    void QueuePropertyTrigger(const std::string& name, const std::string& value);
    void QueueAllPropertyTriggers();
    void QueueBuiltinAction(BuiltinFunction func, const std::string& name);
    void ExecuteOneCommand();
    bool HasMoreCommands() const;
    void DumpState() const;

private:
    ActionManager();

    ActionManager(ActionManager const&) = delete;
    void operator=(ActionManager const&) = delete;

    std::vector<std::unique_ptr<Action>> actions_;
    std::queue<std::unique_ptr<Trigger>> trigger_queue_;
    std::queue<const Action*> current_executing_actions_;
    std::size_t current_command_;
};

class ActionParser : public SectionParser {
public:
    ActionParser() : action_(nullptr) {
    }
    bool ParseSection(const std::vector<std::string>& args,
                      std::string* err) override;
    bool ParseLineSection(const std::vector<std::string>& args,
                          const std::string& filename, int line,
                          std::string* err) const override;
    void EndSection() override;
    void EndFile(const std::string&) override {
    }
private:
    std::unique_ptr<Action> action_;
};

#endif
