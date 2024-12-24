#include "LLMChatLogger.h"
#include "Log.h"

void LLMChatLogger::Log(uint32 level, std::string const& message) {
    if (LLM_Config.LogLevel >= level) {
        switch (level) {
            case 1:
                LOG_FATAL("module.llm_chat", "{}", message);
                break;
            case 2:
                LOG_ERROR("module.llm_chat", "{}", message);
                break;
            case 3:
                LOG_WARN("module.llm_chat", "{}", message);
                break;
            case 4:
                LOG_INFO("module.llm_chat", "{}", message);
                break;
            case 5:
                LOG_DEBUG("module.llm_chat", "{}", message);
                break;
            case 6:
                LOG_TRACE("module.llm_chat", "{}", message);
                break;
            default:
                LOG_INFO("module.llm_chat", "{}", message);
                break;
        }
    }
}

void LLMChatLogger::LogChat(std::string const& playerName, std::string const& input, std::string const& response) {
    // Skip logging if disabled (level 0)
    if (LLM_Config.LogLevel == 0) {
        return;
    }
    
    // Only log chat at detailed level or higher
    if (LLM_Config.LogLevel >= 2) {
        std::string inputMsg = "Player " + playerName + " says: " + input;
        std::string responseMsg = "AI Response: " + response;
        LOG_INFO("module.llm_chat", "{}", inputMsg);
        LOG_INFO("module.llm_chat", "{}", responseMsg);
    }
}

void LLMChatLogger::LogError(std::string const& message) {
    // Always log errors unless logging is completely disabled
    if (LLM_Config.LogLevel > 0) {
        LOG_ERROR("module.llm_chat", "{}", message);
    }
}

void LLMChatLogger::LogDebug(std::string const& message) {
    // Only log debug messages at highest level
    if (LLM_Config.LogLevel >= 3) {
        LOG_DEBUG("module.llm_chat", "{}", message);
    }
} 