#ifndef MOD_LLM_CHAT_CONFIG_H
#define MOD_LLM_CHAT_CONFIG_H

#include "Define.h"
#include <string>
#include <cstdint>

struct LLMConfig
{
    struct Chat
    {
        bool Enable = true;
        bool Announce = true;
        bool LogBotDetection = true;  // Whether to log detailed bot detection info
        uint32_t ResponseCooldown = 1;   // Cooldown between responses in seconds
        float ChatRange = 30.0f;       // Range for proximity chat (SAY)
        uint32_t MinMessageLength = 2;  // Minimum message length to process
    };

    struct API
    {
        std::string Endpoint = "http://localhost:11434/api/chat";
        std::string Model = "mistral";
        std::string APIKey = "";
        uint32_t MaxTokens = 100;
        float Temperature = 0.7f;
    };

    struct Database
    {
        std::string CustomDB = "custom";
        std::string CharacterDB = "characters";
    };

    struct Logging
    {
        uint32_t LogLevel = 3;  // Set to maximum debug level
        bool LogToConsole = true;
        bool LogToFile = false;
        std::string LogFile = "llm_chat.log";
    };

    Chat Chat;
    API API;
    Database Database;
    Logging Logging;
    bool Enable = true;
};

// Global configuration instance
extern LLMConfig LLM_Config;

#endif // MOD_LLM_CHAT_CONFIG_H 