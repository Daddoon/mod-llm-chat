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
#include "Playerbots.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// Forward declarations
void SendAIResponse(Player* sender, const std::string& msg, int team, uint32 originalChatType);

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
        bool EnableBotResponses;
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

std::string QueryLLM(std::string const& message)
{
    try {
        // Prepare request payload according to Ollama API spec
        std::string jsonPayload = json({
            {"model", LLM_Config.OllamaModel},
            {"prompt", message},
            {"stream", false},  // Explicitly disable streaming
            {"raw", false},     // Disable raw mode
            {"options", {
                {"temperature", 0.7},    // Add some randomness to responses
                {"num_predict", 100},    // Limit response length
                {"num_ctx", 512},        // Smaller context window for faster responses
                {"num_thread", 4},       // Use 4 threads for inference
                {"top_k", 40},          // Limit vocabulary for faster responses
                {"top_p", 0.9}          // Nucleus sampling for better quality/speed trade-off
            }}
        }).dump();

        LOG_INFO("module.llm_chat", "%s", "=== API Request ===");
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Model: %s", LLM_Config.OllamaModel.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Input: %s", message.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Full Request: %s", jsonPayload.c_str()).c_str());

        // Set up the IO context
        net::io_context ioc;

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Connecting to: %s:%s", LLM_Config.Host.c_str(), LLM_Config.Port.c_str()).c_str());

        // Make the connection on the IP address we get from a lookup
        stream.connect(results);
        LOG_INFO("module.llm_chat", "%s", "Connected to Ollama API");

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = jsonPayload;
        req.prepare_payload();

        // Send the HTTP request to the remote host
        http::write(stream, req);
        LOG_INFO("module.llm_chat", "%s", "Request sent to API");

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);
        LOG_INFO("module.llm_chat", "%s", "=== API Response ===");
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Status: %d", static_cast<int>(res.result())).c_str());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Raw Response: %s", res.body().c_str()).c_str());

        // Gracefully close the socket
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok) {
            LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("HTTP error: %d", static_cast<int>(res.result())).c_str());
            return "Error communicating with service";
        }

        std::string response = ParseLLMResponse(res.body());
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Final Processed Response: %s", response.c_str()).c_str());
        LOG_INFO("module.llm_chat", "%s", "=== End API Transaction ===\n");
        return response;
    }
    catch (std::exception const& e) {
        LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("API Error: %s", e.what()).c_str());
        return "Error communicating with service";
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

        // Check if message is from a bot
        bool isBot = player->GetSession() && player->GetSession()->IsBot();
        
        // Log the raw chat message
        std::string logMessage = Acore::StringFormat("Chat received from %s '%s' (Type: %u): '%s'", 
            isBot ? "bot" : "player",
            player->GetName().c_str(), 
            type, 
            msg.c_str());
        LOG_INFO("module.llm_chat", "%s", logMessage.c_str());

        // Only process messages from bots
        if (!isBot)
        {
            LOG_INFO("module.llm_chat", "Ignoring message from real player '%s'", player->GetName().c_str());
            return;
        }

        // Prevent response loops by checking for AI prefix
        if (msg.find(LLM_Config.ResponsePrefix) == 0)
        {
            LOG_INFO("module.llm_chat", "Ignoring AI response to prevent loops");
            return;
        }

        // Handle different chat types
        switch (type)
        {
            case CHAT_MSG_SAY:
            {
                std::string processLog = Acore::StringFormat("Processing SAY command from bot '%s': '%s'", 
                    player->GetName().c_str(),
                    msg.c_str());
                LOG_INFO("module.llm_chat", "%s", processLog.c_str());
                SendAIResponse(player, msg, -1, CHAT_MSG_SAY);
                break;
            }
            case CHAT_MSG_YELL:
            {
                std::string processLog = Acore::StringFormat("Processing YELL command from bot '%s': '%s'", 
                    player->GetName().c_str(),
                    msg.c_str());
                LOG_INFO("module.llm_chat", "%s", processLog.c_str());
                SendAIResponse(player, msg, -1, CHAT_MSG_YELL);
                break;
            }
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            {
                std::string processLog = Acore::StringFormat("Processing PARTY command from bot '%s': '%s'", 
                    player->GetName().c_str(),
                    msg.c_str());
                LOG_INFO("module.llm_chat", "%s", processLog.c_str());
                SendAIResponse(player, msg, -1, CHAT_MSG_PARTY);
                break;
            }
            case CHAT_MSG_GUILD:
            {
                std::string processLog = Acore::StringFormat("Processing GUILD command from bot '%s': '%s'", 
                    player->GetName().c_str(),
                    msg.c_str());
                LOG_INFO("module.llm_chat", "%s", processLog.c_str());
                SendAIResponse(player, msg, -1, CHAT_MSG_GUILD);
                break;
            }
            case CHAT_MSG_WHISPER:
            {
                std::string processLog = Acore::StringFormat("Processing WHISPER command from bot '%s': '%s'", 
                    player->GetName().c_str(),
                    msg.c_str());
                LOG_INFO("module.llm_chat", "%s", processLog.c_str());
                SendAIResponse(player, msg, player->GetTeamId(), CHAT_MSG_WHISPER);
                break;
            }
            default:
                break;
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
        LLM_Config.EnableBotResponses = sConfigMgr->GetOption<int32>("LLMChat.EnableBotResponses", 1) == 1;

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
        LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("EnableBotResponses: %s", LLM_Config.EnableBotResponses ? "true" : "false").c_str());
        LOG_INFO("module.llm_chat", "%s", "=== End Configuration ===\n");
    }
};

Player* GetNearbyBot(Player* player, float maxDistance)
{
    if (!player || !player->IsInWorld())
        return nullptr;

    std::vector<Player*> nearbyBots;
    Map* map = player->GetMap();
    if (!map)
        return nullptr;

    // Get nearby players using grid search
    map->DoForAllPlayers([&nearbyBots, player, maxDistance](Player* nearby) {
        if (!nearby || nearby == player)
            return;

        // Check distance
        if (player->GetDistance(nearby) > maxDistance)
            return;

        // Check if it's a playerbot
        if (nearby->GetPlayerbotAI())
        {
            nearbyBots.push_back(nearby);
        }
    });

    if (nearbyBots.empty())
        return nullptr;

    // Select random bot from available ones
    uint32 randomIndex = urand(0, nearbyBots.size() - 1);
    return nearbyBots[randomIndex];
}

void SendAIResponse(Player* sender, const std::string& msg, int team, uint32 originalChatType)
{
    std::string startLog = Acore::StringFormat("Starting AI Response - Player: '%s', Message: '%s', Type: %u", 
        sender->GetName().c_str(), 
        msg.c_str(),
        originalChatType);
    LOG_INFO("module.llm_chat", "%s", startLog.c_str());

    if (!LLM_Config.Enabled)
    {
        std::string disabledLog = Acore::StringFormat("Module is disabled for player '%s'", sender->GetName().c_str());
        LOG_INFO("module.llm_chat", "%s", disabledLog.c_str());
        return;
    }

    // Find a nearby bot to respond
    Player* respondingBot = nullptr;
    
    if (originalChatType == CHAT_MSG_SAY || originalChatType == CHAT_MSG_YELL)
    {
        // For local chat, find a nearby bot
        respondingBot = GetNearbyBot(sender, LLM_Config.ChatRange);
        if (!respondingBot)
        {
            LOG_INFO("module.llm_chat", "No nearby bots found to respond");
            return;
        }
    }
    else
    {
        // For other chat types, find any available bot
        SessionMap sessions = sWorld->GetAllSessions();
        std::vector<Player*> availableBots;

        for (SessionMap::iterator itr = sessions.begin(); itr != sessions.end(); ++itr)
        {
            if (!itr->second || !itr->second->GetPlayer() || !itr->second->GetPlayer()->IsInWorld())
                continue;

            Player* potentialBot = itr->second->GetPlayer();
            if (potentialBot->GetPlayerbotAI())
            {
                // For party chat, check if in same group
                if (originalChatType == CHAT_MSG_PARTY && 
                    (!potentialBot->GetGroup() || potentialBot->GetGroup() != sender->GetGroup()))
                    continue;

                // For guild chat, check if in same guild
                if (originalChatType == CHAT_MSG_GUILD && 
                    (!potentialBot->GetGuild() || potentialBot->GetGuild() != sender->GetGuild()))
                    continue;

                availableBots.push_back(potentialBot);
            }
        }

        if (!availableBots.empty())
        {
            uint32 randomIndex = urand(0, availableBots.size() - 1);
            respondingBot = availableBots[randomIndex];
        }
    }

    if (!respondingBot)
    {
        LOG_INFO("module.llm_chat", "No eligible bots found to respond");
        return;
    }

    LOG_INFO("module.llm_chat", "Selected bot '%s' to respond", respondingBot->GetName().c_str());

    // Get AI response
    std::string response = QueryLLM(msg);
    if (response.empty() || response.find("Error") != std::string::npos)
    {
        LOG_INFO("module.llm_chat", "Error getting LLM response");
        return;
    }

    // Remove AI prefix if present (since bot will be speaking directly)
    if (response.find(LLM_Config.ResponsePrefix) == 0)
    {
        response = response.substr(LLM_Config.ResponsePrefix.length());
    }

    // Have the bot send the message using PlayerbotAI
    PlayerbotAI* botAI = respondingBot->GetPlayerbotAI();
    if (!botAI)
    {
        LOG_INFO("module.llm_chat", "Selected bot has no AI");
        return;
    }

    switch (originalChatType)
    {
        case CHAT_MSG_SAY:
            botAI->TellPlayer(sender, response, CHAT_MSG_SAY);
            LOG_INFO("module.llm_chat", "Bot '%s' says: %s", respondingBot->GetName().c_str(), response.c_str());
            break;
            
        case CHAT_MSG_YELL:
            botAI->TellPlayer(sender, response, CHAT_MSG_YELL);
            LOG_INFO("module.llm_chat", "Bot '%s' yells: %s", respondingBot->GetName().c_str(), response.c_str());
            break;
            
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            if (respondingBot->GetGroup() && respondingBot->GetGroup() == sender->GetGroup())
            {
                botAI->TellPlayer(sender, response, CHAT_MSG_PARTY);
                LOG_INFO("module.llm_chat", "Bot '%s' says to party: %s", respondingBot->GetName().c_str(), response.c_str());
            }
            break;
            
        case CHAT_MSG_GUILD:
            if (respondingBot->GetGuild() && respondingBot->GetGuild() == sender->GetGuild())
            {
                botAI->TellPlayer(sender, response, CHAT_MSG_GUILD);
                LOG_INFO("module.llm_chat", "Bot '%s' says to guild: %s", respondingBot->GetName().c_str(), response.c_str());
            }
            break;
            
        case CHAT_MSG_WHISPER:
            botAI->TellPlayer(sender, response, CHAT_MSG_WHISPER);
            LOG_INFO("module.llm_chat", "Bot '%s' whispers to %s: %s", respondingBot->GetName().c_str(), sender->GetName().c_str(), response.c_str());
            break;
            
        default:
            botAI->TellPlayer(sender, response);
            LOG_INFO("module.llm_chat", "Bot '%s' sends message: %s", respondingBot->GetName().c_str(), response.c_str());
            break;
    }

    LOG_INFO("module.llm_chat", "Finished sending bot response\n");
}

void Add_LLMChatScripts()
{
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatModule();
} 