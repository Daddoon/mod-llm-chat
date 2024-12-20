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
#include "mod_llm_chat.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>

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
    };

    LLMConfig LLM_Config;

    class LLMChat_Config : public WorldScript
    {
    public:
        LLMChat_Config() : WorldScript("LLMChat_Config") { }

        void OnConfigLoad(bool reload) override
        {
            LLM_Config.Enabled = sConfigMgr->GetOption<bool>("LLM.Enable", false);
            LLM_Config.Provider = sConfigMgr->GetOption<int32>("LLM.Provider", 1);
            LLM_Config.OllamaEndpoint = sConfigMgr->GetOption<std::string>("LLM.Ollama.Endpoint", "http://localhost:11435/api/chat");
            LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLM.Ollama.Model", "llama3.2:3b");
            LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLM.ChatRange", 25.0f);
            LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLM.ResponsePrefix", "[AI] ");
            LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLM.LogLevel", 2);
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

        void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg) override
        {
            if (!LLM_Config.Enabled || !player || msg.empty())
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
                ChatHandler(player->GetSession()).PSendSysMessage("%s", response.c_str());
                
                // Log the interaction
                LLMChatLogger::LogChat(player->GetName(), msg, response);
            }
        }

    private:
        static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp)
        {
            userp->append((char*)contents, size * nmemb);
            return size * nmemb;
        }

        std::string ParseLLMResponse(std::string const& rawResponse)
        {
            try {
                json response = json::parse(rawResponse);
                if (response.contains("message") && response["message"].contains("content")) {
                    return response["message"]["content"];
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
            CURL* curl = curl_easy_init();
            if (!curl) {
                LLMChatLogger::Log(1, "Failed to initialize CURL");
                return "Error initializing service";
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

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            CURLcode res = curl_easy_perform(curl);

            if (res != CURLE_OK) {
                std::string errorMsg = curl_easy_strerror(res);
                LLMChatLogger::Log(1, "CURL error: " + errorMsg);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return "Error communicating with service";
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            return ParseLLMResponse(response);
        }
    };
}

void Add_LLMChatScripts()
{
    new LLMChat_Config();
    new LLMChatModule();
} 