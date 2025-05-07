#ifndef MOD_LLM_CHAT_EVENTS_H
#define MOD_LLM_CHAT_EVENTS_H

#include "Common.h"
#include "Define.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "mod-llm-chat.h"
#include "Playerbots.h"

// Forward declarations
class LLMChatHandler;

// Event classes for response timing
class RemovePacifiedEvent : public BasicEvent
{
    Player* player;

public:
    explicit RemovePacifiedEvent(Player* p);
    bool Execute(uint64 time, uint32 diff) override;
};

class BotResponseEvent : public BasicEvent
{
public:
    BotResponseEvent(Player* r, Player* s, std::string resp, uint32 t)
        : responder(r), originalSender(s), response(resp), chatType(t) {}

    bool Execute(uint64 time, uint32 diff) override;

private:
    Player* responder;
    Player* originalSender;
    std::string response;
    uint32 chatType;
};

class LLMChatEvents : public PlayerScript
{
public:
    LLMChatEvents() noexcept;

    void OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg) override;
    void OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override;
    void OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* /*group*/) override;
    void OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* /*guild*/) override;
    void OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override;

    static void SendResponse(Player* responder, Player* sender, std::string const& response, uint32 chatType);
    static bool ShouldProcessMessage(Player* player, uint32 type, const std::string& msg);
    static bool IsValidChatType(uint32 type);
    static std::string GetChatTypeName(uint32 type);
    static std::vector<Player*> GetPotentialResponders(Player* sender, uint32 type);

private:
    static LLMChatHandler* s_handler;
};

#endif // MOD_LLM_CHAT_EVENTS_H 
