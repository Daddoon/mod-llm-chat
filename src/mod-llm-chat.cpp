/*
** Made by Krazor
** AzerothCore 2019 http://www.azerothcore.org/
** Based on LLM Chat integration
*/

#include "mod-llm-chat-config.h"
#include "LLMChatQueue.h"
#include "LLMChatEvents.h"
#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "World.h"

// Define the global configuration instance
LLMConfig LLM_Config;

class LLMChat : public WorldScript
{
public:
    LLMChat() : WorldScript("LLMChat") {}

    void OnStartup() override
    {
        LOG_INFO("module", "[LLMChat] Starting module...");
        
        if (!LLM_Config.Enable)
        {
            LOG_INFO("module", "[LLMChat] Module is disabled in config");
            return;
        }

        // Initialize the chat queue
        LLMChatQueue::Initialize();

        if (LLM_Config.Chat.Announce)
        {
            LOG_INFO("module", "[LLMChat] Module started successfully");
            sWorld->SendServerMessage(SERVER_MSG_STRING, "LLM Chat module loaded");
        }
    }

    void OnUpdate([[maybe_unused]] uint32 /*diff*/) override
    {
        // No need for timer-based queue processing anymore
        // Queue is processed by worker thread
    }
};

// Create a single instance of LLMChatEvents
LLMChatEvents* chatEvents = nullptr;

class LLMChatPlayerScript : public PlayerScript
{
public:
    LLMChatPlayerScript() : PlayerScript("LLMChatPlayerScript") 
    {
        if (!chatEvents)
            chatEvents = new LLMChatEvents();
    }

    ~LLMChatPlayerScript()
    {
        delete chatEvents;
        chatEvents = nullptr;
    }

    void OnChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
    {
        if (!LLM_Config.Enable || !chatEvents)
            return;

        chatEvents->OnChat(player, type, lang, msg);
    }
};

// Add all scripts
void Addmod_llm_chatScripts()
{
    new LLMChat();
    new LLMChatPlayerScript();
} 