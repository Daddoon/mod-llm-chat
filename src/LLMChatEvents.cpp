#include "LLMChatEvents.h"
#include "LLMChatLogger.h"
#include "Chat.h"
#include "Channel.h"
#include "Guild.h"
#include "Group.h"
#include "ChannelMgr.h"

RemovePacifiedEvent::RemovePacifiedEvent(Player* p) : player(p) {}

bool RemovePacifiedEvent::Execute(uint64 /*time*/, uint32 /*diff*/)
{
    if (player && player->IsInWorld())
    {
        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
    }
    return true;
}

BotResponseEvent::BotResponseEvent(Player* r, Player* s, std::string resp, uint32 t, std::string m, TeamId tm, uint32 pd)
    : responder(r), originalSender(s), response(resp), chatType(t), message(m), team(tm), pacifiedDuration(pd) {}

bool BotResponseEvent::Execute(uint64 /*time*/, uint32 /*diff*/)
{
    if (!responder || !responder->IsInWorld())
        return true;

    // Double check we're not responding as the original sender
    if (originalSender && 
        (responder == originalSender || 
         (responder->GetSession() && originalSender->GetSession() && 
          responder->GetSession()->GetAccountId() == originalSender->GetSession()->GetAccountId())))
    {
        LLMChatLogger::LogError("Prevented response from original sender's account");
        return true;
    }

    std::string logMsg = "Executing response from " + responder->GetName() + ": " + response;
    LLMChatLogger::Log(2, logMsg);

    // Check if the bot has a session
    if (WorldSession* session = responder->GetSession())
    {
        if (session->IsBot())
        {
            // Stop any current movement
            responder->StopMoving();
            responder->ClearInCombat();
            
            // Clear any current actions
            responder->InterruptNonMeleeSpells(false);
            responder->RemoveAurasByType(SPELL_AURA_MOUNTED);
            
            // Remove food/drink auras
            responder->RemoveAura(433);  // Food
            responder->RemoveAura(430);  // Drink
        }
    }

    switch (chatType)
    {
        case CHAT_MSG_SAY:
            responder->Say(response, LANG_UNIVERSAL);
            break;
            
        case CHAT_MSG_YELL:
            responder->Yell(response, LANG_UNIVERSAL);
            break;
            
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            if (Group* group = responder->GetGroup())
            {
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, static_cast<ChatMsg>(chatType), 
                    LANG_UNIVERSAL, responder, nullptr, response);
                group->BroadcastPacket(&data, false);
            }
            break;
            
        case CHAT_MSG_GUILD:
            if (Guild* guild = responder->GetGuild())
            {
                guild->BroadcastToGuild(responder->GetSession(), false, 
                    response, LANG_UNIVERSAL);
            }
            break;
            
        case CHAT_MSG_CHANNEL:
            if (ChannelMgr* cMgr = ChannelMgr::forTeam(team))
            {
                size_t spacePos = message.find(' ');
                if (spacePos != std::string::npos)
                {
                    std::string channelName = message.substr(0, spacePos);
                    if (Channel* channel = cMgr->GetChannel(channelName, responder))
                    {
                        channel->Say(responder->GetGUID(), response, LANG_UNIVERSAL);
                    }
                }
            }
            break;
    }

    // Add a small delay before the bot can act again
    if (WorldSession* session = responder->GetSession())
    {
        if (session->IsBot())
        {
            responder->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
            responder->m_Events.AddEvent(new RemovePacifiedEvent(responder), 
                responder->m_Events.CalculateTime(pacifiedDuration));
        }
    }

    LLMChatLogger::Log(2, "Successfully delivered response from " + responder->GetName());
    return true;
}

TriggerResponseEvent::TriggerResponseEvent(Player* p, std::string msg, uint32 type)
    : player(p), message(msg), chatType(type), team(p->GetTeamId()) {}

bool TriggerResponseEvent::Execute(uint64 /*time*/, uint32 /*diff*/)
{
    if (!player || !player->IsInWorld())
        return true;

    SendAIResponse(player, message, chatType, team);
    return true;
} 