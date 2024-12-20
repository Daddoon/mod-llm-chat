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

using json = nlohmann::json;

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
        CURL* curl = curl_easy_init();
        if (!curl) {
            return "Error initializing CURL";
        }

        std::string response;
        std::string jsonPayload = json({
            {"model", LLM_Config.OllamaModel},
            {"messages", json::array({
                {
                    {"role", "user"},
                    {"content", message}
                }
            })}
        }).dump();

        curl_easy_setopt(curl, CURLOPT_URL, LLM_Config.OllamaEndpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            std::string errorMsg = curl_easy_strerror(res);
            LOG_ERROR("server.loading", "LLM Chat Module: CURL error: {}", errorMsg);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return "Error communicating with LLM service: " + errorMsg;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Parse the response
        return ParseLLMResponse(response);
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp)
    {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
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

void AddLLMChatScripts()
{
    new LLMChat_Config();
    new LLMChatModule();
} 