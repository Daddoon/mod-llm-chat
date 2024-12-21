/*
** Made by Krazor
** AzerothCore 2019 http://www.azerothcore.org/
** Based on LLM Chat integration
*/

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "World.h"
#include "Channel.h"
#include "Guild.h"
#include "ChannelMgr.h"
#include "mod_llm_chat.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "mod-playerbots/src/PlayerbotAI.h"
#include "mod-playerbots/src/PlayerbotMgr.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// Forward declarations
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team);
std::string QueryLLM(std::string const& message, const std::string& playerName);

namespace {
/* Config Variables */
struct LLMConfig
{
    bool Enabled;
        int32 Provider;
    std::string OllamaEndpoint;
    std::string OllamaModel;
    float ChatRange;
    std::string ResponsePrefix;
        int32 LogLevel;
        // URL parsing components
        std::string Host;
        std::string Port;
        std::string Target;
};

LLMConfig LLM_Config;
}

// Helper function to parse URL
void ParseEndpointURL(std::string const& endpoint, LLMConfig& config)
{
    try {
        size_t protocolEnd = endpoint.find("://");
        if (protocolEnd == std::string::npos) {
            LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("Invalid endpoint URL (no protocol): %s", endpoint.c_str()).c_str());
            return;
        }

        std::string url = endpoint.substr(protocolEnd + 3);
        size_t pathStart = url.find('/');
        if (pathStart == std::string::npos) {
            LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("Invalid endpoint URL (no path): %s", endpoint.c_str()).c_str());
            return;
        }

        std::string hostPort = url.substr(0, pathStart);
        config.Target = url.substr(pathStart);

        size_t portStart = hostPort.find(':');
        if (portStart != std::string::npos) {
            config.Host = hostPort.substr(0, portStart);
            config.Port = hostPort.substr(portStart + 1);
            
            // Validate port
            try {
                int port = std::stoi(config.Port);
                if (port <= 0 || port > 65535) {
                    LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("Invalid port number: %s", config.Port.c_str()).c_str());
                    config.Port = "11434"; // Default to Ollama port
                }
            } catch (std::exception const& e) {
                LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("Invalid port format: %s", e.what()).c_str());
                config.Port = "11434"; // Default to Ollama port
            }
        } else {
            config.Host = hostPort;
            config.Port = "11434"; // Default to Ollama port
        }

        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("URL parsed successfully - Host: %s, Port: %s, Target: %s", 
                config.Host.c_str(), config.Port.c_str(), config.Target.c_str()).c_str());
    }
    catch (std::exception const& e) {
        LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("Error parsing URL: %s", e.what()).c_str());
        // Set defaults
        config.Host = "localhost";
        config.Port = "11434";
        config.Target = "/api/generate";
    }
}

std::string ParseLLMResponse(const std::string& rawResponse)
{
    try {
        LOG_DEBUG("module.llm_chat", "Parsing raw response: %s", rawResponse.c_str());
        
        auto jsonResponse = json::parse(rawResponse);
        
        // Check for response field
        if (jsonResponse.contains("response"))
        {
            std::string response = jsonResponse["response"].get<std::string>();
            LOG_DEBUG("module.llm_chat", "Parsed response: %s", response.c_str());
            return response;
        }
        
        // Fallback to checking for text field
        if (jsonResponse.contains("text"))
        {
            std::string response = jsonResponse["text"].get<std::string>();
            LOG_DEBUG("module.llm_chat", "Parsed response from text field: %s", response.c_str());
            return response;
        }

        LOG_ERROR("module.llm_chat", "No valid response field found in JSON");
        return "Error: Invalid response format";
    }
    catch (const json::parse_error& e)
    {
        LOG_ERROR("module.llm_chat", "JSON parse error: %s", e.what());
        return "Error: Failed to parse response";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "Error parsing response: %s", e.what());
        return "Error: Failed to process response";
    }
}

std::string DetectTone(const std::string& message) {
    // Convert message to lowercase for easier matching
    std::string lowerMsg = message;
    std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);

    // Check for aggressive/angry tone
    if (lowerMsg.find("!") != std::string::npos ||
        lowerMsg.find("hate") != std::string::npos ||
        lowerMsg.find("stupid") != std::string::npos ||
        lowerMsg.find("angry") != std::string::npos ||
        lowerMsg.find("fight") != std::string::npos) {
        return "aggressive";
    }

    // Check for friendly/happy tone
    if (lowerMsg.find("hello") != std::string::npos ||
        lowerMsg.find("hey") != std::string::npos ||
        lowerMsg.find("thanks") != std::string::npos ||
        lowerMsg.find("lol") != std::string::npos ||
        lowerMsg.find(":)") != std::string::npos ||
        lowerMsg.find("happy") != std::string::npos) {
        return "friendly";
    }

    // Check for sad/melancholic tone
    if (lowerMsg.find("sad") != std::string::npos ||
        lowerMsg.find("sorry") != std::string::npos ||
        lowerMsg.find("miss") != std::string::npos ||
        lowerMsg.find(":(") != std::string::npos ||
        lowerMsg.find("worried") != std::string::npos) {
        return "sad";
    }

    // Check for formal/respectful tone
    if (lowerMsg.find("please") != std::string::npos ||
        lowerMsg.find("would you") != std::string::npos ||
        lowerMsg.find("excuse") != std::string::npos ||
        lowerMsg.find("sir") != std::string::npos ||
        lowerMsg.find("madam") != std::string::npos) {
        return "formal";
    }

    return "neutral";
}

std::string GetMoodBasedResponse(const std::string& tone) {
    if (tone == "aggressive") {
        return "You are a proud warrior of your faction, ready to defend your honor. "
               "Your responses should be bold and challenging, but maintain respect for fellow warriors. "
               "Use phrases like 'For the Horde/Alliance!' and reference your combat prowess.";
    }
    else if (tone == "friendly") {
        return "You are a jovial adventurer, always ready to share tales of your journeys. "
               "Be warm and welcoming, maybe share a laugh about murlocs or reference popular gathering spots. "
               "Use emotes like /wave, /smile, or /cheer in your responses.";
    }
    else if (tone == "sad") {
        return "You are an empathetic soul who has seen both victory and loss in Azeroth. "
               "Offer words of comfort and reference the hope that always exists, even in the darkest times. "
               "Maybe mention inspiring moments from Warcraft lore.";
    }
    else if (tone == "formal") {
        return "You are a learned scholar of Azeroth's history and customs. "
               "Speak with the wisdom of one who has studied in Dalaran or consulted with the nobles of Stormwind. "
               "Reference historical events and maintain proper etiquette.";
    }
    return "You are a seasoned adventurer with many tales to share. "
           "Be natural but always stay true to the World of Warcraft setting.";
}

std::string QueryLLM(std::string const& message, const std::string& playerName)
{
    if (message.empty() || playerName.empty())
    {
        LOG_ERROR("module.llm_chat", "Empty message or player name");
        return "Error: Invalid input";
    }

    try {
        // Detect the tone of the message
        std::string tone = DetectTone(message);
        LOG_DEBUG("module.llm_chat", "Detected tone: %s", tone.c_str());
        
        // Create a context that reflects actual WoW chat style
        std::string contextPrompt = 
            "You are a player in World of Warcraft. Respond naturally like a real player would in-game. Keep in mind:\n"
            "- Keep responses very short (1-2 lines max)\n"
            "- Use common WoW abbreviations (e.g. ty, np, lf2m, wts, wtb)\n"
            "- Reference game locations and items naturally\n"
            "- Don't use emotes in text form\n"
            "- If referring to the player you're responding to, use their name: " + playerName + "\n"
            "- Stay in character as a player, not an NPC\n"
            "- Be casual and friendly, like a real gamer\n\n"
            "The message you're responding to is from " + playerName + ": " + message;

        LOG_DEBUG("module.llm_chat", "Context prompt: %s", contextPrompt.c_str());

        // Prepare request payload with optimized parameters for llama2:3.2b
        json requestJson = {
            {"model", LLM_Config.OllamaModel},
            {"prompt", contextPrompt},
            {"stream", false},
            {"options", {
                {"temperature", 0.7},     // Lower temperature for more focused responses
                {"num_predict", 48},      // Shorter responses for chat
                {"num_ctx", 512},         // Smaller context window for faster responses
                {"num_thread", std::thread::hardware_concurrency()},
                {"top_k", 20},            // More focused token selection
                {"top_p", 0.7},           // More focused sampling
                {"repeat_penalty", 1.1},   // Slightly lower to allow some repetition in chat
                {"stop", {"\n", ".", "!", "?"}} // Stop at natural sentence endings
            }}
        };

        std::string jsonPayload = requestJson.dump();
        LOG_DEBUG("module.llm_chat", "Request payload: %s", jsonPayload.c_str());

        // Set up the IO context
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
        LOG_DEBUG("module.llm_chat", "Resolved host %s:%s", LLM_Config.Host.c_str(), LLM_Config.Port.c_str());

        // Make the connection
        beast::error_code ec;
        stream.connect(results, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to connect to API: %s", ec.message().c_str());
            return "Error: Connection failed";
        }
        LOG_DEBUG("module.llm_chat", "Connected to API endpoint");

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = jsonPayload;
        req.prepare_payload();

        // Send the HTTP request
        http::write(stream, req, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to send request: %s", ec.message().c_str());
            return "Error: Request failed";
        }
        LOG_DEBUG("module.llm_chat", "Sent request to API");

        // This buffer is used for reading
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to read response: %s", ec.message().c_str());
            return "Error: Response failed";
        }
        LOG_DEBUG("module.llm_chat", "Received response with status: %d", static_cast<int>(res.result()));
        LOG_DEBUG("module.llm_chat", "Response body: %s", res.body().c_str());

        // Gracefully close the socket
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok)
        {
            LOG_ERROR("module.llm_chat", "HTTP error %d: %s", static_cast<int>(res.result()), res.body().c_str());
            return "Error: Service unavailable";
        }

        std::string response = ParseLLMResponse(res.body());
        LOG_DEBUG("module.llm_chat", "Final processed response: %s", response.c_str());
        
        return response;
    }
    catch (const boost::system::system_error& e)
    {
        LOG_ERROR("module.llm_chat", "Boost system error: %s", e.what());
        return "Error: Network error";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "API Error: %s", e.what());
        return "Error: Service error";
    }
}

class LLMChatLogger {
public:
    static void Log(int32 level, std::string const& message) {
        if (LLM_Config.LogLevel >= level) {
            LOG_INFO("module.llm_chat", "%s", message.c_str());
        }
    }

    static void LogChat(std::string const& playerName, std::string const& input, std::string const& response) {
        if (LLM_Config.LogLevel >= 2) {
            LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Player: %s, Input: %s", playerName.c_str(), input.c_str()).c_str());
            LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("AI Response: %s", response.c_str()).c_str());
        }
    }
};

// Create a custom event class for bot responses
class BotResponseEvent : public BasicEvent
{
    Player* responder;
    Player* originalSender;
    std::string response;
    uint32 chatType;
    std::string message;
    TeamId team;

public:
    BotResponseEvent(Player* r, Player* s, std::string resp, uint32 t, std::string m, TeamId tm) 
        : responder(r), originalSender(s), response(resp), chatType(t), message(m), team(tm) {}

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (!responder || !responder->IsInWorld())
            return true;

        // Double check we're not responding as the original sender
        if (originalSender && 
            (responder == originalSender || 
             (responder->GetSession() && originalSender->GetSession() && 
              responder->GetSession()->GetAccountId() == originalSender->GetSession()->GetAccountId())))
        {
            LOG_ERROR("module.llm_chat", "Prevented response from original sender's account");
            return true;
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
                        LANG_UNIVERSAL, responder->GetGUID(), ObjectGuid::Empty, 
                        response, 0);
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
                            // Get AI response
                            std::string response = QueryLLM(message, responder->GetName());
                            if (!response.empty())
                            {
                                // Use Say method with player's GUID
                                channel->Say(responder->GetGUID(), response.c_str(), LANG_UNIVERSAL);
                            }
                        }
                    }
                }
                break;
        }
        return true;
    }
};

// Add this class definition before the LLMChatModule class
class TriggerResponseEvent : public BasicEvent
{
    Player* player;
    std::string message;
    uint32 chatType;
    TeamId team;

public:
    TriggerResponseEvent(Player* p, std::string msg, uint32 type) 
        : player(p), message(msg), chatType(type), team(p->GetTeamId()) {}

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (!player || !player->IsInWorld())
            return true;

        SendAIResponse(player, message, chatType, team);
        return true;
    }
};

class LLMChatModule : public PlayerScript
{
public:
    LLMChatModule() : PlayerScript("LLMChatModule") {}

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        LOG_INFO("module.llm_chat", "OnChat triggered - checking conditions...");

        if (!LLM_Config.Enabled)
        {
            LOG_INFO("module.llm_chat", "Module is disabled in config");
            return;
        }

        if (!player || msg.empty())
        {
            LOG_INFO("module.llm_chat", "Invalid player or empty message");
            return;
        }

        // Skip if this is an AI response to prevent loops
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
        {
            LOG_INFO("module.llm_chat", "Skipping AI response message");
            return;
        }

        // Only process messages from real players
        if (!player->GetSession() || player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
        {
            LOG_INFO("module.llm_chat", "Skipping message from GM or invalid session");
            return;
        }

        LOG_INFO("module.llm_chat", "Chat received from player - Player: %s, Type: %s, Message: '%s'", 
            player->GetName().c_str(), 
            GetChatTypeString(type).c_str(), 
            msg.c_str());

        // Handle different chat types
        switch (type)
        {
            case CHAT_MSG_SAY:
            case CHAT_MSG_YELL:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_GUILD:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_CHANNEL:
            {
                LOG_INFO("module.llm_chat", "Processing player message for bot responses - Type: %s, Message: '%s'", 
                    GetChatTypeString(type).c_str(), msg.c_str());

                // Add a small delay before processing
                uint32 delay = urand(100, 500);
                LOG_INFO("module.llm_chat", "Adding AI response event with delay: %u ms", delay);

                // Create and add the event
                TriggerResponseEvent* event = new TriggerResponseEvent(player, msg, type);
                player->m_Events.AddEvent(event, player->m_Events.CalculateTime(delay));

                LOG_INFO("module.llm_chat", "Successfully added AI response event");
                break;
            }
            default:
                LOG_INFO("module.llm_chat", "Ignoring unsupported chat type: %u", type);
                break;
        }
    }

    // Helper function to get chat type string
    std::string GetChatTypeString(uint32 type)
    {
        switch (type)
        {
            case CHAT_MSG_SAY: return "SAY";
            case CHAT_MSG_YELL: return "YELL";
            case CHAT_MSG_PARTY: return "PARTY";
            case CHAT_MSG_PARTY_LEADER: return "PARTY_LEADER";
            case CHAT_MSG_GUILD: return "GUILD";
            case CHAT_MSG_WHISPER: return "WHISPER";
            case CHAT_MSG_CHANNEL: return "CHANNEL";
            default: return "UNKNOWN";
        }
    }
};

class LLMChatAnnounce : public PlayerScript
{
public:
    LLMChatAnnounce() : PlayerScript("LLMChatAnnounce") {}

    void OnLogin(Player* player) override
    {
        // Announce Module
        if (sConfigMgr->GetOption<int32>("LLMChat.Announce", 1))
        {
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00LLM Chat|r module.");
        }
    }
};

class LLMChatConfig : public WorldScript
{
public:
    LLMChatConfig() : WorldScript("LLMChatConfig") {}

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        LOG_INFO("module.llm_chat", "Loading LLM Chat configuration...");

        LLM_Config.Enabled = sConfigMgr->GetOption<int32>("LLMChat.Enable", 0) == 1;
        LLM_Config.Provider = sConfigMgr->GetOption<int32>("LLMChat.Provider", 1);
        LLM_Config.OllamaEndpoint = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Endpoint", "http://localhost:11434/api/generate");
        LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Model", "llama3.2:1b");
        LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLMChat.ChatRange", 25.0f);
        LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLMChat.ResponsePrefix", "[AI] ");
        LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLMChat.LogLevel", 3);

        // Parse the endpoint URL
        ParseEndpointURL(LLM_Config.OllamaEndpoint, LLM_Config);

        // Log the loaded configuration
        LOG_INFO("module.llm_chat", "=== LLM Chat Configuration ===");
        LOG_INFO("module.llm_chat", "Enabled: %s", LLM_Config.Enabled ? "true" : "false");
        LOG_INFO("module.llm_chat", "Provider: %d", LLM_Config.Provider);
        LOG_INFO("module.llm_chat", "Endpoint: %s", LLM_Config.OllamaEndpoint.c_str());
        LOG_INFO("module.llm_chat", "Model: %s", LLM_Config.OllamaModel.c_str());
        LOG_INFO("module.llm_chat", "Host: %s", LLM_Config.Host.c_str());
        LOG_INFO("module.llm_chat", "Port: %s", LLM_Config.Port.c_str());
        LOG_INFO("module.llm_chat", "Target: %s", LLM_Config.Target.c_str());
        LOG_INFO("module.llm_chat", "Response Prefix: '%s'", LLM_Config.ResponsePrefix.c_str());
        LOG_INFO("module.llm_chat", "Chat Range: %.2f", LLM_Config.ChatRange);
        LOG_INFO("module.llm_chat", "Log Level: %d", LLM_Config.LogLevel);
        LOG_INFO("module.llm_chat", "=== End Configuration ===");
    }
};

Player* GetNearbyBot(Player* player, float maxDistance)
{
    if (!player || !player->IsInWorld())
        return nullptr;

    Map* map = player->GetMap();
    if (!map)
        return nullptr;

    std::vector<Player*> botList;
    float playerX = player->GetPositionX();
    float playerY = player->GetPositionY();
    float playerZ = player->GetPositionZ();

    // Iterate through all players on the map
    map->DoForAllPlayers([&](Player* potentialBot) {
        if (!potentialBot || potentialBot == player || !potentialBot->IsInWorld())
            return;

        // Check if it's a bot
        if (!potentialBot->GetSession() || !potentialBot->GetSession()->IsBot())
            return;

        // Calculate 3D distance
        float distance = std::sqrt(
            std::pow(playerX - potentialBot->GetPositionX(), 2) +
            std::pow(playerY - potentialBot->GetPositionY(), 2) +
            std::pow(playerZ - potentialBot->GetPositionZ(), 2)
        );

        if (distance <= maxDistance)
        {
            botList.push_back(potentialBot);
        }
    });

    if (botList.empty())
        return nullptr;

    // Select random bot from available ones
    uint32 randomIndex = urand(0, botList.size() - 1);
    return botList[randomIndex];
}

// Add these personality definitions at the top of the file after the includes
struct BotPersonality {
    std::string trait;
    std::string prompt;
};

std::vector<BotPersonality> BOT_PERSONALITIES = {
    {
        "Warrior",
        "You are a proud warrior who values honor and combat. Use terms like 'For Honor!' and reference battles and weapons."
    },
    {
        "Scholar",
        "You are a knowledgeable scholar interested in lore and history. Reference books, magic, and historical events."
    },
    {
        "Trader",
        "You are a savvy merchant. Talk about gold, trades, and the auction house. Use terms like 'wts', 'wtb', and discuss prices."
    },
    {
        "Adventurer",
        "You are an enthusiastic explorer. Share stories about dungeons, quests, and discoveries. Be excited about adventures."
    },
    {
        "Roleplayer",
        "You are deeply immersed in your character. Use rich fantasy language and stay true to WoW lore."
    },
    {
        "Casual",
        "You are a laid-back player. Use lots of game abbreviations, be friendly and relaxed."
    }
};

// Modify the SendAIResponse function to handle multiple bot responses
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team)
{
    if (!sender || !sender->IsInWorld())
        return;

    Map* map = sender->GetMap();
    if (!map)
        return;

    // Get all players in range
    std::list<Player*> nearbyPlayers;
    float maxDistance = (chatType == CHAT_MSG_YELL) ? 300.0f : 25.0f; // Default yell range is 300 yards, say range is 25 yards

    // For party/guild/channel chat, we don't need distance checks
    if (chatType == CHAT_MSG_PARTY || chatType == CHAT_MSG_PARTY_LEADER ||
        chatType == CHAT_MSG_GUILD || chatType == CHAT_MSG_CHANNEL)
    {
        maxDistance = std::numeric_limits<float>::max();
    }

    map->DoForAllPlayers([&nearbyPlayers, sender, maxDistance, chatType, &msg, team](Player* player) {
        if (player && player->IsInWorld() && player != sender)
        {
            // For party chat, check if in same group
            if (chatType == CHAT_MSG_PARTY || chatType == CHAT_MSG_PARTY_LEADER)
            {
                Group* group = sender->GetGroup();
                if (!group || !group->IsMember(player->GetGUID()))
                    return;
            }

            // For guild chat, check if in same guild
            if (chatType == CHAT_MSG_GUILD)
            {
                Guild* guild = sender->GetGuild();
                if (!guild || guild->GetId() != player->GetGuildId())
                    return;
            }

            // For channel chat, check if in same channel
            if (chatType == CHAT_MSG_CHANNEL)
            {
                size_t spacePos = msg.find(' ');
                if (spacePos != std::string::npos)
                {
                    std::string channelName = msg.substr(0, spacePos);
                    if (ChannelMgr* cMgr = ChannelMgr::forTeam(team))
                    {
                        if (Channel* channel = cMgr->GetChannel(channelName, sender))
                        {
                            // Get AI response
                            std::string response = QueryLLM(msg, sender->GetName());
                            if (!response.empty())
                            {
                                // Use Say method with player's GUID
                                channel->Say(sender->GetGUID(), response.c_str(), LANG_UNIVERSAL);
                            }
                        }
                    }
                }
            }

            // For say/yell, check distance
            if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_YELL)
            {
                if (sender->GetDistance(player) > maxDistance)
                    return;
            }

            nearbyPlayers.push_back(player);
        }
    });

    if (nearbyPlayers.empty())
    {
        LOG_DEBUG("module.llm_chat", "No eligible players found to respond");
        return;
    }

    // Select a random player to respond
    uint32 randomIndex = urand(0, nearbyPlayers.size() - 1);
    auto it = nearbyPlayers.begin();
    std::advance(it, randomIndex);
    Player* respondingPlayer = *it;

    if (!respondingPlayer || !respondingPlayer->IsInWorld())
    {
        LOG_ERROR("module.llm_chat", "Selected player is invalid or not in world");
        return;
    }

    // Get AI response
    std::string response = QueryLLM(msg, sender->GetName());
    if (response.empty())
    {
        LOG_ERROR("module.llm_chat", "Failed to get AI response");
        return;
    }

    // Add a random delay between 1-3 seconds
    uint32 delay = urand(1000, 3000);

    // Schedule the response using the custom event
    BotResponseEvent* event = new BotResponseEvent(respondingPlayer, sender, response, chatType, msg, team);
    respondingPlayer->m_Events.AddEvent(event, respondingPlayer->m_Events.CalculateTime(delay));
}

void Add_LLMChatScripts()
{
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatModule();
    new LLMChatPlayerScript();
}

class LLMChatPlayerScript : public PlayerScript
{
public:
    LLMChatPlayerScript() : PlayerScript("LLMChatPlayerScript") {}

    void OnChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
    {
        if (!player || !player->IsInWorld())
            return;   

        // Skip if message is empty or too short
        if (msg.empty() || msg.length() < 2)
            return;

        // Process the message and send AI response
        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChatGroup(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
    {
        if (!player || !player->IsInWorld() || !group)
            return;
      

        // Skip if message is empty or too short
        if (msg.empty() || msg.length() < 2)
            return;

        // Process the message and send AI response
        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChatGuild(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* guild) override
    {
        if (!player || !player->IsInWorld() || !guild)
            return;

        // Skip if message is empty or too short
        if (msg.empty() || msg.length() < 2)
            return;

        // Process the message and send AI response
        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChatChannel(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override
    {
        if (!player || !player->IsInWorld() || !channel)
            return;

        // Skip if message is empty or too short
        if (msg.empty() || msg.length() < 2)
            return;

        // Process the message and send AI response
        SendAIResponse(player, msg, type, player->GetTeamId());
    }
}; 