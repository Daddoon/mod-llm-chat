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

std::string ParseLLMResponse(std::string const& rawResponse)
{
    try {
        LOG_INFO("module.llm_chat", "%s", "Parsing API Response...");
        
        // Split response by newlines in case of streaming
        std::string finalResponse;
        std::istringstream stream(rawResponse);
        std::string line;
        
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            
            try {
                json responseObj = json::parse(line);
                
                // Extract response text
                if (responseObj.contains("response")) {
                    std::string responseText = responseObj["response"].get<std::string>();
                    LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Got response part: %s", responseText.c_str()).c_str());
                    finalResponse += responseText;
                }
                
                // If this is the final message in a stream
                if (responseObj.contains("done") && responseObj["done"].get<bool>()) {
                    LOG_INFO("module.llm_chat", "%s", "Found end of stream");
                    break;
                }
            }
            catch (json::parse_error const& e) {
                LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("Failed to parse line: %s", line.c_str()).c_str());
                continue;
            }
        }
        
        if (!finalResponse.empty()) {
            LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Final combined response: %s", finalResponse.c_str()).c_str());
            return finalResponse;
        }
        
        // Fallback: try parsing as single response
        json singleResponse = json::parse(rawResponse);
        if (singleResponse.contains("response")) {
            std::string response = singleResponse["response"].get<std::string>();
            LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Single response: %s", response.c_str()).c_str());
            return response;
        }
        
        LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("No valid response found in: %s", rawResponse.c_str()).c_str());
        return "Error parsing LLM response";
    }
    catch (json::parse_error const& e) {
        LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("JSON parse error: %s", e.what()).c_str());
        return "Error parsing response";
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

        // Prepare request payload with enhanced parameters
        std::string jsonPayload = json({
            {"model", LLM_Config.OllamaModel},
            {"prompt", contextPrompt},
            {"stream", false},
            {"raw", false},
            {"options", {
                {"temperature", 0.9},    // Keep high for creative responses
                {"num_predict", 100},    // Limit response length
                {"num_ctx", 2048},       // Large context window for lore
                {"num_thread", std::thread::hardware_concurrency()},
                {"num_gpu", 1},
                {"top_k", 40},
                {"top_p", 0.9},
                {"repeat_penalty", 1.2},  // Increased to reduce repetition
                {"mirostat", 2},
                {"mirostat_tau", 5.0},
                {"mirostat_eta", 0.1}
            }}
        }).dump();

        LOG_DEBUG("module.llm_chat", "=== API Request ===");
        LOG_DEBUG("module.llm_chat", "Model: %s", LLM_Config.OllamaModel.c_str());
        LOG_DEBUG("module.llm_chat", "Input: %s", message.c_str());
        LOG_DEBUG("module.llm_chat", "Full Request: %s", jsonPayload.c_str());

        // Set up the IO context
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
        LOG_DEBUG("module.llm_chat", "Connecting to: %s:%s", LLM_Config.Host.c_str(), LLM_Config.Port.c_str());

        // Make the connection on the IP address we get from a lookup
        stream.connect(results);
        LOG_DEBUG("module.llm_chat", "Connected to Ollama API");

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = jsonPayload;
        req.prepare_payload();

        // Send the HTTP request to the remote host
        http::write(stream, req);
        LOG_DEBUG("module.llm_chat", "Request sent to API");

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);
        LOG_DEBUG("module.llm_chat", "=== API Response ===");
        LOG_DEBUG("module.llm_chat", "Status: %d", static_cast<int>(res.result()));
        LOG_DEBUG("module.llm_chat", "Raw Response: %s", res.body().c_str());

        // Gracefully close the socket
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok)
        {
            LOG_ERROR("module.llm_chat", "HTTP error: %d", static_cast<int>(res.result()));
            return "Error: Service unavailable";
        }

        std::string response = ParseLLMResponse(res.body());
        if (response.empty())
        {
            LOG_ERROR("module.llm_chat", "Empty response after parsing");
            return "Error: Empty response";
        }

        LOG_DEBUG("module.llm_chat", "Final Processed Response: %s", response.c_str());
        LOG_DEBUG("module.llm_chat", "=== End API Transaction ===\n");
        return response;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "API Error: %s", e.what());
        return "Error: Service error";
    }
    catch (...)
    {
        LOG_ERROR("module.llm_chat", "Unknown API Error");
        return "Error: Unknown error";
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
        if (player && player->IsInWorld())
        {
            // Build the chat packet
            WorldPacket data;
            switch (type)
            {
                case CHAT_MSG_SAY:
                {
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_SAY, message, LANG_UNIVERSAL, CHAT_TAG_NONE, player->GetGUID(), player->GetName());
                    player->SendMessageToSetInRange(&data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY), true);
                    break;
                }
                case CHAT_MSG_YELL:
                {
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_YELL, message, LANG_UNIVERSAL, CHAT_TAG_NONE, player->GetGUID(), player->GetName());
                    player->SendMessageToSetInRange(&data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL), true);
                    break;
                }
                case CHAT_MSG_PARTY:
                case CHAT_MSG_PARTY_LEADER:
                {
                    if (Group* group = player->GetGroup())
                    {
                        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, message, LANG_UNIVERSAL, CHAT_TAG_NONE, player->GetGUID(), player->GetName());
                        group->BroadcastPacket(&data, false);
                    }
                    break;
                }
                case CHAT_MSG_GUILD:
                {
                    if (Guild* guild = player->GetGuild())
                    {
                        ChatHandler::BuildChatPacket(data, CHAT_MSG_GUILD, message, LANG_UNIVERSAL, CHAT_TAG_NONE, player->GetGUID(), player->GetName());
                        guild->BroadcastPacket(&data);
                    }
                    break;
                }
                case CHAT_MSG_WHISPER:
                {
                    // For whispers, we need to find the target player
                    if (ObjectGuid targetGuid = player->GetTarget())
                    {
                        if (Player* target = ObjectAccessor::FindPlayer(targetGuid))
                        {
                            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, message, LANG_UNIVERSAL, CHAT_TAG_NONE, player->GetGUID(), player->GetName());
                            target->GetSession()->SendPacket(&data);
                        }
                    }
                    break;
                }
                case CHAT_MSG_CHANNEL:
                {
                    if (ChannelMgr* cMgr = ChannelMgr::forTeam(teamId))
                    {
                        // Extract channel name from the message
                        std::string channelName;
                        size_t spacePos = message.find(' ');
                        if (spacePos != std::string::npos)
                        {
                            channelName = message.substr(0, spacePos);
                            
                            if (Channel* channel = cMgr->GetChannel(channelName, player))
                            {
                                ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, 
                                    message.substr(spacePos + 1),
                                    LANG_UNIVERSAL,
                                    CHAT_TAG_NONE,
                                    player->GetGUID(),
                                    player->GetName(),
                                    nullptr,
                                    "",
                                    channelName);

                                // Send to all players in the channel
                                SessionMap sessions = sWorld->GetAllSessions();
                                for (SessionMap::iterator itr = sessions.begin(); itr != sessions.end(); ++itr)
                                {
                                    if (!itr->second || !itr->second->GetPlayer())
                                        continue;

                                    Player* target = itr->second->GetPlayer();
                                    if (target->IsInWorld() && cMgr->GetChannel(channelName, target))
                                    {
                                        target->GetSession()->SendPacket(&data);
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
        return true;
    }

private:
    Player* player;
    std::string message;
    uint32 type;
    TeamId teamId;

    static std::string GetChatTypeString(uint32 type)
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

class LLMChatModule : public PlayerScript
{
public:
    LLMChatModule() : PlayerScript("LLMChatModule") {}

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        if (!LLM_Config.Enabled || !player || msg.empty())
        {
            return;
        }

        // Store the message for processing after it's sent
        std::string message = msg;

        // Check if the message is from a bot and if we should allow bot-to-bot interactions
        bool isBot = player->GetSession() && player->GetSession()->IsBot();
        static const float BOT_RESPONSE_CHANCE = 0.5f; // 50% chance for bots to respond to other bots

        // If it's a bot message, randomly decide if we should process it
        if (isBot && (rand() / float(RAND_MAX)) > BOT_RESPONSE_CHANCE)
        {
            return;
        }

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
                if (!message.empty())
                {
                    // Let the original message go through first
                    // Then process the AI response in the next update
                    player->m_Events.AddEvent(
                        new AIResponseEvent(player, message, type, player->GetTeamId()),
                        player->m_Events.CalculateTime(100) // 100ms delay
                    );
                }
                break;
            }
            default:
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
        LOG_INFO("module.llm_chat", "%s", "=== LLM Chat Configuration ===");
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Enabled: %s", LLM_Config.Enabled ? "true" : "false").c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Provider: %d", LLM_Config.Provider).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Endpoint: %s", LLM_Config.OllamaEndpoint.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Model: %s", LLM_Config.OllamaModel.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Host: %s", LLM_Config.Host.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Port: %s", LLM_Config.Port.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Target: %s", LLM_Config.Target.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Log Level: %d", LLM_Config.LogLevel).c_str());
        LOG_INFO("module.llm_chat", "%s", "=== End Configuration ===\n");
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