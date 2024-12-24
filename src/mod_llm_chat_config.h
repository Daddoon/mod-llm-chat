#ifndef MOD_LLM_CHAT_CONFIG_H
#define MOD_LLM_CHAT_CONFIG_H

#include <string>
#include <cstdint>

struct LLMConfig
{
    // Core Settings
    bool Enabled;
    int32_t LogLevel;
    bool Announce;

    // Provider Settings
    std::string Endpoint;
    std::string Model;
    std::string ApiKey;
    std::string ApiSecret;
    std::string ApiVersion;
    bool UseOllama;
    std::string OllamaEndpoint;

    // Chat Behavior
    float ChatRange;
    std::string ResponsePrefix;
    uint32_t MaxResponsesPerMessage;
    uint32_t ResponseChance;

    // Performance & Rate Limiting
    struct {
        struct {
            uint32_t WindowSize;
            uint32_t MaxMessages;
            uint32_t MaxTokensPerMinute;
        } GlobalRateLimit;

        struct {
            uint32_t Player;
            uint32_t Bot;
            uint32_t Global;
            uint32_t ApiCall;
        } Cooldowns;

        struct {
            uint32_t MaxThreads;
            uint32_t MaxApiCalls;
            uint32_t ApiTimeout;
            uint32_t QueueProcessInterval;
        } Threading;

        struct {
            uint32_t Min;
            uint32_t Max;
        } MessageLimits;

        struct {
            uint32_t Min;
            uint32_t Max;
            uint32_t Pacified;
            uint32_t QueueRetry;
        } Delays;

        struct {
            uint32_t MaxQueueSize;
            uint32_t MaxCacheSize;
            uint32_t CleanupInterval;
        } Memory;
    } Performance;

    // Queue Settings
    struct {
        uint32_t Size;
        uint32_t Timeout;
        uint32_t RetryAttempts;
        uint32_t MaxPendingPerPlayer;
    } Queue;

    // LLM Parameters
    struct {
        float Temperature;
        float TopP;
        uint32_t NumPredict;
        uint32_t ContextSize;
        float RepeatPenalty;
        uint32_t MaxTokens;
        std::string StopSequence;
    } LLM;

    // Memory System
    struct {
        bool Enable;
        uint32_t MaxInteractionsPerPair;
        uint32_t ExpirationTime;
        uint32_t MaxContextLength;
        bool PersistToDisk;
        std::string StoragePath;
        uint32_t SaveInterval;
    } Memory;

    // Personality System
    std::string PersonalityFile;
    bool EnableDynamicPersonality;
    uint32_t PersonalityUpdateInterval;

    // URL components (parsed from endpoint)
    std::string Host;
    std::string Port;
    std::string Target;
};

extern LLMConfig LLM_Config;

#endif // MOD_LLM_CHAT_CONFIG_H 