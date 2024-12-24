#ifndef _MOD_LLM_CHAT_CONFIG_H_
#define _MOD_LLM_CHAT_CONFIG_H_

#include "Define.h"
#include <string>

struct LLMConfig {
    // Core Settings
    bool Enabled;
    uint32 LogLevel;
    bool Debug;
    bool Announce;

    // Provider Settings
    std::string Endpoint;
    std::string Model;
    std::string ApiKey;
    std::string ApiSecret;

    // Chat Behavior
    float ChatRange;
    std::string ResponsePrefix;
    uint32 MaxResponsesPerMessage;
    uint32 ResponseChance;

    // Performance & Rate Limiting
    struct {
        struct {
            uint32 WindowSize;
            uint32 MaxMessages;
        } GlobalRateLimit;

        struct {
            uint32 Player;
            uint32 Bot;
            uint32 Global;
        } Cooldowns;

        struct {
            uint32 MaxThreads;
            uint32 MaxApiCalls;
            uint32 ApiTimeout;
        } Threading;

        struct {
            uint32 Min;
            uint32 Max;
        } MessageLimits;

        struct {
            uint32 Min;
            uint32 Max;
            uint32 Pacified;
        } Delays;
    } Performance;

    // Queue Settings
    struct {
        uint32 Size;
        uint32 Timeout;
    } Queue;

    // LLM Parameters
    struct {
        float Temperature;
        float TopP;
        uint32 NumPredict;
        uint32 ContextSize;
        float RepeatPenalty;
        uint32 MaxQueueSize;
    } LLM;

    // Memory System
    struct {
        bool Enable;
        uint32 MaxInteractionsPerPair;
        uint32 ExpirationTime;
        uint32 MaxContextLength;
    } Memory;

    // Personality System
    std::string PersonalityFile;

    // URL components (parsed from endpoint)
    std::string Host;
    std::string Port;
    std::string Target;

    // Database Settings
    struct {
        std::string CharacterDB;
        std::string WorldDB;
        std::string AuthDB;
        std::string CustomDB;
    } Database;
};

extern LLMConfig LLM_Config;

#endif // _MOD_LLM_CHAT_CONFIG_H_ 