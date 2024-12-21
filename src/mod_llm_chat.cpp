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
void SendAIResponse(Player* sender, const std::string& msg, TeamId team, uint32 originalChatType);
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

        // Prepare request payload
        json requestJson = {
            {"model", LLM_Config.OllamaModel},
            {"prompt", contextPrompt},
            {"stream", false},
            {"options", {
                {"temperature", 0.9},
                {"num_predict", 100},
                {"num_ctx", 2048},
                {"num_thread", std::thread::hardware_concurrency()},
                {"top_k", 40},
                {"top_p", 0.9},
                {"repeat_penalty", 1.2}
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
        stream.connect(results);
        LOG_DEBUG("module.llm_chat", "Connected to API endpoint");

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = jsonPayload;
        req.prepare_payload();

        // Send the HTTP request
        http::write(stream, req);
        LOG_DEBUG("module.llm_chat", "Sent request to API");

        // This buffer is used for reading
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);
        LOG_DEBUG("module.llm_chat", "Received response with status: %d", static_cast<int>(res.result()));
        LOG_DEBUG("module.llm_chat", "Response body: %s", res.body().c_str());

        // Gracefully close the socket
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok)
        {
            LOG_ERROR("module.llm_chat", "HTTP error: %d", static_cast<int>(res.result()));
            return "Error: Service unavailable";
        }

        std::string response = ParseLLMResponse(res.body());
        LOG_DEBUG("module.llm_chat", "Final processed response: %s", response.c_str());
        
        return response;
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

// Add this class definition before the LLMChatModule class
class AIResponseEvent : public BasicEvent
{
public:
    AIResponseEvent(Player* p, std::string m, uint32 t, TeamId team) 
        : player(p), message(m), type(t), teamId(team) {}

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        LOG_INFO("module.llm_chat", "AIResponseEvent Execute started");

        if (!player || !player->IsInWorld())
        {
            LOG_INFO("module.llm_chat", "Invalid player or not in world");
            return true;
        }

        LOG_INFO("module.llm_chat", "Getting AI response for message: %s", message.c_str());
        
        // Get the AI response
        std::string response = QueryLLM(message, player->GetName());
        
        if (response.empty() || response.find("Error") != std::string::npos)
        {
            LOG_ERROR("module.llm_chat", "Failed to get AI response: %s", response.c_str());
            return true;
        }

        LOG_INFO("module.llm_chat", "Got AI response: %s", response.c_str());

        // Add response prefix to message
        std::string prefixedMessage = LLM_Config.ResponsePrefix + response;
        LOG_INFO("module.llm_chat", "Sending prefixed message: %s", prefixedMessage.c_str());

        switch (type)
        {
            case CHAT_MSG_SAY:
                LOG_INFO("module.llm_chat", "Sending SAY message");
                player->Say(prefixedMessage, LANG_UNIVERSAL);
                break;

            case CHAT_MSG_YELL:
                LOG_INFO("module.llm_chat", "Sending YELL message");
                player->Yell(prefixedMessage, LANG_UNIVERSAL);
                break;

            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
                if (Group* group = player->GetGroup())
                {
                    LOG_INFO("module.llm_chat", "Sending PARTY message");
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, 
                        static_cast<ChatMsg>(type), 
                        LANG_UNIVERSAL,
                        player->GetGUID(),
                        ObjectGuid::Empty,
                        prefixedMessage,
                        0);
                    group->BroadcastPacket(&data, false);
                }
                break;

            case CHAT_MSG_GUILD:
                if (Guild* guild = player->GetGuild())
                {
                    LOG_INFO("module.llm_chat", "Sending GUILD message");
                    guild->BroadcastToGuild(player->GetSession(), false, prefixedMessage, LANG_UNIVERSAL);
                }
                break;

            case CHAT_MSG_WHISPER:
                if (Player* target = ObjectAccessor::FindPlayer(player->GetTarget()))
                {
                    LOG_INFO("module.llm_chat", "Sending WHISPER message to: %s", target->GetName().c_str());
                    player->Whisper(prefixedMessage, LANG_UNIVERSAL, target);
                }
                break;

            case CHAT_MSG_CHANNEL:
                {
                    LOG_INFO("module.llm_chat", "Processing CHANNEL message");
                    std::string channelName;
                    size_t spacePos = message.find(' ');
                    if (spacePos != std::string::npos)
                    {
                        channelName = message.substr(0, spacePos);
                        std::string channelMessage = prefixedMessage.substr(spacePos + 1);
                        
                        LOG_INFO("module.llm_chat", "Channel: %s, Message: %s", channelName.c_str(), channelMessage.c_str());
                        
                        if (ChannelMgr* cMgr = ChannelMgr::forTeam(teamId))
                        {
                            if (Channel* channel = cMgr->GetChannel(channelName, player))
                            {
                                LOG_INFO("module.llm_chat", "Sending message to channel");
                                channel->Say(player->GetGUID(), channelMessage, LANG_UNIVERSAL);
                            }
                            else
                            {
                                LOG_ERROR("module.llm_chat", "Failed to get channel: %s", channelName.c_str());
                            }
                        }
                        else
                        {
                            LOG_ERROR("module.llm_chat", "Failed to get ChannelMgr for team: %d", teamId);
                        }
                    }
                }
                break;
        }

        LOG_INFO("module.llm_chat", "AIResponseEvent Execute completed");
        return true;
    }

private:
    Player* player;
    std::string message;
    uint32 type;
    TeamId teamId;
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

        // Only process messages from real players to trigger bot responses
        // Skip if the message is from a bot
        if (player->GetSession() && player->GetSession()->IsBot())
        {
            LOG_INFO("module.llm_chat", "Skipping message from bot");
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
                AIResponseEvent* event = new AIResponseEvent(player, msg, type, player->GetTeamId());
                if (event)
                {
                    player->m_Events.AddEvent(event, player->m_Events.CalculateTime(delay));
                    LOG_INFO("module.llm_chat", "Successfully added AI response event");
                }
                else
                {
                    LOG_ERROR("module.llm_chat", "Failed to create AI response event");
                }
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
        LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Model", "llama2");
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
void SendAIResponse(Player* sender, const std::string& msg, TeamId team, uint32 originalChatType)
{
    if (!sender || !sender->IsInWorld() || msg.empty())
    {
        return;
    }

    try {
        // Get a list of eligible bots
        std::vector<Player*> eligibleBots;
        Map* map = sender->GetMap();
        if (!map)
            return;

        // Determine how many bots should respond (random between 1 and 3)
        uint32 numResponders = urand(1, 3);
        float chatRange = (originalChatType == CHAT_MSG_YELL) ? LLM_Config.ChatRange * 2 : LLM_Config.ChatRange;
        bool requiresDistance = (originalChatType == CHAT_MSG_SAY || originalChatType == CHAT_MSG_YELL);

        map->DoForAllPlayers([&](Player* potentialBot) {
            if (!potentialBot || !potentialBot->GetSession() || !potentialBot->IsInWorld())
                return;

            if (potentialBot->GetSession()->IsBot())
            {
                bool isEligible = false;
                
                if (requiresDistance)
                {
                    // Check distance for SAY/YELL
                    float distance = sender->GetDistance(potentialBot);
                    isEligible = (distance <= chatRange);
                }
                else
                {
                    switch (originalChatType)
                    {
                        case CHAT_MSG_PARTY:
                        case CHAT_MSG_PARTY_LEADER:
                            isEligible = potentialBot->GetGroup() && potentialBot->GetGroup() == sender->GetGroup();
                            break;
                        case CHAT_MSG_GUILD:
                            isEligible = potentialBot->GetGuild() && potentialBot->GetGuild() == sender->GetGuild();
                            break;
                        case CHAT_MSG_CHANNEL:
                            // For channel chat, check if bot is in the same channel
                            if (ChannelMgr* cMgr = ChannelMgr::forTeam(team))
                            {
                                // Extract channel name from the message
                                std::string channelName;
                                std::string message = msg;
                                size_t spacePos = message.find(' ');
                                if (spacePos != std::string::npos)
                                {
                                    channelName = message.substr(0, spacePos);
                                    isEligible = (cMgr->GetChannel(channelName, potentialBot) != nullptr);
                                }
                            }
                            break;
                        case CHAT_MSG_WHISPER:
                            isEligible = true;
                            break;
                    }
                }

                if (isEligible)
                {
                    eligibleBots.push_back(potentialBot);
                }
            }
        });

        // Shuffle the eligible bots list
        if (eligibleBots.size() > 1)
        {
            std::random_shuffle(eligibleBots.begin(), eligibleBots.end());
        }

        // Limit the number of responders to available bots
        numResponders = std::min(numResponders, (uint32)eligibleBots.size());

        // Have each selected bot respond with a different personality
        for (uint32 i = 0; i < numResponders; ++i)
        {
            Player* respondingBot = eligibleBots[i];
            
            // Select a random personality for this bot
            BotPersonality personality = BOT_PERSONALITIES[urand(0, BOT_PERSONALITIES.size() - 1)];
            
            // Add personality to the context
            std::string contextPrompt = personality.prompt + "\n\n" +
                "You are responding to: " + sender->GetName() + ": " + msg + "\n" +
                "Keep responses very short (1-2 lines max)\n" +
                "Use common WoW abbreviations when appropriate\n" +
                "Stay in character and be natural";

            // Get AI response with this personality
            std::string response = QueryLLM(contextPrompt, sender->GetName());
            
            if (!response.empty() && response.find("Error") == std::string::npos)
            {
                // Add a small delay between responses (100-500ms per bot)
                uint32 delay = 100 * (i + 1) + urand(0, 400);
                
                respondingBot->m_Events.AddEvent(
                    new AIResponseEvent(respondingBot, response, originalChatType, team),
                    respondingBot->m_Events.CalculateTime(delay)
                );
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "Exception in SendAIResponse: %s", e.what());
    }
}

void Add_LLMChatScripts()
{
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatModule();
} 