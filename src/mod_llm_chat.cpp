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

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// Forward declarations
void SendAIResponse(Player* sender, const std::string& msg, int team);

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
            LOG_ERROR("module.llm_chat", "Invalid endpoint URL (no protocol): %s", endpoint.c_str());
            return;
        }

        std::string url = endpoint.substr(protocolEnd + 3);
        size_t pathStart = url.find('/');
        if (pathStart == std::string::npos) {
            LOG_ERROR("module.llm_chat", "Invalid endpoint URL (no path): %s", endpoint.c_str());
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
                    LOG_ERROR("module.llm_chat", "Invalid port number: %s", config.Port.c_str());
                    config.Port = "11434"; // Default to Ollama port
                }
            } catch (std::exception const& e) {
                LOG_ERROR("module.llm_chat", "Invalid port format: %s", e.what());
                config.Port = "11434"; // Default to Ollama port
            }
        } else {
            config.Host = hostPort;
            config.Port = "11434"; // Default to Ollama port
        }

        LOG_INFO("module.llm_chat", "URL parsed successfully - Host: %s, Port: %s, Target: %s", 
                config.Host.c_str(), config.Port.c_str(), config.Target.c_str());
    }
    catch (std::exception const& e) {
        LOG_ERROR("module.llm_chat", "Error parsing URL: %s", e.what());
        // Set defaults
        config.Host = "localhost";
        config.Port = "11434";
        config.Target = "/api/generate";
    }
}

std::string ParseLLMResponse(std::string const& rawResponse)
{
    try {
        LOG_INFO("module.llm_chat", "Parsing API Response...");
        json response = json::parse(rawResponse);
        
        // Check if this is a streaming response
        if (response.contains("done")) {
            LOG_INFO("module.llm_chat", "Found streaming response");
            // This is a streaming response
            if (response["done"].get<bool>()) {
                // This is the final response in the stream
                std::string result = response["response"].get<std::string>();
                LOG_INFO("module.llm_chat", "Final stream response: %s", result.c_str());
                return result;
            }
            LOG_INFO("module.llm_chat", "Partial stream response - ignoring");
            // This is a partial response, ignore it
            return "";
        }
        
        // Non-streaming response
        if (response.contains("response")) {
            std::string aiResponse = response["response"].get<std::string>();
            LOG_INFO("module.llm_chat", "Non-streaming response: %s", aiResponse.c_str());
            return aiResponse;
        }
        
        LOG_ERROR("module.llm_chat", "Response missing 'response' field: %s", rawResponse.c_str());
        return "Error parsing LLM response";
    }
    catch (json::parse_error const& e) {
        LOG_ERROR("module.llm_chat", "JSON parse error: %s", e.what());
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
            {"options", {
                {"temperature", 0.7},    // Add some randomness to responses
                {"num_predict", 100},    // Limit response length
                {"num_ctx", 512},        // Smaller context window for faster responses
                {"num_thread", 4},       // Use 4 threads for inference
                {"top_k", 40},          // Limit vocabulary for faster responses
                {"top_p", 0.9}          // Nucleus sampling for better quality/speed trade-off
            }}
        }).dump();

        LOG_INFO("module.llm_chat", "=== API Request ===");
        LOG_INFO("module.llm_chat", "Model: %s", LLM_Config.OllamaModel.c_str());
        LOG_INFO("module.llm_chat", "Input: %s", message.c_str());
        LOG_INFO("module.llm_chat", "Full Request: %s", jsonPayload.c_str());

        // Set up the IO context
        net::io_context ioc;

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
        LOG_INFO("module.llm_chat", "Connecting to: %s:%s", LLM_Config.Host.c_str(), LLM_Config.Port.c_str());

        // Make the connection on the IP address we get from a lookup
        stream.connect(results);
        LOG_INFO("module.llm_chat", "Connected to Ollama API");

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = jsonPayload;
        req.prepare_payload();

        // Send the HTTP request to the remote host
        http::write(stream, req);
        LOG_INFO("module.llm_chat", "Request sent to API");

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);
        LOG_INFO("module.llm_chat", "=== API Response ===");
        LOG_INFO("module.llm_chat", "Status: %d", static_cast<int>(res.result()));
        LOG_INFO("module.llm_chat", "Raw Response: %s", res.body().c_str());

        // Gracefully close the socket
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok) {
            LOG_ERROR("module.llm_chat", "HTTP error: %d", static_cast<int>(res.result()));
            return "Error communicating with service";
        }

        std::string response = ParseLLMResponse(res.body());
        LOG_INFO("module.llm_chat", "Final Processed Response: %s", response.c_str());
        LOG_INFO("module.llm_chat", "=== End API Transaction ===\n");
        return response;
    }
    catch (std::exception const& e) {
        LOG_ERROR("module.llm_chat", "API Error: %s", e.what());
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
            LOG_INFO("module.llm_chat", "Player: %s, Input: %s", playerName.c_str(), input.c_str());
            LOG_INFO("module.llm_chat", "AI Response: %s", response.c_str());
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
            return;

        // Ignore if player is a bot or not a real player
        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        LOG_INFO("module.llm_chat", "OnChat triggered - Type: %u, Message: %s", type, msg.c_str());

        switch (type)
        {
            case CHAT_MSG_SAY:
            case CHAT_MSG_YELL:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_GUILD:
            case CHAT_MSG_OFFICER:
            case CHAT_MSG_RAID:
            case CHAT_MSG_RAID_LEADER:
            case CHAT_MSG_RAID_WARNING:
            case CHAT_MSG_BATTLEGROUND:
            case CHAT_MSG_BATTLEGROUND_LEADER:
            case CHAT_MSG_CHANNEL:
                SendAIResponse(player, msg, -1);
                break;
            case CHAT_MSG_WHISPER:
                SendAIResponse(player, msg, player->GetTeamId());
                break;
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
        LOG_INFO("module.llm_chat", "Log Level: %d", LLM_Config.LogLevel);
        LOG_INFO("module.llm_chat", "=== End Configuration ===\n");
    }
};

void SendAIResponse(Player* sender, const std::string& msg, int team)
{
    if (!LLM_Config.Enabled)
    {
        ChatHandler(sender->GetSession()).PSendSysMessage("LLM Chat is disabled.");
        return;
    }

    if (!sender->CanSpeak())
    {
        ChatHandler(sender->GetSession()).PSendSysMessage("You can't use LLM Chat while muted!");
        return;
    }

    std::string response = QueryLLM(msg);
    if (response.empty() || response.find("Error") != std::string::npos)
    {
        response = "Sorry, I couldn't process your message.";
    }

    response = LLM_Config.ResponsePrefix + response;

    SessionMap sessions = sWorld->GetAllSessions();
    for (SessionMap::iterator itr = sessions.begin(); itr != sessions.end(); ++itr)
    {
        if (!itr->second || !itr->second->GetPlayer() || !itr->second->GetPlayer()->IsInWorld())
            continue;

        Player* target = itr->second->GetPlayer();

        // Check team if specified
        if (team != -1 && target->GetTeamId() != team)
            continue;

        // Check range for local chat types
        if ((msg.find("/s") == 0 || msg.find("/y") == 0) && 
            target->GetDistance(sender) > LLM_Config.ChatRange)
            continue;

        std::string message = Acore::StringFormat("[AI][%s]: %s", 
            sender->GetName().c_str(), 
            response.c_str());

        ChatHandler(target->GetSession()).PSendSysMessage("%s", message.c_str());
    }

    // Log the interaction
    LOG_INFO("module.llm_chat", "Player: %s, Input: %s", sender->GetName().c_str(), msg.c_str());
    LOG_INFO("module.llm_chat", "AI Response: %s", response.c_str());
}

void Add_LLMChatScripts()
{
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatModule();
} 