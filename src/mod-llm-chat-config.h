#ifndef _MOD_LLM_CHAT_CONFIG_H_
#define _MOD_LLM_CHAT_CONFIG_H_

#include <string>
#include "Define.h"

struct LLMConfig {
    bool Enabled{true};
    uint32 LogLevel{1};
    bool Debug{false};
    bool Announce{true};

    std::string Endpoint;
    std::string Model;
    std::string ApiKey;
    std::string ApiSecret;
    float ChatRange{30.0f};
    std::string ResponsePrefix;
    uint32 MaxResponsesPerMessage{3};
    uint32 ResponseChance{100};

    struct {
        uint32 MessagesPerSecond{5};
        uint32 WindowSize{10000};
        uint32 MaxMessages{5};
    } RateLimit;

    struct {
        struct {
            uint32 MaxThreads{2};
            uint32 MaxApiCalls{5};
            uint32 ApiTimeout{3};
        } Threading;

        struct {
            uint32 WindowSize{10000};
            uint32 MaxMessages{5};
        } GlobalRateLimit;

        struct {
            uint32 Player{10000};
            uint32 Bot{15000};
            uint32 Global{5000};
        } Cooldowns;

        struct {
            uint32 Min{5};
            uint32 Max{200};
        } MessageLimits;

        struct {
            uint32 Min{2000};
            uint32 Max{1500};
            uint32 Pacified{5000};
        } Delays;
    } Performance;

    struct {
        uint32 Size{25};
        uint32 Timeout{180};
    } Queue;

    struct {
        float Temperature{0.85f};
        float TopP{0.9f};
        uint32 NumPredict{2048};
        uint32 ContextSize{4096};
        float RepeatPenalty{1.2f};
        uint32 MaxQueueSize{100};
    } LLM;

    struct {
        bool Enable{true};
        uint32 MaxInteractionsPerPair{10};
        uint32 ExpirationTime{3600};
        uint32 MaxContextLength{2000};
    } Memory;

    std::string Host;
    std::string Port;
    std::string Target;

    struct {
        std::string CharacterDB{"acore_characters"};
        std::string WorldDB{"acore_world"};
        std::string AuthDB{"acore_auth"};
        std::string CustomDB{"acore_llm_chat"};
    } Database;

    std::string PersonalityFile{"modules/mod_llm_chat/conf/personalities.json"};
};

extern LLMConfig LLM_Config;

#endif // _MOD_LLM_CHAT_CONFIG_H_ 