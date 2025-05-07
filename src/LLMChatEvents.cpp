#include "LLMChatEvents.h"
#include "LLMChatQueue.h"
#include "Chat.h"
#include "Channel.h"
#include "Guild.h"
#include "Group.h"
#include "ChannelMgr.h"
#include "Log.h"
#include "mod-llm-chat.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include <random>

LLMChatHandler* LLMChatEvents::s_handler = nullptr;

LLMChatEvents::LLMChatEvents() noexcept : PlayerScript("LLMChatEvents") 
{
    LOG_INFO("module", "[LLMChat] LLMChatEvents initialized");
}

void LLMChatEvents::OnPlayerChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg)
{
    if (!player || msg.empty())
        return;

    if (!LLM_Config.Enable)
    {
        LOG_DEBUG("module", "[LLMChat] Module is disabled, ignoring chat message from {}", player->GetName());
        return;
    }

    LOG_INFO("module", "[LLMChat] Processing SAY/YELL message from player: {}", player->GetName());
    
    // Check if there are any bots in the map at all
    Map* map = player->GetMap();
    if (map)
    {
        int botCount = 0;
        const Map::PlayerList& players = map->GetPlayers();
        for (const auto& itr : players)
        {
            Player* p = itr.GetSource();
            if (p)
            {
                PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(p);
                if (botAI && !botAI->IsRealPlayer())
                    botCount++;
            }
        }
        LOG_INFO("module", "[LLMChat] Found {} total bots in the map", botCount);
    }
    
    // Store original message since we'll be processing it
    std::string originalMsg = msg;
    
    // Get potential responders
    auto responders = GetPotentialResponders(player, CHAT_MSG_SAY);

    if (!responders.empty())
    {
        LOG_INFO("module", "[LLMChat] Found {} potential responders", responders.size());
        
        // Process message for each responder
        for (auto* responder : responders)
        {
            PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(responder);
            if (botAI && !botAI->IsRealPlayer())
            {
                LOG_INFO("module", "[LLMChat] Queueing response for bot: {}", responder->GetName());
                LLMChatQueue::EnqueueResponse(responder, originalMsg, "SAY");
            }
        }
    }
    else
    {
        LOG_DEBUG("module", "[LLMChat] No potential responders found");
    }

    // Don't clear the message - let it display normally
    return;
}

void LLMChatEvents::OnPlayerChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver)
{
    LOG_INFO("module", "[LLMChat] ====== BEGIN CHAT PROCESSING ======");
    LOG_INFO("module", "[LLMChat] OnChat (Whisper) triggered - Player: {}, Type: {} ({}), Message: {}", 
        player ? player->GetName() : "null", GetChatTypeName(type), type, msg);

    if (!player || !receiver)
    {
        LOG_ERROR("module", "[LLMChat] Cannot process chat - Player or receiver is null");
        return;
    }

    if (!LLM_Config.Enable)
    {
        LOG_DEBUG("module", "[LLMChat] Module is disabled, ignoring chat message from {}", player->GetName());
        return;
    }

    if (!ShouldProcessMessage(player, type, msg))
    {
        LOG_DEBUG("module", "[LLMChat] Message filtered out by ShouldProcessMessage");
        return;
    }

    // Check if receiver is a bot
    PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(receiver);
    if (botAI && !botAI->IsRealPlayer())
    {
        LOG_INFO("module", "[LLMChat] Receiver is a bot, queueing response");
        std::string originalMsg = msg; // Store original message
        
        // Queue response for the bot
        LLMChatQueue::EnqueueResponse(receiver, originalMsg, GetChatTypeName(type));
        
        // Clear the message since we found a bot responder
        msg.clear();
    }

    LOG_INFO("module", "[LLMChat] ====== END CHAT PROCESSING ======");
}

void LLMChatEvents::SendResponse(Player* responder, Player* sender, std::string const& response, uint32 chatType)
{
    if (!responder || !sender || !responder->IsInWorld())
        return;

    LOG_DEBUG("module.llm_chat", "Sending response from {} to {}: {}", 
        responder->GetName(), sender->GetName(), response);

    // Stop any current bot actions
    PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(responder);
    if (botAI && !botAI->IsRealPlayer())
    {
        responder->StopMoving();
        responder->ClearInCombat();
        responder->InterruptNonMeleeSpells(false);
        responder->RemoveAurasByType(SPELL_AURA_MOUNTED);
        responder->RemoveAura(433);  // Food
        responder->RemoveAura(430);  // Drink
    }

    // Send the message through appropriate channel
    switch (chatType)
    {
        case CHAT_MSG_SAY:
            responder->Say(response, LANG_UNIVERSAL);
            break;
            
        case CHAT_MSG_YELL:
            responder->Yell(response, LANG_UNIVERSAL);
            break;
            
        case CHAT_MSG_WHISPER:
            responder->Whisper(response, LANG_UNIVERSAL, sender);
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
        case CHAT_MSG_OFFICER:
            if (Guild* guild = responder->GetGuild())
            {
                guild->BroadcastToGuild(responder->GetSession(), false, 
                    response, LANG_UNIVERSAL);
            }
            break;
    }

    // Add pacification if it's a bot
    if (botAI && !botAI->IsRealPlayer())
    {
        uint32 pacifiedDuration = urand(2000, 3500);
        responder->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
        responder->m_Events.AddEvent(
            new RemovePacifiedEvent(responder),
            responder->m_Events.CalculateTime(pacifiedDuration)
        );
    }
}

bool LLMChatEvents::ShouldProcessMessage(Player* player, uint32 type, const std::string& msg)
{
    LOG_DEBUG("module", "[LLMChat] Checking message - Player: {}, Type: {} ({}), Content: {}", 
        player ? player->GetName() : "null", GetChatTypeName(type), type, msg);

    if (!player)
    {
        LOG_DEBUG("module", "[LLMChat] Null player, filtering message");
        return false;
    }

    if (!IsValidChatType(type))
    {
        LOG_DEBUG("module", "[LLMChat] Invalid chat type: {} ({})", GetChatTypeName(type), type);
        return false;
    }

    if (msg.empty())
    {
        LOG_DEBUG("module", "[LLMChat] Empty message");
        return false;
    }

    LOG_DEBUG("module", "[LLMChat] Message passed all filters");
    return true;
}

bool LLMChatEvents::IsValidChatType(uint32 type)
{
    LOG_DEBUG("module", "[LLMChat] Validating chat type: {} ({})", GetChatTypeName(type), type);
    
    // Accept all chat types
    switch (type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_TEXT_EMOTE:
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
        case CHAT_MSG_CHANNEL:
        case CHAT_MSG_AFK:
        case CHAT_MSG_DND:
        case CHAT_MSG_IGNORED:
        case CHAT_MSG_SKILL:
        case CHAT_MSG_LOOT:
            LOG_DEBUG("module", "[LLMChat] Valid chat type: {} ({})", GetChatTypeName(type), type);
            return true;
        default:
            LOG_DEBUG("module", "[LLMChat] Invalid chat type: {} ({})", GetChatTypeName(type), type);
            return false;
    }
}

// Event implementations
RemovePacifiedEvent::RemovePacifiedEvent(Player* p) : player(p) {}

bool RemovePacifiedEvent::Execute(uint64 /*time*/, uint32 /*diff*/)
{
    if (player && player->IsInWorld())
    {
        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
    }
    return true;
}

bool BotResponseEvent::Execute(uint64 /*time*/, uint32 /*diff*/)
{
    if (!responder || !responder->IsInWorld())
        return true;

    LLMChatEvents::SendResponse(responder, originalSender, response, chatType);
    return true;
}

std::vector<Player*> LLMChatEvents::GetPotentialResponders(Player* sender, uint32 type)
{
    std::vector<Player*> responders;

    if (!sender || !sender->IsInWorld())
        return responders;

    Map* map = sender->GetMap();
    if (!map)
        return responders;

    // Handle different chat types
    if (type == CHAT_MSG_SAY || type == CHAT_MSG_YELL)
    {
        // Proximity-based chat - use distance checks
        float range = LLM_Config.Chat.ChatRange;
        if (type == CHAT_MSG_YELL)
            range *= 2.0f;

        std::vector<std::pair<Player*, float>> nearbyBots;
        map->DoForAllPlayers([&](Player* player) {
            if (!player || !player->IsInWorld() || player == sender)
                return;

            PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(player);
            if (!botAI || botAI->IsRealPlayer())
                return;

            float dist = sender->GetDistance(player);
            if (dist <= range)
            {
                nearbyBots.push_back(std::make_pair(player, dist));
            }
        });

        // Sort by distance and take closest 1-2 bots
        if (!nearbyBots.empty())
        {
            std::sort(nearbyBots.begin(), nearbyBots.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            
            const size_t maxResponders = (type == CHAT_MSG_YELL) ? 2 : 1;
            const size_t numResponders = std::min(maxResponders, nearbyBots.size());
            
            for (size_t i = 0; i < numResponders; ++i)
            {
                responders.push_back(nearbyBots[i].first);
            }
        }
    }
    else if (type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER)
    {
        // Group chat - only respond with group member bots
        Group* group = sender->GetGroup();
        if (group)
        {
            map->DoForAllPlayers([&](Player* player) {
                if (!player || !player->IsInWorld() || player == sender)
                    return;

                PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(player);
                if (!botAI || botAI->IsRealPlayer())
                    return;

                // Check if bot is in the same group
                if (player->GetGroup() == group)
                {
                    responders.push_back(player);
                }
            });
        }
    }
    else if (type == CHAT_MSG_GUILD || type == CHAT_MSG_OFFICER)
    {
        // Guild chat - only respond with guild member bots
        Guild* guild = sender->GetGuild();
        if (guild)
        {
            map->DoForAllPlayers([&](Player* player) {
                if (!player || !player->IsInWorld() || player == sender)
                    return;

                PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(player);
                if (!botAI || botAI->IsRealPlayer())
                    return;

                // Check if bot is in the same guild
                if (player->GetGuild() == guild)
                {
                    responders.push_back(player);
                }
            });
        }
    }
    else if (type == CHAT_MSG_CHANNEL)
    {
        // Global channels - any bot can respond
        map->DoForAllPlayers([&](Player* player) {
            if (!player || !player->IsInWorld() || player == sender)
                return;

            PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(player);
            if (!botAI || botAI->IsRealPlayer())
                return;

            responders.push_back(player);
        });

        // Limit to 3 random responders for global channels
        if (responders.size() > 3)
        {
            static std::random_device rd;
            static std::mt19937 gen(rd());
            std::shuffle(responders.begin(), responders.end(), gen);
            responders.resize(3);
        }
    }

    return responders;
}

void LLMChatEvents::OnPlayerChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel)
{
    LOG_INFO("module", "[LLMChat] OnChat (Channel) received - Type: {} ({}) Channel: {} Message: {}", 
        GetChatTypeName(type), type, channel ? channel->GetName() : "null", msg);
    
    if (!LLM_Config.Enable || !channel)
    {
        LOG_DEBUG("module", "[LLMChat] Module is disabled or null channel, ignoring message");
        return;
    }

    if (!ShouldProcessMessage(player, type, msg))
    {
        LOG_DEBUG("module", "[LLMChat] Message filtered out by ShouldProcessMessage");
        return;
    }

    LOG_INFO("module", "[LLMChat] Processing channel message from player: {} in channel: {}", 
        player->GetName(), channel->GetName());
    
    // Get potential responders
    auto responders = GetPotentialResponders(player, type);

    if (!responders.empty())
    {
        LOG_INFO("module", "[LLMChat] Found {} potential responders", responders.size());
        std::string originalMsg = msg; // Store original message
        
        // Process message for each responder
        for (auto* responder : responders)
        {
            PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(responder);
            if (botAI && !botAI->IsRealPlayer())
            {
                LOG_INFO("module", "[LLMChat] Queueing response for bot: {}", responder->GetName());
                LLMChatQueue::EnqueueResponse(responder, originalMsg, GetChatTypeName(type));
            }
        }
        // Don't clear the message - let it display in the channel
    }
    else
    {
        LOG_DEBUG("module", "[LLMChat] No potential responders found");
    }
}

void LLMChatEvents::OnPlayerChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* /*group*/)
{
    LOG_INFO("module", "[LLMChat] OnChat (Group) received - Type: {} Message: {}", type, msg);
    
    if (!LLM_Config.Enable)
    {
        LOG_DEBUG("module", "[LLMChat] Module is disabled, ignoring message");
        return;
    }

    if (!ShouldProcessMessage(player, type, msg))
    {
        LOG_DEBUG("module", "[LLMChat] Message filtered out by ShouldProcessMessage");
        return;
    }

    LOG_INFO("module", "[LLMChat] Processing group message from player: {}", player->GetName());
    
    // Get potential responders
    auto responders = GetPotentialResponders(player, type);

    if (!responders.empty())
    {
        LOG_INFO("module", "[LLMChat] Found {} potential responders", responders.size());
        std::string originalMsg = msg; // Store original message
        
        // Process message for each responder
        for (auto* responder : responders)
        {
            PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(responder);
            if (botAI && !botAI->IsRealPlayer())
            {
                LOG_INFO("module", "[LLMChat] Queueing response for bot: {}", responder->GetName());
                LLMChatQueue::EnqueueResponse(responder, originalMsg, GetChatTypeName(type));
            }
        }
        // Don't clear the message - let it display in the group
    }
    else
    {
        LOG_DEBUG("module", "[LLMChat] No potential responders found");
    }
}

void LLMChatEvents::OnPlayerChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* /*guild*/)
{
    LOG_INFO("module", "[LLMChat] OnChat (Guild) received - Type: {} Message: {}", type, msg);
    
    if (!LLM_Config.Enable)
    {
        LOG_DEBUG("module", "[LLMChat] Module is disabled, ignoring message");
        return;
    }

    if (!ShouldProcessMessage(player, type, msg))
    {
        LOG_DEBUG("module", "[LLMChat] Message filtered out by ShouldProcessMessage");
        return;
    }

    LOG_INFO("module", "[LLMChat] Processing guild message from player: {}", player->GetName());
    
    // Get potential responders
    auto responders = GetPotentialResponders(player, type);

    if (!responders.empty())
    {
        LOG_INFO("module", "[LLMChat] Found {} potential responders", responders.size());
        std::string originalMsg = msg; // Store original message
        
        // Process message for each responder
        for (auto* responder : responders)
        {
            PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(responder);
            if (botAI && !botAI->IsRealPlayer())
            {
                LOG_INFO("module", "[LLMChat] Queueing response for bot: {}", responder->GetName());
                LLMChatQueue::EnqueueResponse(responder, originalMsg, GetChatTypeName(type));
            }
        }
        // Don't clear the message - let it display in the guild
    }
    else
    {
        LOG_DEBUG("module", "[LLMChat] No potential responders found");
    }
} 

std::string LLMChatEvents::GetChatTypeName(uint32 type)
{
    switch (type)
    {
        case CHAT_MSG_SYSTEM:          return "System";
        case CHAT_MSG_SAY:             return "Say";
        case CHAT_MSG_YELL:            return "Yell";
        case CHAT_MSG_EMOTE:           return "Emote";
        case CHAT_MSG_TEXT_EMOTE:      return "TextEmote";
        case CHAT_MSG_WHISPER:         return "Whisper";
        case CHAT_MSG_WHISPER_INFORM:  return "WhisperInform";
        case CHAT_MSG_PARTY:           return "Party";
        case CHAT_MSG_PARTY_LEADER:    return "PartyLeader";
        case CHAT_MSG_RAID:            return "Raid";
        case CHAT_MSG_RAID_LEADER:     return "RaidLeader";
        case CHAT_MSG_RAID_WARNING:    return "RaidWarning";
        case CHAT_MSG_GUILD:           return "Guild";
        case CHAT_MSG_OFFICER:         return "Officer";
        case CHAT_MSG_CHANNEL:         return "Channel";
        case CHAT_MSG_AFK:             return "AFK";
        case CHAT_MSG_DND:             return "DND";
        case CHAT_MSG_IGNORED:         return "Ignored";
        case CHAT_MSG_SKILL:           return "Skill";
        case CHAT_MSG_LOOT:            return "Loot";
        default:                       return fmt::format("Unknown({})", type);
    }
} 
