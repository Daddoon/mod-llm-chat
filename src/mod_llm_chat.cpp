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
        // Add host and port for proper URL parsing
        std::string Host;
        std::string Port;
        std::string Target;
    };

    LLMConfig LLM_Config;

    class LLMChat_Config : public WorldScript
    {
    public:
        LLMChat_Config() : WorldScript("LLMChat_Config") { }

        void OnBeforeConfigLoad(bool /*reload*/) override
        {
            LLM_Config.Enabled = sConfigMgr->GetOption<int32>("LLM.Enable", 0) == 1;
            LLM_Config.Provider = sConfigMgr->GetOption<int32>("LLM.Provider", 1);
            LLM_Config.OllamaEndpoint = sConfigMgr->GetOption<std::string>("LLM.Ollama.Endpoint", "http://localhost:11434/api/generate");
            LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLM.Ollama.Model", "llama3.2:1b");
            LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLM.ChatRange", 25.0f);
            LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLM.ResponsePrefix", "[AI] ");
            LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLM.LogLevel", 2);

            // Parse endpoint URL
            size_t protocolEnd = LLM_Config.OllamaEndpoint.find("://");
            if (protocolEnd != std::string::npos) {
                std::string url = LLM_Config.OllamaEndpoint.substr(protocolEnd + 3);
                size_t pathStart = url.find('/');
                std::string hostPort = url.substr(0, pathStart);
                LLM_Config.Target = url.substr(pathStart);

                size_t portStart = hostPort.find(':');
                if (portStart != std::string::npos) {
                    LLM_Config.Host = hostPort.substr(0, portStart);
                    LLM_Config.Port = hostPort.substr(portStart + 1);
                } else {
                    LLM_Config.Host = hostPort;
                    LLM_Config.Port = "80";
                }
            }
        }
    };

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

        void OnChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
        {
            // Only process certain chat types
            if (!LLM_Config.Enabled || !player || msg.empty())
                return;

            // Ignore if player is a bot or not a real player
            if (!player->GetSession() || player->GetSession()->IsBot() || !player->GetSession()->GetPlayer())
                return;

            // Log the incoming message
            LLMChatLogger::Log(2, "Received chat from " + player->GetName() + ": " + msg);

            // Get response from LLM
            std::string response = QueryLLM(msg);
            LLMChatLogger::Log(2, "Got response from QueryLLM: " + response);

            // Check for invalid or empty responses
            if (response.empty() || response.find("Error") != std::string::npos) {
                response = "Sorry, I couldn't process your message.";
                LLMChatLogger::Log(1, "Using error response: " + response);
            }

            // Send response in chat
            if (!response.empty())
            {
                response = LLM_Config.ResponsePrefix + response;
                LLMChatLogger::Log(2, "Sending final response: " + response);

                switch (type)
                {
                    case CHAT_MSG_SAY:
                    {
                        // Local chat - use range-based response
                        WorldPacket data(SMSG_MESSAGECHAT, 200);
                        data << uint8(CHAT_MSG_SAY);  // Use normal SAY instead of monster say
                        data << uint32(LANG_UNIVERSAL);
                        ObjectGuid guid = player->GetGUID();
                        data << guid;  // Source GUID
                        data << uint32(0);  // Chat flags
                        data << guid;  // Target GUID (same as source for say)
                        data << uint32(response.length() + 1);
                        data << response;
                        data << uint8(0);  // Chat tag

                        player->SendMessageToSetInRange(&data, LLM_Config.ChatRange, true);
                        LLMChatLogger::Log(2, "Sent local say response");
                        break;
                    }
                    case CHAT_MSG_YELL:
                    {
                        // Yell chat - use extended range
                        WorldPacket data(SMSG_MESSAGECHAT, 200);
                        data << uint8(CHAT_MSG_YELL);  // Use normal YELL instead of monster yell
                        data << uint32(LANG_UNIVERSAL);
                        ObjectGuid guid = player->GetGUID();
                        data << guid;
                        data << uint32(0);
                        data << guid;
                        data << uint32(response.length() + 1);
                        data << response;
                        data << uint8(0);

                        player->SendMessageToSetInRange(&data, LLM_Config.ChatRange * 2, true);
                        LLMChatLogger::Log(2, "Sent yell response");
                        break;
                    }
                    case CHAT_MSG_PARTY:
                    case CHAT_MSG_PARTY_LEADER:
                    {
                        // Party chat - send to all party members
                        if (Group* group = player->GetGroup())
                        {
                            WorldPacket data(SMSG_MESSAGECHAT, 200);
                            data << uint8(type);  // Keep original party chat type
                            data << uint32(LANG_UNIVERSAL);
                            ObjectGuid guid = player->GetGUID();
                            data << guid;
                            data << uint32(0);
                            data << guid;
                            data << uint32(response.length() + 1);
                            data << response;
                            data << uint8(0);

                            group->BroadcastPacket(&data, false);
                            LLMChatLogger::Log(2, "Sent party response");
                        }
                        break;
                    }
                    case CHAT_MSG_GUILD:
                    case CHAT_MSG_OFFICER:
                    {
                        // Guild chat - send to all guild members
                        if (Guild* guild = player->GetGuild())
                        {
                            WorldPacket data(SMSG_MESSAGECHAT, 200);
                            data << uint8(type);  // Keep original guild chat type
                            data << uint32(LANG_UNIVERSAL);
                            ObjectGuid guid = player->GetGUID();
                            data << guid;
                            data << uint32(0);
                            data << guid;
                            data << uint32(response.length() + 1);
                            data << response;
                            data << uint8(0);

                            guild->BroadcastPacket(&data);
                            LLMChatLogger::Log(2, "Sent guild response");
                        }
                        break;
                    }
                    case CHAT_MSG_CHANNEL:
                    {
                        // Channel chat - respond in the same channel
                        if (ChannelMgr* cMgr = ChannelMgr::forTeam(player->GetTeam()))
                        {
                            // Get the channel name from the message
                            std::string channelName = "Trade"; // Default to trade
                            if (Channel* chn = cMgr->GetChannel(channelName, player))
                            {
                                WorldPacket data(SMSG_MESSAGECHAT, 200);
                                data << uint8(CHAT_MSG_CHANNEL);
                                data << uint32(LANG_UNIVERSAL);
                                ObjectGuid guid = player->GetGUID();
                                data << guid;
                                data << uint32(0);
                                data << channelName.c_str();  // Channel name
                                data << guid;
                                data << uint32(response.length() + 1);
                                data << response;
                                data << uint8(0);

                                chn->SendToAll(&data);
                                LLMChatLogger::Log(2, "Sent channel response to " + channelName);
                            }
                        }
                        break;
                    }
                    case CHAT_MSG_WHISPER:
                    {
                        // Whisper - respond directly to the player
                        WorldPacket data(SMSG_MESSAGECHAT, 200);
                        data << uint8(CHAT_MSG_WHISPER);
                        data << uint32(LANG_UNIVERSAL);
                        ObjectGuid guid = player->GetGUID();
                        data << guid;
                        data << uint32(0);
                        data << guid;
                        data << uint32(response.length() + 1);
                        data << response;
                        data << uint8(0);
                        player->GetSession()->SendPacket(&data);
                        LLMChatLogger::Log(2, "Sent whisper response");
                        break;
                    }
                }
                
                // Log the interaction
                LLMChatLogger::LogChat(player->GetName(), msg, response);
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
    };
}

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
        LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLMChat.LogLevel", 2);

        // Parse endpoint URL
        size_t protocolEnd = LLM_Config.OllamaEndpoint.find("://");
        if (protocolEnd != std::string::npos) {
            std::string url = LLM_Config.OllamaEndpoint.substr(protocolEnd + 3);
            size_t pathStart = url.find('/');
            std::string hostPort = url.substr(0, pathStart);
            LLM_Config.Target = url.substr(pathStart);

            size_t portStart = hostPort.find(':');
            if (portStart != std::string::npos) {
                LLM_Config.Host = hostPort.substr(0, portStart);
                LLM_Config.Port = hostPort.substr(portStart + 1);
            } else {
                LLM_Config.Host = hostPort;
                LLM_Config.Port = "80";
            }
        }
    }
};

void Add_LLMChatScripts()
{
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatModule();
} 