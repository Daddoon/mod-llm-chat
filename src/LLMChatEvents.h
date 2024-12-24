#ifndef MOD_LLM_CHAT_EVENTS_H
#define MOD_LLM_CHAT_EVENTS_H

#include "Player.h"
#include "World.h"
#include "Channel.h"
#include "Guild.h"

// Forward declarations
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team);

class RemovePacifiedEvent : public BasicEvent
{
    Player* player;

public:
    RemovePacifiedEvent(Player* p);
    bool Execute(uint64 time, uint32 diff) override;
};

class BotResponseEvent : public BasicEvent
{
    Player* responder;
    Player* originalSender;
    std::string response;
    uint32 chatType;
    std::string message;
    TeamId team;
    uint32 pacifiedDuration;

public:
    BotResponseEvent(Player* r, Player* s, std::string resp, uint32 t, std::string m, TeamId tm, uint32 pd);
    bool Execute(uint64 time, uint32 diff) override;
};

class TriggerResponseEvent : public BasicEvent
{
    Player* player;
    std::string message;
    uint32 chatType;
    TeamId team;

public:
    TriggerResponseEvent(Player* p, std::string msg, uint32 type);
    bool Execute(uint64 time, uint32 diff) override;
};

#endif // MOD_LLM_CHAT_EVENTS_H 