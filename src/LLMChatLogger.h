#ifndef MOD_LLM_CHAT_LOGGER_H
#define MOD_LLM_CHAT_LOGGER_H

#include "Config.h"
#include "Define.h"
#include <string>
#include <fmt/format.h>
#include "mod_llm_chat_config.h"

class LLMChatLogger {
public:
    static void Log(int32 level, std::string const& message);
    
    template<typename... Args>
    static void LogFormat(int32 level, std::string_view format, Args&&... args) {
        if (LLM_Config.LogLevel >= level) {
            Log(level, fmt::format(format, std::forward<Args>(args)...));
        }
    }

    static void LogChat(std::string const& playerName, std::string const& input, std::string const& response);
    static void LogError(std::string const& message);
    
    template<typename... Args>
    static void LogErrorFormat(std::string_view format, Args&&... args) {
        if (LLM_Config.LogLevel > 0) {
            LogError(fmt::format(format, std::forward<Args>(args)...));
        }
    }

    static void LogDebug(std::string const& message);
    
    template<typename... Args>
    static void LogDebugFormat(std::string_view format, Args&&... args) {
        if (LLM_Config.LogLevel >= 3) {
            LogDebug(fmt::format(format, std::forward<Args>(args)...));
        }
    }
};

#endif // MOD_LLM_CHAT_LOGGER_H 