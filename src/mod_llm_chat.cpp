/*
<--------------------------------------------------------------------------->
- Developer(s): Your Name
- Complete: 100%
- ScriptName: 'LLM Chat'
- Comment: AI Chat integration using Ollama
<--------------------------------------------------------------------------->
*/

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Util.h"
#include "Log.h"
#include <curl/curl.h>
#include <string>
#include <regex>
#include <nlohmann/json.hpp>

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
private:
    bool enabled;
    int provider;
    std::string ollamaEndpoint;
    std::string ollamaModel;
    std::string lmStudioEndpoint;
    float chatRange;
    std::string responsePrefix;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp)
    {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string ParseLLMResponse(const std::string& rawResponse, int providerType)
    {
        try {
            json response = json::parse(rawResponse);

            if (providerType == 1) // Ollama
            {
                // Ollama format: {"message": {"content": "response text"}}
                if (response.contains("message") && response["message"].contains("content"))
                {
                    return response["message"]["content"];
                }
            }
            else if (providerType == 2) // LM Studio (OpenAI format)
            {
                // OpenAI format: {"choices":[{"message":{"content":"response text"}}]}
                if (response.contains("choices") && !response["choices"].empty())
                {
                    if (response["choices"][0].contains("message") &&
                        response["choices"][0]["message"].contains("content"))
                    {
                        return response["choices"][0]["message"]["content"];
                    }
                }
            }
            return "Error parsing LLM response";
        }
        catch (const json::parse_error& e) {
            return "Error parsing JSON response: " + std::string(e.what());
        }
    }

    std::string QueryLLM(const std::string& message)
    {
        CURL* curl = curl_easy_init();
        std::string response;

        if (!curl)
            return "Error initializing CURL";

        std::string jsonPayload;
        std::string endpoint;

        // Prepare request based on provider
        if (provider == 1) // Ollama
        {
            jsonPayload = json({
                {"model", ollamaModel},
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", message}
                    }
                })}
            }).dump();
            endpoint = ollamaEndpoint;
        }
        else if (provider == 2) // LM Studio
        {
            jsonPayload = json({
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", message}
                    }
                })},
                {"temperature", 0.7},
                {"max_tokens", 500}
            }).dump();
            endpoint = lmStudioEndpoint;
        }
        else
        {
            return "Invalid LLM provider configured";
        }

        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
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
            return "Error communicating with LLM service: " + errorMsg;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Parse the response
        return ParseLLMResponse(response, provider);
    }

public:
    LLMChatModule() : PlayerScript("LLMChatModule") 
    {
        enabled = sConfigMgr->GetOption<bool>("LLM.Enable", false);
        provider = sConfigMgr->GetOption<int>("LLM.Provider", 1);
        ollamaEndpoint = sConfigMgr->GetOption<std::string>("LLM.Ollama.Endpoint", "http://localhost:11435/api/chat");
        ollamaModel = sConfigMgr->GetOption<std::string>("LLM.Ollama.Model", "llama3.2:3b");
        lmStudioEndpoint = sConfigMgr->GetOption<std::string>("LLM.LMStudio.Endpoint", "http://localhost:8080/v1/chat/completions");
        chatRange = sConfigMgr->GetOption<float>("LLM.ChatRange", 25.0f);
        responsePrefix = sConfigMgr->GetOption<std::string>("LLM.ResponsePrefix", "[AI] ");
    }

    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg) override
    {
        if (!enabled || !player || msg.empty())
            return;

        // Log the incoming message
        std::ostringstream logStream;
        logStream << "Received chat from " << player->GetName() << ": " << msg;
        LLMChatLogger::Log(logStream.str());

        // Get response from LLM
        std::string response = QueryLLM(msg);


        // Check for invalid or empty responses
        if (response.empty() || response.find("Error") != std::string::npos) {
            response = "[AI] Sorry, I couldn't process your message.";
        }

        // Log the AI response
        LLMChatLogger::LogChat(player->GetName(), msg, response);

        // Send response in chat
        if (!response.empty())
        {
            response = responsePrefix + response;

            // Create a custom chat packet for the response
            WorldPacket data(SMSG_MESSAGECHAT, 200);
            data << uint8(CHAT_MSG_SAY);          // Chat type
            data << uint32(LANG_UNIVERSAL);       // Language
            data << player->GetGUID().GetRawValue();    // Sender GUID
            data << uint32(0);                    // Chat flags
            data << player->GetGUID().GetRawValue();    // Sender GUID again
            data << uint32(response.length() + 1);// Message length + 1
            data << response;                     // Message
            data << uint8(0);                     // Chat tag

            // Send to nearby players using configured chat range
            player->SendMessageToSetInRange(&data, chatRange, true);
        }
    }

};

class LLMChat_Announce : public PlayerScript
{
public:
    LLMChat_Announce() : PlayerScript("LLMChat_Announce") { }

    void OnLogin(Player* player) override
    {
        if (LLM_Config.Enabled)
        {
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the LLM Chat module. Chat with AI using normal chat.");
        }
    }
};

void AddSC_LLMChatScripts()
{
    new LLMChat_Config();
    new LLMChatModule();
    new LLMChat_Announce();
} 