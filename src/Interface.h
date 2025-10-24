#pragma once
#include "Settings.h"
#include <atomic>
#include <deque>
#include <future>
#include <vector>

class Interface {
public:
    int Initialize();
    int Render(std::atomic<bool>* runningFlag);
    static void Minimize();
    static void Show();
    
    std::future<bool> prompt_user_async(std::string context);
    std::future<std::string> prompt_user_async_string(std::string context);
    
private:
    bool render_prompt();
    enum class PromptType { boolean, string };
    struct prompt {
        PromptType type;
        std::string context;
        std::promise<bool> promise_bool;
        std::promise<std::string> promise_str;
        std::vector<char> input_buf;
        std::atomic<bool> ans{false}; // ensure future is answered once

        // explicit prompt(std::string c) : context(std::move(c)) {}
        explicit prompt(PromptType t, std::string c, size_t buf_size = 256)
            : type(t), context(std::move(c)), input_buf(buf_size)
        {
            // start with empty C-string
            if (!input_buf.empty()) input_buf[0] = '\0';
        }
        prompt(const prompt&) = delete;
        // prompt(prompt&&) = default;
        prompt(prompt&&) = delete;
    };
    std::deque<std::unique_ptr<prompt>> prompt_queue_;
    std::mutex prompt_mutex_;
    std::unique_ptr<prompt> active_prompt_;

    Settings& cfg = Settings::GetInstance();
    std::string ReadFileToString(); // requires cfg object
};