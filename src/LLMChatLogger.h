#ifndef _MOD_LLM_CHAT_LOGGER_H_
#define _MOD_LLM_CHAT_LOGGER_H_

#include <string>
#include "mod_llm_chat_config.h"

class LLMChatLogger {
public:
    static void Log(uint32 level, std::string const& message);
    static void LogChat(std::string const& playerName, std::string const& input, std::string const& response);
    static void LogError(std::string const& message);
    static void LogDebug(std::string const& message);
};

#endif // _MOD_LLM_CHAT_LOGGER_H_ 