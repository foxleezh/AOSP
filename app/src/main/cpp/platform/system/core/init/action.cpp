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

#include "action.h"

#include <errno.h>

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "builtins.h"
#include "error.h"
#include "init_parser.h"
#include "log.h"
#include "util.h"

using android::base::Join;
using android::base::StringPrintf;

Command::Command(BuiltinFunction f, const std::vector<std::string>& args,
                 const std::string& filename, int line)
    : func_(f), args_(args), filename_(filename), line_(line) {
}

int Command::InvokeFunc() const {
    std::vector<std::string> expanded_args;
    expanded_args.resize(args_.size());
    expanded_args[0] = args_[0];
    for (std::size_t i = 1; i < args_.size(); ++i) {
        if (!expand_props(args_[i], &expanded_args[i])) {
            LOG(ERROR) << args_[0] << ": cannot expand '" << args_[i] << "'";
            return -EINVAL;
        }
    }

    return func_(expanded_args);
}

std::string Command::BuildCommandString() const {
    return Join(args_, ' ');
}

std::string Command::BuildSourceString() const {
    if (!filename_.empty()) {
        return StringPrintf(" (%s:%d)", filename_.c_str(), line_);
    } else {
        return std::string();
    }
}

Action::Action(bool oneshot) : oneshot_(oneshot) {
}

const KeywordMap<BuiltinFunction>* Action::function_map_ = nullptr;

bool Action::AddCommand(const std::vector<std::string>& args,
                        const std::string& filename, int line, std::string* err) {
    if (!function_map_) {
        *err = "no function map available";
        return false;
    }

    if (args.empty()) {
        *err = "command needed, but not provided";
        return false;
    }

    auto function = function_map_->FindFunction(args[0], args.size() - 1, err);
    if (!function) {
        return false;
    }

    AddCommand(function, args, filename, line);
    return true;
}

void Action::AddCommand(BuiltinFunction f,
                        const std::vector<std::string>& args,
                        const std::string& filename, int line) {
    commands_.emplace_back(f, args, filename, line);
}

void Action::CombineAction(const Action& action) {
    for (const auto& c : action.commands_) {
        commands_.emplace_back(c);
    }
}

std::size_t Action::NumCommands() const {
    return commands_.size();
}

void Action::ExecuteOneCommand(std::size_t command) const {
    // We need a copy here since some Command execution may result in
    // changing commands_ vector by importing .rc files through parser
    Command cmd = commands_[command];
    ExecuteCommand(cmd);
}

void Action::ExecuteAllCommands() const {
    for (const auto& c : commands_) {
        ExecuteCommand(c);
    }
}

void Action::ExecuteCommand(const Command& command) const {
    Timer t;
    int result = command.InvokeFunc();

    double duration_ms = t.duration_s() * 1000;
    // Any action longer than 50ms will be warned to user as slow operation
    if (duration_ms > 50.0 ||
        android::base::GetMinimumLogSeverity() <= android::base::DEBUG) {
        std::string trigger_name = BuildTriggersString();
        std::string cmd_str = command.BuildCommandString();
        std::string source = command.BuildSourceString();

        LOG(INFO) << "Command '" << cmd_str << "' action=" << trigger_name << source
                  << " returned " << result << " took " << duration_ms << "ms.";
    }
}

bool Action::ParsePropertyTrigger(const std::string& trigger, std::string* err) {
    const static std::string prop_str("property:");
    std::string prop_name(trigger.substr(prop_str.length()));
    size_t equal_pos = prop_name.find('=');
    if (equal_pos == std::string::npos) {
        *err = "property trigger found without matching '='";
        return false;
    }

    std::string prop_value(prop_name.substr(equal_pos + 1));
    prop_name.erase(equal_pos);

    if (auto [it, inserted] = property_triggers_.emplace(prop_name, prop_value); !inserted) {
        *err = "multiple property triggers found for same property";
        return false;
    }
    return true;
}

bool Action::InitTriggers(const std::vector<std::string>& args, std::string* err) {
    const static std::string prop_str("property:");
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].empty()) {
            *err = "empty trigger is not valid";
            return false;
        }

        if (i % 2) {
            if (args[i] != "&&") {
                *err = "&& is the only symbol allowed to concatenate actions";
                return false;
            } else {
                continue;
            }
        }

        if (!args[i].compare(0, prop_str.length(), prop_str)) {
            if (!ParsePropertyTrigger(args[i], err)) {
                return false;
            }
        } else {
            if (!event_trigger_.empty()) {
                *err = "multiple event triggers are not allowed";
                return false;
            }

            event_trigger_ = args[i];
        }
    }

    return true;
}

bool Action::InitSingleTrigger(const std::string& trigger) {
    std::vector<std::string> name_vector{trigger};
    std::string err;
    bool ret = InitTriggers(name_vector, &err);
    if (!ret) {
        LOG(ERROR) << "InitSingleTrigger failed due to: " << err;
    }
    return ret;
}

// This function checks that all property triggers are satisfied, that is
// for each (name, value) in property_triggers_, check that the current
// value of the property 'name' == value.
//
// It takes an optional (name, value) pair, which if provided must
// be present in property_triggers_; it skips the check of the current
// property value for this pair.
bool Action::CheckPropertyTriggers(const std::string& name,
                                   const std::string& value) const {
    if (property_triggers_.empty()) {
        return true;
    }

    bool found = name.empty();
    for (const auto& [trigger_name, trigger_value] : property_triggers_) {
        if (trigger_name == name) {
            if (trigger_value != "*" && trigger_value != value) {
                return false;
            } else {
                found = true;
            }
        } else {
            std::string prop_val = android::base::GetProperty(trigger_name, "");
            if (prop_val.empty() || (trigger_value != "*" && trigger_value != prop_val)) {
                return false;
            }
        }
    }
    return found;
}

bool Action::CheckEventTrigger(const std::string& trigger) const {
    return !event_trigger_.empty() &&
        trigger == event_trigger_ &&
        CheckPropertyTriggers();
}

bool Action::CheckPropertyTrigger(const std::string& name,
                                  const std::string& value) const {
    return event_trigger_.empty() && CheckPropertyTriggers(name, value);
}

bool Action::TriggersEqual(const Action& other) const {
    return property_triggers_ == other.property_triggers_ &&
        event_trigger_ == other.event_trigger_;
}

std::string Action::BuildTriggersString() const {
    std::vector<std::string> triggers;

    for (const auto& [trigger_name, trigger_value] : property_triggers_) {
        triggers.emplace_back(trigger_name + '=' + trigger_value);
    }
    if (!event_trigger_.empty()) {
        triggers.emplace_back(event_trigger_);
    }

    return Join(triggers, " && ");
}

void Action::DumpState() const {
    std::string trigger_name = BuildTriggersString();
    LOG(INFO) << "on " << trigger_name;

    for (const auto& c : commands_) {
        std::string cmd_str = c.BuildCommandString();
        LOG(INFO) << "  " << cmd_str;
    }
}

class EventTrigger : public Trigger {
public:
    explicit EventTrigger(const std::string& trigger) : trigger_(trigger) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action.CheckEventTrigger(trigger_);
    }
private:
    const std::string trigger_;
};

class PropertyTrigger : public Trigger {
public:
    PropertyTrigger(const std::string& name, const std::string& value)
        : name_(name), value_(value) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action.CheckPropertyTrigger(name_, value_);
    }
private:
    const std::string name_;
    const std::string value_;
};

class BuiltinTrigger : public Trigger {
public:
    explicit BuiltinTrigger(Action* action) : action_(action) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action_ == &action;
    }
private:
    const Action* action_;
};

ActionManager::ActionManager() : current_command_(0) {
}

ActionManager& ActionManager::GetInstance() {
    static ActionManager instance;
    return instance;
}

void ActionManager::AddAction(std::unique_ptr<Action> action) {
    auto old_action_it =
        std::find_if(actions_.begin(), actions_.end(),
                     [&action] (std::unique_ptr<Action>& a) {
                         return action->TriggersEqual(*a);
                     });

    if (old_action_it != actions_.end()) {
        (*old_action_it)->CombineAction(*action);
    } else {
        actions_.emplace_back(std::move(action));
    }
}

void ActionManager::QueueEventTrigger(const std::string& trigger) {
    trigger_queue_.push(std::make_unique<EventTrigger>(trigger));
}

void ActionManager::QueuePropertyTrigger(const std::string& name,
                                         const std::string& value) {
    trigger_queue_.push(std::make_unique<PropertyTrigger>(name, value));
}

void ActionManager::QueueAllPropertyTriggers() {
    QueuePropertyTrigger("", "");
}

void ActionManager::QueueBuiltinAction(BuiltinFunction func,
                                       const std::string& name) {
    auto action = std::make_unique<Action>(true);
    std::vector<std::string> name_vector{name};

    if (!action->InitSingleTrigger(name)) {
        return;
    }

    action->AddCommand(func, name_vector);

    trigger_queue_.push(std::make_unique<BuiltinTrigger>(action.get()));
    actions_.emplace_back(std::move(action));
}

void ActionManager::ExecuteOneCommand() {
    // Loop through the trigger queue until we have an action to execute
    while (current_executing_actions_.empty() && !trigger_queue_.empty()) {
        for (const auto& action : actions_) {
            if (trigger_queue_.front()->CheckTriggers(*action)) {
                current_executing_actions_.emplace(action.get());
            }
        }
        trigger_queue_.pop();
    }

    if (current_executing_actions_.empty()) {
        return;
    }

    auto action = current_executing_actions_.front();

    if (current_command_ == 0) {
        std::string trigger_name = action->BuildTriggersString();
        LOG(INFO) << "processing action (" << trigger_name << ")";
    }

    action->ExecuteOneCommand(current_command_);

    // If this was the last command in the current action, then remove
    // the action from the executing list.
    // If this action was oneshot, then also remove it from actions_.
    ++current_command_;
    if (current_command_ == action->NumCommands()) {
        current_executing_actions_.pop();
        current_command_ = 0;
        if (action->oneshot()) {
            auto eraser = [&action] (std::unique_ptr<Action>& a) {
                return a.get() == action;
            };
            actions_.erase(std::remove_if(actions_.begin(), actions_.end(), eraser));
        }
    }
}

bool ActionManager::HasMoreCommands() const {
    return !current_executing_actions_.empty() || !trigger_queue_.empty();
}

void ActionManager::DumpState() const {
    for (const auto& a : actions_) {
        a->DumpState();
    }
}

bool ActionParser::ParseSection(const std::vector<std::string>& args,
                                std::string* err) {
    std::vector<std::string> triggers(args.begin() + 1, args.end());
    if (triggers.size() < 1) {
        *err = "actions must have a trigger";
        return false;
    }

    auto action = std::make_unique<Action>(false);
    if (!action->InitTriggers(triggers, err)) {
        return false;
    }

    action_ = std::move(action);
    return true;
}

bool ActionParser::ParseLineSection(const std::vector<std::string>& args,
                                    const std::string& filename, int line,
                                    std::string* err) const {
    return action_ ? action_->AddCommand(args, filename, line, err) : false;
}

void ActionParser::EndSection() {
    if (action_ && action_->NumCommands() > 0) {
        ActionManager::GetInstance().AddAction(std::move(action_));
    }
}
