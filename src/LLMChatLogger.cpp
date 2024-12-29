#include "LLMChatLogger.h"
#include "Log.h"
#include "mod-llm-chat-config.h"

void LLMChatLogger::Log(uint32_t level, std::string const& message) {
    if (!LLM_Config.Enable)
        return;

    if (LLM_Config.Logging.LogLevel >= level)
    {
        if (LLM_Config.Logging.LogToConsole)
            LOG_INFO("module", "[LLMChat] {}", message);

        if (LLM_Config.Logging.LogToFile)
        {
            // TODO: Implement file logging
        }
    }
}

void LLMChatLogger::LogChat(std::string const& playerName, std::string const& input, std::string const& response) {
    if (!LLM_Config.Enable)
        return;
    
    // Only log chat at detailed level or higher
    if (LLM_Config.Logging.LogLevel >= 2)
    {
        if (LLM_Config.Logging.LogToConsole)
        {
            std::string inputMsg = "Player " + playerName + " says: " + input;
            std::string responseMsg = "AI Response: " + response;
            LOG_INFO("module", "[LLMChat] {}", inputMsg);
            LOG_INFO("module", "[LLMChat] {}", responseMsg);
        }

        if (LLM_Config.Logging.LogToFile)
        {
            // TODO: Implement file logging
        }
    }
}

void LLMChatLogger::LogError(std::string const& message) {
    if (!LLM_Config.Enable)
        return;

    LOG_ERROR("module", "[LLMChat] {}", message);

    if (LLM_Config.Logging.LogToFile)
    {
        // TODO: Implement file logging
    }
}

void LLMChatLogger::LogDebug(std::string const& message) {
    if (!LLM_Config.Enable)
        return;

    if (LLM_Config.Logging.LogLevel >= 2)
    {
        if (LLM_Config.Logging.LogToConsole)
            LOG_DEBUG("module", "[LLMChat] {}", message);

        if (LLM_Config.Logging.LogToFile)
        {
            // TODO: Implement file logging
        }
    }
} 