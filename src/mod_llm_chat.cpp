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
            LLM_Config.Enabled = sConfigMgr->GetOption<bool>("LLM.Enable", false);
            LLM_Config.Provider = sConfigMgr->GetOption<int32>("LLM.Provider", 1);
            LLM_Config.OllamaEndpoint = sConfigMgr->GetOption<std::string>("LLM.Ollama.Endpoint", "http://localhost:11435/api/chat");
            LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLM.Ollama.Model", "llama3.2:3b");
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

            // Check if this is a chat type we want to respond to
            if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL && 
                type != CHAT_MSG_CHANNEL && type != CHAT_MSG_WHISPER)
                return;

            // Log the incoming message
            LLMChatLogger::Log(2, "Received chat from " + player->GetName() + ": " + msg);

            // Get response from LLM
            std::string response = QueryLLM(msg);

            // Check for invalid or empty responses
            if (response.empty() || response.find("Error") != std::string::npos) {
                response = "Sorry, I couldn't process your message.";
            }

            // Send response in chat
            if (!response.empty())
            {
                response = LLM_Config.ResponsePrefix + response;
                
                // Send response based on chat type
                switch (type)
                {
                    case CHAT_MSG_SAY:
                    {
                        // Create a new WorldPacket for the response
                        WorldPacket data(SMSG_MESSAGECHAT, 200);
                        data << uint8(CHAT_MSG_MONSTER_SAY);
                        data << uint32(LANG_UNIVERSAL);
                        ObjectGuid guid = player->GetGUID();
                        data << guid;
                        data << uint32(0);                      // Flags
                        data << guid;                           // Target GUID
                        data << uint32(response.length() + 1);  // Message length
                        data << response;                       // Message
                        data << uint8(0);                       // Chat Tag

                        // Send to all players in range
                        player->SendMessageToSetInRange(&data, LLM_Config.ChatRange, true);
                        break;
                    }
                    case CHAT_MSG_YELL:
                    {
                        // Similar to SAY but with larger range
                        WorldPacket data(SMSG_MESSAGECHAT, 200);
                        data << uint8(CHAT_MSG_MONSTER_YELL);
                        data << uint32(LANG_UNIVERSAL);
                        ObjectGuid guid = player->GetGUID();
                        data << guid;
                        data << uint32(0);
                        data << guid;
                        data << uint32(response.length() + 1);
                        data << response;
                        data << uint8(0);

                        player->SendMessageToSetInRange(&data, LLM_Config.ChatRange * 2, true);
                        break;
                    }
                    case CHAT_MSG_CHANNEL:
                    {
                        // For channels, use system message instead
                        ChatHandler(player->GetSession()).PSendSysMessage("%s", response.c_str());
                        break;
                    }
                    case CHAT_MSG_WHISPER:
                    {
                        // Direct whisper back to the player
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
                json response = json::parse(rawResponse);
                if (response.contains("response")) {
                    return response["response"];
                }
                return "Error parsing LLM response";
            }
            catch (json::parse_error const& e) {
                LLMChatLogger::Log(1, "Error parsing JSON response: " + std::string(e.what()));
                return "Error parsing response";
            }
        }

        std::string QueryLLM(std::string const& message)
        {
            try {
                std::string jsonPayload = json({
                    {"model", LLM_Config.OllamaModel},
                    {"prompt", message},
                    {"stream", false}
                }).dump();

                // Set up the IO context
                net::io_context ioc;

                // These objects perform our I/O
                tcp::resolver resolver(ioc);
                beast::tcp_stream stream(ioc);

                // Look up the domain name
                auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);

                // Make the connection on the IP address we get from a lookup
                stream.connect(results);

                // Set up an HTTP POST request message
                http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
                req.set(http::field::host, LLM_Config.Host);
                req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                req.set(http::field::content_type, "application/json");
                req.body() = jsonPayload;
                req.prepare_payload();

                // Send the HTTP request to the remote host
                http::write(stream, req);

                // This buffer is used for reading and must be persisted
                beast::flat_buffer buffer;

                // Declare a container to hold the response
                http::response<http::string_body> res;

                // Receive the HTTP response
                http::read(stream, buffer, res);

                // Gracefully close the socket
                beast::error_code ec;
                stream.socket().shutdown(tcp::socket::shutdown_both, ec);

                if (res.result() != http::status::ok) {
                    LLMChatLogger::Log(1, "HTTP error: " + std::to_string(static_cast<int>(res.result())));
                    return "Error communicating with service";
                }

                return ParseLLMResponse(res.body());
            }
            catch (std::exception const& e) {
                LLMChatLogger::Log(1, "Error in HTTP request: " + std::string(e.what()));
                return "Error communicating with service";
            }
        }
    };
}

void Add_LLMChatScripts()
{
    new LLMChat_Config();
    new LLMChatModule();
} 