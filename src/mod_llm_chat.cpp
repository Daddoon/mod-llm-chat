/*
** Made by Krazor
** AzerothCore 2019 http://www.azerothcore.org/
** Based on LLM Chat integration
*/

#include <fstream>
#include <random>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "World.h"
#include "Channel.h"
#include "Guild.h"
#include "ChannelMgr.h"
#include "mod_llm_chat.h"
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
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team);
std::string QueryLLM(std::string const& message, const std::string& playerName);
class BotResponseEvent;
class RemovePacifiedEvent;
class TriggerResponseEvent;

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
    // Configuration options
    uint32 MaxResponsesPerMessage;
    uint32 MaxConversationRounds;
    uint32 ResponseChance;
    // LLM Parameters
    float Temperature;
    float TopP;
    uint32 NumPredict;
    uint32 ContextSize;
    float RepeatPenalty;
    std::string PersonalityFile;
};

LLMConfig LLM_Config;

// Add a conversation counter
std::map<std::string, uint32> conversationRounds;  // Key: channelName+originalMessage, Value: round count
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

// Personality structure
struct Personality {
    std::string id;
    std::string name;
    std::string prompt;
    std::vector<std::string> emotions;
    nlohmann::json traits;
    std::vector<std::string> interests;
    nlohmann::json chat_style;
};

// Global variables
std::vector<Personality> g_personalities;
nlohmann::json g_emotion_types;

// Load personalities from JSON file
bool LoadPersonalities(std::string const& filename) {
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            LOG_ERROR("module.llm_chat", "Failed to open personality file: {}", filename);
            return false;
        }

        nlohmann::json json_data;
        file >> json_data;

        // Load personalities
        g_personalities.clear();
        for (const auto& p : json_data["personalities"]) {
            Personality personality;
            personality.id = p["id"];
            personality.name = p["name"];
            personality.prompt = p["prompt"];
            personality.emotions = p["emotions"].get<std::vector<std::string>>();
            personality.traits = p["traits"];
            personality.interests = p["interests"].get<std::vector<std::string>>();
            personality.chat_style = p["chat_style"];
            g_personalities.push_back(personality);
        }

        // Load emotion types
        g_emotion_types = json_data["emotion_types"];

        LOG_INFO("module.llm_chat", "Loaded {} personalities from {}", g_personalities.size(), filename);
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("module.llm_chat", "Error loading personalities: {}", e.what());
        return false;
    }
}

// Function to select appropriate personality based on emotion
Personality SelectPersonality(const std::string& emotion) {
    std::vector<Personality> matchingPersonalities;
    
    // Find personalities that handle this emotion well
    for (const auto& personality : g_personalities) {
        if (std::find(personality.emotions.begin(), 
                      personality.emotions.end(), 
                      emotion) != personality.emotions.end()) {
            matchingPersonalities.push_back(personality);
        }
    }
    
    // If no matching personalities, use all personalities
    if (matchingPersonalities.empty()) {
        matchingPersonalities = g_personalities;
    }
    
    // Select random personality from matches
    return matchingPersonalities[urand(0, matchingPersonalities.size() - 1)];
}

// Function to detect emotion from message
std::string DetectEmotion(const std::string& message) {
    // Convert message to lowercase for comparison
    std::string lowerMsg = message;
    std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);

    // Count emotion keywords from emotion types
    std::map<std::string, int> emotionScores;
    
    for (const auto& [emotion, data] : g_emotion_types.items()) {
        int score = 0;
        for (const auto& phrase : data["typical_phrases"]) {
            std::string keyword = phrase.get<std::string>();
            size_t pos = 0;
            while ((pos = lowerMsg.find(keyword, pos)) != std::string::npos) {
                score++;
                pos += keyword.length();
            }
        }
        emotionScores[emotion] = score;
    }

    // Find emotion with highest score
    std::string dominantEmotion = "Friendly"; // Default
    int maxScore = 0;
    
    for (const auto& [emotion, score] : emotionScores) {
        if (score > maxScore) {
            maxScore = score;
            dominantEmotion = emotion;
        }
    }

    LOG_DEBUG("module.llm_chat", "Detected emotion '%s' for message: %s", 
        dominantEmotion.c_str(), message.c_str());
    return dominantEmotion;
}

// Modify QueryLLM to use personality system
std::string QueryLLM(std::string const& message, const std::string& playerName)
{
    if (message.empty() || playerName.empty())
    {
        LOG_ERROR("module.llm_chat", "Empty message or player name");
        return "Error: Invalid input";
    }

    try {
        // Detect emotion and select appropriate personality
        std::string emotion = DetectEmotion(message);
        Personality personality = SelectPersonality(emotion);
        
        LOG_DEBUG("module.llm_chat", "Detected emotion: %s, Selected personality: %s", 
                 emotion.c_str(), personality.name.c_str());

        // Create context with selected personality
        std::string contextPrompt = 
            personality.prompt + "\n\n"
            "Additional guidelines:\n"
            "- You are a real person playing WoW who can discuss any topic naturally\n"
            "- Feel free to share personal opinions and experiences beyond just the game\n"
            "- You can discuss real-world topics, jokes, or general conversation\n"
            "- Don't force every response to be about WoW or lore\n"
            "- Be natural and casual, like a real player chatting while gaming\n"
            "- You can reference pop culture, current events, or other games\n"
            "- If referring to the player, use their name: " + playerName + "\n\n"
            "Chat naturally with " + playerName + " about: " + message;

        LOG_DEBUG("module.llm_chat", "Context prompt: %s", contextPrompt.c_str());

        // Prepare request payload with emotion-adjusted parameters
        json requestJson = {
            {"model", LLM_Config.OllamaModel},
            {"prompt", contextPrompt},
            {"stream", false},
            {"options", {
                {"temperature", LLM_Config.Temperature},
                {"num_predict", LLM_Config.NumPredict},
                {"num_ctx", LLM_Config.ContextSize},
                {"num_thread", std::thread::hardware_concurrency()},
                {"top_k", 40},
                {"top_p", LLM_Config.TopP},
                {"repeat_penalty", LLM_Config.RepeatPenalty},
                {"stop", {"\n\n", "Human:", "Assistant:", "[", "<"}}
            }}
        };

        // Adjust parameters based on emotion
        if (emotion == "Aggressive" || emotion == "Excited") {
            requestJson["options"]["temperature"] = 0.8; // More varied responses
            requestJson["options"]["top_p"] = 0.8;      // More creative
        }
        else if (emotion == "Sad" || emotion == "Helpful") {
            requestJson["options"]["temperature"] = 0.6; // More focused responses
            requestJson["options"]["top_p"] = 0.6;      // More consistent
        }

        std::string jsonPayload = requestJson.dump();
        LOG_DEBUG("module.llm_chat", "Request payload: %s", jsonPayload.c_str());

        // Set up the IO context
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
        LOG_INFO("module.llm_chat", "Attempting to connect to %s:%s", LLM_Config.Host.c_str(), LLM_Config.Port.c_str());

        // Make the connection
        beast::error_code ec;
        stream.connect(results, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to connect to Ollama API at %s:%s - Error: %s", 
                LLM_Config.Host.c_str(), LLM_Config.Port.c_str(), ec.message().c_str());
            return "Error: Cannot connect to Ollama. Please check if Ollama is running.";
        }
        LOG_INFO("module.llm_chat", "Successfully connected to Ollama API");

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = jsonPayload;
        req.prepare_payload();

        // Send the HTTP request
        http::write(stream, req, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to send request to Ollama - Error: %s", ec.message().c_str());
            return "Error: Failed to send request to Ollama";
        }
        LOG_INFO("module.llm_chat", "Successfully sent request to Ollama");

        // This buffer is used for reading
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to read response from Ollama - Error: %s", ec.message().c_str());
            return "Error: Failed to get response from Ollama";
        }

        // Gracefully close the socket
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok)
        {
            LOG_ERROR("module.llm_chat", "HTTP error %d from Ollama: %s", 
                static_cast<int>(res.result()), res.body().c_str());
            return "Error: Ollama service error - " + std::to_string(static_cast<int>(res.result()));
        }

        std::string response = ParseLLMResponse(res.body());
        LOG_INFO("module.llm_chat", "Successfully received response from Ollama: %s", response.c_str());
        
        return response;
    }
    catch (const boost::system::system_error& e)
    {
        LOG_ERROR("module.llm_chat", "Network error connecting to Ollama: %s", e.what());
        return "Error: Cannot connect to Ollama - " + std::string(e.what());
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "Error communicating with Ollama: %s", e.what());
        return "Error: Ollama service error - " + std::string(e.what());
    }
}

// Move these class definitions before SendAIResponse
class RemovePacifiedEvent : public BasicEvent
{
    Player* player;

public:
    RemovePacifiedEvent(Player* p) : player(p) {}

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (player && player->IsInWorld())
        {
            player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
        }
        return true;
    }
};

class BotResponseEvent : public BasicEvent
{
    Player* responder;
    Player* originalSender;
    std::string response;
    uint32 chatType;
    std::string message;
    TeamId team;

public:
    BotResponseEvent(Player* r, Player* s, std::string resp, uint32 t, std::string m, TeamId tm) 
        : responder(r), originalSender(s), response(resp), chatType(t), message(m), team(tm) {}

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (!responder || !responder->IsInWorld())
            return true;

        // Double check we're not responding as the original sender
        if (originalSender && 
            (responder == originalSender || 
             (responder->GetSession() && originalSender->GetSession() && 
              responder->GetSession()->GetAccountId() == originalSender->GetSession()->GetAccountId())))
        {
            LOG_ERROR("module.llm_chat", "Prevented response from original sender's account");
            return true;
        }

        std::string logMsg = "Executing response from " + responder->GetName() + ": " + response;
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());

        // Check if the bot has a session
        if (WorldSession* session = responder->GetSession())
        {
            if (session->IsBot())
            {
                // Stop any current movement
                responder->StopMoving();
                responder->ClearInCombat();
                
                // Clear any current actions
                responder->InterruptNonMeleeSpells(false);
                responder->RemoveAurasByType(SPELL_AURA_MOUNTED);
                
                // Remove food/drink auras (using actual aura IDs)
                responder->RemoveAura(433);  // Food
                responder->RemoveAura(430);  // Drink
            }
        }

        switch (chatType)
        {
            case CHAT_MSG_SAY:
                responder->Say(response, LANG_UNIVERSAL);
                break;
                
            case CHAT_MSG_YELL:
                responder->Yell(response, LANG_UNIVERSAL);
                break;
                
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
                if (Group* group = responder->GetGroup())
                {
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, static_cast<ChatMsg>(chatType), 
                        LANG_UNIVERSAL, responder, nullptr, response);
                    group->BroadcastPacket(&data, false);
                }
                break;
                
            case CHAT_MSG_GUILD:
                if (Guild* guild = responder->GetGuild())
                {
                    guild->BroadcastToGuild(responder->GetSession(), false, 
                        response, LANG_UNIVERSAL);
                }
                break;
                
            case CHAT_MSG_CHANNEL:
                if (ChannelMgr* cMgr = ChannelMgr::forTeam(team))
                {
                    size_t spacePos = message.find(' ');
                    if (spacePos != std::string::npos)
                    {
                        std::string channelName = message.substr(0, spacePos);
                        if (Channel* channel = cMgr->GetChannel(channelName, responder))
                        {
                            channel->Say(responder->GetGUID(), response, LANG_UNIVERSAL);
                        }
                    }
                }
                break;
        }

        // Add a small delay before the bot can act again
        if (WorldSession* session = responder->GetSession())
        {
            if (session->IsBot())
            {
                // Add a 5-second immunity to prevent immediate actions
                responder->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
                responder->m_Events.AddEvent(new RemovePacifiedEvent(responder), 
                    responder->m_Events.CalculateTime(5000));
            }
        }

        std::string deliveredMsg = "Successfully delivered response from " + responder->GetName();
        LOG_INFO("module.llm_chat", "%s", deliveredMsg.c_str());
        return true;
    }
};

class TriggerResponseEvent : public BasicEvent
{
    Player* player;
    std::string message;
    uint32 chatType;
    TeamId team;

public:
    TriggerResponseEvent(Player* p, std::string msg, uint32 type) 
        : player(p), message(msg), chatType(type), team(p->GetTeamId()) {}

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (!player || !player->IsInWorld())
            return true;

        SendAIResponse(player, message, chatType, team);
        return true;
    }
};

// Add after the QueryLLM function
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team)
{
    if (!sender || !sender->IsInWorld())
        return;

    Map* map = sender->GetMap();
    if (!map)
        return;

    // Create a conversation key
    std::string conversationKey = sender->GetName() + "_" + msg;
    
    // Check if we've reached the maximum rounds for this conversation
    if (conversationRounds[conversationKey] >= LLM_Config.MaxConversationRounds)
    {
        std::string logMsg = "Maximum conversation rounds reached for " + sender->GetName();
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());
        return;
    }
    
    // Increment the conversation round counter
    conversationRounds[conversationKey]++;

    std::string logMsg = "Player " + sender->GetName() + " says: " + msg;
    LOG_INFO("module.llm_chat", "%s", logMsg.c_str());

    // Get all eligible bots
    std::vector<Player*> eligibleBots;
    float maxDistance = (chatType == CHAT_MSG_YELL) ? 300.0f : LLM_Config.ChatRange;

    map->DoForAllPlayers([&](Player* player) {
        if (!player || !player->IsInWorld() || player == sender)
            return;

        // Skip if it's not a bot
        if (!player->GetSession() || !player->GetSession()->IsBot())
            return;

        // Skip if player is too far for say/yell
        if ((chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_YELL) && 
            sender->GetDistance(player) > maxDistance)
            return;

        // For party chat, check if in same group
        if ((chatType == CHAT_MSG_PARTY || chatType == CHAT_MSG_PARTY_LEADER) &&
            (!sender->GetGroup() || !sender->GetGroup()->IsMember(player->GetGUID())))
            return;

        // For guild chat, check if in same guild
        if (chatType == CHAT_MSG_GUILD &&
            (!sender->GetGuild() || sender->GetGuild()->GetId() != player->GetGuildId()))
            return;

        eligibleBots.push_back(player);
    });

    if (eligibleBots.empty())
    {
        std::string logMsg = "No eligible bots found to respond to " + sender->GetName();
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());
        return;
    }

    std::string countMsg = "Found " + std::to_string(eligibleBots.size()) + " eligible bots to respond";
    LOG_INFO("module.llm_chat", "%s", countMsg.c_str());

    // Use proper random shuffle
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(eligibleBots.begin(), eligibleBots.end(), g);
    
    uint32 numResponders = std::min(LLM_Config.MaxResponsesPerMessage, static_cast<uint32>(eligibleBots.size()));
    
    for (uint32 i = 0; i < numResponders; ++i)
    {
        // Apply response chance
        if (urand(1, 100) > LLM_Config.ResponseChance)
        {
            std::string skipMsg = "Bot " + eligibleBots[i]->GetName() + " skipped response due to chance";
            LOG_INFO("module.llm_chat", "%s", skipMsg.c_str());
            continue;
        }

        Player* respondingBot = eligibleBots[i];
        
        // Get AI response
        std::string response = QueryLLM(msg, sender->GetName());
        if (response.empty())
        {
            std::string emptyMsg = "Bot " + respondingBot->GetName() + " got empty response from LLM";
            LOG_INFO("module.llm_chat", "%s", emptyMsg.c_str());
            continue;
        }

        // Add the response prefix if configured
        if (!LLM_Config.ResponsePrefix.empty())
        {
            response = LLM_Config.ResponsePrefix + response;
        }

        std::string responseMsg = "Bot " + respondingBot->GetName() + " responds to " + 
            sender->GetName() + ": " + response;
        LOG_INFO("module.llm_chat", "%s", responseMsg.c_str());

        // Add a random delay between 1-3 seconds, increasing with each responder
        uint32 delay = urand(1000 * (i + 1), 3000 * (i + 1));
        std::string delayMsg = "Scheduling response from " + respondingBot->GetName() + 
            " with " + std::to_string(delay) + "ms delay";
        LOG_INFO("module.llm_chat", "%s", delayMsg.c_str());

        // Schedule the response
        BotResponseEvent* event = new BotResponseEvent(respondingBot, sender, response, chatType, msg, team);
        respondingBot->m_Events.AddEvent(event, respondingBot->m_Events.CalculateTime(delay));
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
            std::string inputMsg = "Player " + playerName + " says: " + input;
            std::string responseMsg = "AI Response: " + response;
            LOG_INFO("module.llm_chat", "%s", inputMsg.c_str());
            LOG_INFO("module.llm_chat", "%s", responseMsg.c_str());
        }
    }
};

// Add this class before BotResponseEvent
class LLMChatAnnounce : public PlayerScript
{
public:
    LLMChatAnnounce() : PlayerScript("LLMChatAnnounce") {}

    void OnLogin(Player* player) override
    {
        // Announce Module
        if (sConfigMgr->GetOption<int32>("LLMChat.Announce", 1))
        {
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the LLM Chat module.");
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
        LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Model", "gddisney/llama3.2-uncensored");
        LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLMChat.ChatRange", 25.0f);
        LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLMChat.ResponsePrefix", "");
        LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLMChat.LogLevel", 3);
        
        // New configuration options
        LLM_Config.MaxResponsesPerMessage = sConfigMgr->GetOption<uint32>("LLMChat.MaxResponsesPerMessage", 2);
        LLM_Config.MaxConversationRounds = sConfigMgr->GetOption<uint32>("LLMChat.MaxConversationRounds", 3);
        LLM_Config.ResponseChance = sConfigMgr->GetOption<uint32>("LLMChat.ResponseChance", 50);

        // Parse the endpoint URL
        ParseEndpointURL(LLM_Config.OllamaEndpoint, LLM_Config);

        // Load LLM parameters
        LLM_Config.Temperature = sConfigMgr->GetOption<float>("LLMChat.LLM.Temperature", 0.8f);
        LLM_Config.TopP = sConfigMgr->GetOption<float>("LLMChat.LLM.TopP", 0.9f);
        LLM_Config.NumPredict = sConfigMgr->GetOption<uint32>("LLMChat.LLM.NumPredict", 1024);
        LLM_Config.ContextSize = sConfigMgr->GetOption<uint32>("LLMChat.LLM.ContextSize", 4096);
        LLM_Config.RepeatPenalty = sConfigMgr->GetOption<float>("LLMChat.LLM.RepeatPenalty", 1.2f);
        LLM_Config.PersonalityFile = sConfigMgr->GetOption<std::string>("LLMChat.PersonalityFile", "mod_llm_chat/conf/personalities.json");

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
        LOG_INFO("module.llm_chat", "Max Responses Per Message: %u", LLM_Config.MaxResponsesPerMessage);
        LOG_INFO("module.llm_chat", "Max Conversation Rounds: %u", LLM_Config.MaxConversationRounds);
        LOG_INFO("module.llm_chat", "Response Chance: %u%%", LLM_Config.ResponseChance);
        LOG_INFO("module.llm_chat", "Temperature: %.2f", LLM_Config.Temperature);
        LOG_INFO("module.llm_chat", "TopP: %.2f", LLM_Config.TopP);
        LOG_INFO("module.llm_chat", "NumPredict: %u", LLM_Config.NumPredict);
        LOG_INFO("module.llm_chat", "ContextSize: %u", LLM_Config.ContextSize);
        LOG_INFO("module.llm_chat", "RepeatPenalty: %.2f", LLM_Config.RepeatPenalty);
        LOG_INFO("module.llm_chat", "=== End Configuration ===");

        // Load personalities
        if (!LoadPersonalities(LLM_Config.PersonalityFile)) {
            LOG_ERROR("module.llm_chat", "No personalities loaded! Using default personality.");
            Personality defaultPersonality;
            defaultPersonality.id = "default";
            defaultPersonality.name = "Default";
            defaultPersonality.prompt = "You are a friendly and helpful player who enjoys casual conversation.";
            defaultPersonality.emotions = {"Friendly", "Helpful", "Excited"};
            defaultPersonality.traits = {
                {"gaming_experience", "moderate"},
                {"chattiness", "moderate"},
                {"humor_level", "moderate"},
                {"formality", "low"}
            };
            defaultPersonality.interests = {"gaming", "socializing", "helping"};
            defaultPersonality.chat_style = {
                {"uses_emotes", true},
                {"uses_slang", true},
                {"typo_frequency", "occasional"}
            };
            g_personalities.push_back(defaultPersonality);
        }
    }

    bool LoadPersonalities(std::string const& filename) {
        try {
            std::ifstream file(filename);
            if (!file.is_open()) {
                LOG_ERROR("module.llm_chat", "Failed to open personality file: {}", filename);
                return false;
            }

            nlohmann::json json_data;
            file >> json_data;

            // Load personalities
            g_personalities.clear();
            for (const auto& p : json_data["personalities"]) {
                Personality personality;
                personality.id = p["id"];
                personality.name = p["name"];
                personality.prompt = p["prompt"];
                personality.emotions = p["emotions"].get<std::vector<std::string>>();
                personality.traits = p["traits"];
                personality.interests = p["interests"].get<std::vector<std::string>>();
                personality.chat_style = p["chat_style"];
                g_personalities.push_back(personality);
            }

            // Load emotion types
            g_emotion_types = json_data["emotion_types"];

            LOG_INFO("module.llm_chat", "Loaded {} personalities from {}", g_personalities.size(), filename);
            return true;
        }
        catch (const std::exception& e) {
            LOG_ERROR("module.llm_chat", "Error loading personalities: {}", e.what());
            return false;
        }
    }
};

class LLMChatPlayerScript : public PlayerScript
{
public:
    LLMChatPlayerScript() : PlayerScript("LLMChatPlayerScript") {}

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        if (!LLM_Config.Enabled)
            return;

        if (!player || !player->IsInWorld() || msg.empty() || msg.length() < 2)
            return;

        // Skip if this is an AI response to prevent loops
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        std::string logMsg = "Player " + player->GetName() + " says: " + msg;
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());

        // Add a small delay before processing
        uint32 delay = urand(100, 500);
        LOG_INFO("module.llm_chat", "Adding AI response event with delay: %u ms", delay);

        // Create and add the event
        player->m_Events.AddEvent(new TriggerResponseEvent(player, msg, type), 
            player->m_Events.CalculateTime(delay));
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        if (!LLM_Config.Enabled || !player || !player->IsInWorld() || !channel || msg.empty() || msg.length() < 2)
            return;

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        std::string logMsg = "Player " + player->GetName() + " says in channel: " + msg;
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());

        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        if (!LLM_Config.Enabled || !player || !player->IsInWorld() || !group || msg.empty() || msg.length() < 2)
            return;

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        std::string logMsg = "Player " + player->GetName() + " says in group: " + msg;
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());

        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* guild) override
    {
        if (!LLM_Config.Enabled || !player || !player->IsInWorld() || !guild || msg.empty() || msg.length() < 2)
            return;

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        std::string logMsg = "Player " + player->GetName() + " says in guild: " + msg;
        LOG_INFO("module.llm_chat", "%s", logMsg.c_str());

        SendAIResponse(player, msg, type, player->GetTeamId());
    }
};

class LLMChat_WorldScript : public WorldScript {
public:
    LLMChat_WorldScript() : WorldScript("LLMChat_WorldScript") {}

    void OnStartup() override {
        std::string personalityFile = sConfigMgr->GetOption<std::string>("LLMChat.PersonalityFile", "mod_llm_chat/conf/personalities.json");
        if (!LoadPersonalities(personalityFile)) {
            LOG_ERROR("module.llm_chat", "Failed to load personalities!");
        }
    }
};

void AddLLMChatScripts() {
    new LLMChat_WorldScript();
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatPlayerScript();
} 