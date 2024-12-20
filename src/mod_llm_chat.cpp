/*
** Made by Krazor
** AzerothCore 2019 http://www.azerothcore.org/
** Based on LLM Chat integration
*/

#include "ScriptMgr.h"
#include "Player.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "Util.h"
#include "Log.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <cpprest/http_client.h>
#include <cpprest/json.h>

using json = nlohmann::json;
using namespace web; // Common features like URIs and JSON
using namespace web::http; // Common HTTP functionality
using namespace web::http::client; // HTTP client features

/* VERSION */
float ver = 1.0f;

/* Config Variables */
struct LLMConfig
{
    bool Enabled;
    int Provider;
    std::string OllamaEndpoint;
    std::string OllamaModel;
    float ChatRange;
    std::string ResponsePrefix;
};

LLMConfig LLM_Config;

class LLMChat_Config : public WorldScript
{
public: 
    LLMChat_Config() : WorldScript("LLMChat_Config") { }
    
    void OnBeforeConfigLoad(bool reload) override
    {
        if (!reload) {
            LLM_Config.Enabled = sConfigMgr->GetOption<bool>("LLM.Enable", false);
            LLM_Config.Provider = sConfigMgr->GetOption<int>("LLM.Provider", 1);
            LLM_Config.OllamaEndpoint = sConfigMgr->GetOption<std::string>("LLM.Ollama.Endpoint", "http://localhost:11435/api/chat");
            LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLM.Ollama.Model", "llama3.2:3b");
            LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLM.ChatRange", 25.0f);
            LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLM.ResponsePrefix", "[AI] ");
        }
    }
};

class LLMChatLogger {
public:
    static void Log(std::string const& message) {
        LOG_INFO("chat.ai", "%s", message.c_str());
    }

    static void LogChat(std::string const& playerName, std::string const& input, std::string const& response) {
        LOG_INFO("chat.ai", "Player: %s, Input: %s", playerName.c_str(), input.c_str());
        LOG_INFO("chat.ai", "AI Response: %s", response.c_str());
    }
};

class LLMChatModule : public PlayerScript
{
public:
    LLMChatModule() : PlayerScript("LLMChatModule") {}

    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg) override
    {
        if (!LLM_Config.Enabled || !player || msg.empty())
            return;

        // Log the incoming message
        LLMChatLogger::Log("Received chat from " + player->GetName() + ": " + msg);

        // Get response from LLM
        std::string response = QueryLLM(msg);

        // Check for invalid or empty responses
        if (response.empty() || response.find("Error") != std::string::npos) {
            response = "[AI] Sorry, I couldn't process your message.";
        }

        // Send response in chat
        if (!response.empty())
        {
            response = LLM_Config.ResponsePrefix + response;
            WorldPacket data(SMSG_MESSAGECHAT, 200);
            data << uint8(CHAT_MSG_SAY);
            data << uint32(LANG_UNIVERSAL);
            data << player->GetGUID().GetRawValue();
            data << uint32(0);
            data << player->GetGUID().GetRawValue();
            data << uint32(response.length() + 1);
            data << response;
            data << uint8(0);
            player->SendMessageToSetInRange(&data, LLM_Config.ChatRange, true);
        }
    }

private:
    std::string QueryLLM(const std::string& message)
    {
        // Create an HTTP client
        http_client client(LLM_Config.OllamaEndpoint);

        // Create a JSON object for the request
        json::value requestData;
        requestData[U("model")] = json::value::string(LLM_Config.OllamaModel);
        requestData[U("messages")] = json::value::array();
        requestData[U("messages")][0][U("role")] = json::value::string(U("user"));
        requestData[U("messages")][0][U("content")] = json::value::string(message);

        // Send the request
        client.request(methods::POST, U(""), requestData.serialize(), U("application/json"))
            .then([](http_response response) {
                if (response.status_code() == status_codes::OK) {
                    return response.extract_json();
                }
                throw std::runtime_error("Error: " + std::to_string(response.status_code()));
            })
            .then([this](json::value jsonResponse) {
                return ParseLLMResponse(jsonResponse);
            })
            .wait(); // Wait for the response

        return "Response received"; // Placeholder, handle the actual response
    }

    std::string ParseLLMResponse(const std::string& rawResponse)
    {
        try {
            json response = json::parse(rawResponse);
            if (response.contains("message") && response["message"].contains("content")) {
                return response["message"]["content"];
            }
            return "Error parsing LLM response";
        }
        catch (const json::parse_error& e) {
            return "Error parsing JSON response: " + std::string(e.what());
        }
    }
};

void Add_LLMChatScripts()
{
    new LLMChat_Config();
    new LLMChatModule();
} 