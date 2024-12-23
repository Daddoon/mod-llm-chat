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
#include <queue>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>

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
    // New configuration options
    uint32 ResponseCooldown;
    uint32 GlobalCooldown;
    uint32 MaxQueueSize;
    uint32 QueueTimeout;
};

LLMConfig LLM_Config;

// Add a conversation counter
std::map<std::string, uint32> conversationRounds;  // Key: channelName+originalMessage, Value: round count
std::map<ObjectGuid, uint32> botCooldowns;  // Track individual bot cooldowns
uint32 lastGlobalResponse = 0;  // Track last global response time

// Add at the top with other global variables
struct QueuedResponse {
    uint32 timestamp;
    Player* sender;
    std::vector<Player*> responders;  // Now supports multiple responders
    std::string message;
    uint32 chatType;
    TeamId team;
    uint32 responsesGenerated;  // Track how many responses we've generated
    uint32 maxResponses;        // How many responses we want for this message
};

std::queue<QueuedResponse> responseQueue;
std::mutex queueMutex;
uint32 const RESPONSE_TIMEOUT = 10000; // 10 seconds timeout

// Add to the global variables section
std::atomic<uint32> activeThreads(0);
const uint32 MAX_CONCURRENT_THREADS = 2;  // Maximum concurrent LLM requests

// Add to global variables
struct GlobalRateLimit {
    uint32 lastMessageTime{0};
    uint32 messageCount{0};
    static const uint32 WINDOW_SIZE = 10000;  // 10 seconds window
    static const uint32 MAX_MESSAGES = 5;     // Max 5 messages per window
} globalRateLimit;

// Helper function to join strings with a delimiter
template<typename Container>
std::string JoinStrings(const Container& strings, const std::string& delimiter) {
    std::string result;
    bool first = true;
    for (const auto& str : strings) {
        if (!first) {
            result += delimiter;
        }
        result += str;
        first = false;
    }
    return result;
}

// Helper function to convert chat type to string
std::string GetChatTypeString(uint32 type) {
    switch (type) {
        case CHAT_MSG_SAY:
            return "Say";
        case CHAT_MSG_YELL:
            return "Yell";
        case CHAT_MSG_PARTY:
            return "Party";
        case CHAT_MSG_PARTY_LEADER:
            return "Party Leader";
        case CHAT_MSG_GUILD:
            return "Guild";
        case CHAT_MSG_OFFICER:
            return "Officer";
        case CHAT_MSG_RAID:
            return "Raid";
        case CHAT_MSG_RAID_LEADER:
            return "Raid Leader";
        case CHAT_MSG_RAID_WARNING:
            return "Raid Warning";
        case CHAT_MSG_CHANNEL:
            return "Channel";
        case CHAT_MSG_WHISPER:
            return "Whisper";
        case CHAT_MSG_WHISPER_INFORM:
            return "Whisper Reply";
        case CHAT_MSG_EMOTE:
            return "Emote";
        case CHAT_MSG_TEXT_EMOTE:
            return "Text Emote";
        default:
            return "Unknown";
    }
}
}

// Helper function to parse URL
void ParseEndpointURL(std::string const& endpoint, LLMConfig& config)
{
    try {
        size_t protocolEnd = endpoint.find("://");
        if (protocolEnd == std::string::npos) {
            LOG_ERROR("module.llm_chat", "Invalid endpoint URL (no protocol): {}", endpoint);
            return;
        }

        std::string url = endpoint.substr(protocolEnd + 3);
        size_t pathStart = url.find('/');
        if (pathStart == std::string::npos) {
            LOG_ERROR("module.llm_chat", "Invalid endpoint URL (no path): {}", endpoint);
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
                    LOG_ERROR("module.llm_chat", "Invalid port number: {}", config.Port);
                    config.Port = "11434"; // Default to Ollama port
                }
            } catch (std::exception const& e) {
                LOG_ERROR("module.llm_chat", "Invalid port format: {}", e.what());
                config.Port = "11434"; // Default to Ollama port
            }
        } else {
            config.Host = hostPort;
            config.Port = "11434"; // Default to Ollama port
        }

        LOG_INFO("module.llm_chat", "URL parsed successfully - Host: {}, Port: {}, Target: {}", 
                config.Host, config.Port, config.Target);
    }
    catch (std::exception const& e) {
        LOG_ERROR("module.llm_chat", "Error parsing URL: {}", e.what());
        // Set defaults
        config.Host = "localhost";
        config.Port = "11434";
        config.Target = "/api/generate";
    }
}

std::string ParseLLMResponse(const std::string& rawResponse)
{
    try {
        LOG_DEBUG("module.llm_chat", "Parsing raw response: {}", rawResponse);
        
        auto jsonResponse = json::parse(rawResponse);
        
        // Check for response field
        if (jsonResponse.contains("response"))
        {
            std::string response = jsonResponse["response"].get<std::string>();
            LOG_DEBUG("module.llm_chat", "Parsed response: {}", response);
            return response;
        }
        
        // Fallback to checking for text field
        if (jsonResponse.contains("text"))
        {
            std::string response = jsonResponse["text"].get<std::string>();
            LOG_DEBUG("module.llm_chat", "Parsed response from text field: {}", response);
            return response;
        }

        LOG_ERROR("module.llm_chat", "No valid response field found in JSON");
        return "Error: Invalid response format";
    }
    catch (const json::parse_error& e)
    {
        LOG_ERROR("module.llm_chat", "JSON parse error: {}", e.what());
        return "Error: Failed to parse response";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "Error parsing response: {}", e.what());
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

    LOG_DEBUG("module.llm_chat", "Detected emotion '{}' for message: {}", 
        dominantEmotion, message);
    return dominantEmotion;
}

// Modify QueryLLM to be async
std::string QueryLLM(std::string const& message, const std::string& playerName)
{
    if (message.empty() || playerName.empty())
    {
        LOG_ERROR("module.llm_chat", "Empty message or player name");
        return "Error: Invalid input";
    }

    // Create a promise to handle the async result
    std::promise<std::string> responsePromise;
    auto responseFuture = responsePromise.get_future();

    // Launch API request in a separate thread
    std::thread apiThread([message, playerName, &responsePromise]() {
        try {
            // Detect emotion and select appropriate personality
            std::string emotion = DetectEmotion(message);
            Personality personality = SelectPersonality(emotion);
            
            LOG_DEBUG("module.llm_chat", "Detected emotion: {}, Selected personality: {}", 
                     emotion, personality.name);

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

            LOG_DEBUG("module.llm_chat", "Context prompt: {}", contextPrompt);

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

        // Set up the IO context
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
            LOG_DEBUG("module.llm_chat", "Attempting to connect to {}:{}", LLM_Config.Host, LLM_Config.Port);

            // Make the connection with timeout
        beast::error_code ec;
        stream.connect(results, ec);
        if (ec)
        {
                responsePromise.set_value("Error: Cannot connect to Ollama. Please check if Ollama is running.");
                return;
        }

        // Set up an HTTP POST request message
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
            req.body() = requestJson.dump();
        req.prepare_payload();

        // Send the HTTP request
        http::write(stream, req, ec);
        if (ec)
        {
                responsePromise.set_value("Error: Failed to send request to Ollama");
                return;
        }

        // This buffer is used for reading
        beast::flat_buffer buffer;
        http::response<http::string_body> res;

            // Receive the HTTP response with timeout
        http::read(stream, buffer, res, ec);
        if (ec)
        {
                responsePromise.set_value("Error: Failed to get response from Ollama");
                return;
        }

        // Gracefully close the socket
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok)
        {
                std::string error = "Error: Ollama service error - " + std::to_string(static_cast<int>(res.result()));
                responsePromise.set_value(error);
                return;
        }

        std::string response = ParseLLMResponse(res.body());
            responsePromise.set_value(response);
        }
        catch (const std::exception& e)
        {
            responsePromise.set_value("Error: " + std::string(e.what()));
        }
    });

    // Detach the thread to let it run independently
    apiThread.detach();

    // Wait for the response with a timeout
    if (responseFuture.wait_for(std::chrono::seconds(LLM_Config.QueueTimeout)) == std::future_status::timeout)
    {
        return "Error: Request timed out";
    }

    return responseFuture.get();
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
        LOG_INFO("module.llm_chat", "{}", logMsg);

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
        LOG_INFO("module.llm_chat", "{}", deliveredMsg);
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

// Add before SendAIResponse function
bool IsOnCooldown(Player* bot) {
    if (!bot) return true;

    uint32 currentTime = getMSTime();

    // Check global cooldown
    if (currentTime - lastGlobalResponse < (LLM_Config.GlobalCooldown * 1000))
        return true;

    // Check individual bot cooldown
    auto it = botCooldowns.find(bot->GetGUID());
    if (it != botCooldowns.end()) {
        if (currentTime - it->second < (LLM_Config.ResponseCooldown * 1000))
            return true;
    }

    return false;
}

void UpdateCooldowns(Player* bot) {
    if (!bot) return;
    uint32 currentTime = getMSTime();
    botCooldowns[bot->GetGUID()] = currentTime;
    lastGlobalResponse = currentTime;
}

// Modify ProcessResponseQueue to not use 'this'
void ProcessResponseQueue() {
    std::lock_guard<std::mutex> lock(queueMutex);
    uint32 currentTime = getMSTime();

    // Don't process if we have too many active threads
    if (activeThreads >= MAX_CONCURRENT_THREADS) {
        LOG_DEBUG("module.llm_chat", "Max concurrent threads reached ({}), waiting...", 
            MAX_CONCURRENT_THREADS);
            return;
        }

    if (responseQueue.empty()) {
            return;
        }

    // Only process the front of the queue
    QueuedResponse& queuedResponse = responseQueue.front();
        
    // Check if response has timed out
    if (currentTime - queuedResponse.timestamp > (LLM_Config.QueueTimeout * 1000)) {
        LOG_DEBUG("module.llm_chat", "Response timed out for {} after {}s", 
            queuedResponse.sender->GetName(), LLM_Config.QueueTimeout);
        responseQueue.pop();
            return;
        }

    // Get next responder
    if (queuedResponse.responsesGenerated >= queuedResponse.maxResponses || 
        queuedResponse.responsesGenerated >= queuedResponse.responders.size()) {
        responseQueue.pop();
            return;
        }

    Player* currentResponder = queuedResponse.responders[queuedResponse.responsesGenerated];
    
    // Increment active threads before starting new one
    activeThreads++;
    
    // Create a copy of the necessary data for the thread
    Player* sender = queuedResponse.sender;
    std::string message = queuedResponse.message;
    uint32 chatType = queuedResponse.chatType;
    TeamId team = queuedResponse.team;
    
    // Process the response for this bot in a separate thread
    std::thread([sender, message, currentResponder, chatType, team]() {
        std::string response = QueryLLM(message, sender->GetName());
        
        if (!response.empty()) {
            // Add the response prefix if configured
            if (!LLM_Config.ResponsePrefix.empty()) {
                response = LLM_Config.ResponsePrefix + response;
            }

            // Schedule the response with increasing delay based on response number
            uint32 baseDelay = 2000;  // 2 seconds base delay
            uint32 delay = baseDelay + (urand(0, 1500));  // Add random delay up to 1.5s
            
            // Create the event
            BotResponseEvent* event = new BotResponseEvent(
                currentResponder, 
                sender, 
                response, 
                chatType, 
                message, 
                team
            );

            // Add the event to the responder's event queue
            if (currentResponder && currentResponder->IsInWorld()) {
                currentResponder->m_Events.AddEvent(event, 
                    currentResponder->m_Events.CalculateTime(delay));
            } else {
                delete event;
            }
        }

        // Increment the responses generated count and decrement active threads
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!responseQueue.empty()) {
                responseQueue.front().responsesGenerated++;
            }
            activeThreads--;
        }
    }).detach();
}

// Add to SendAIResponse before processing
bool CanProcessMessage(Player* sender) {
    if (!sender)
        return false;

    uint32 currentTime = getMSTime();

    // Global rate limiting
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        
        // Reset counter if window has passed
        if (currentTime - globalRateLimit.lastMessageTime > GlobalRateLimit::WINDOW_SIZE) {
            globalRateLimit.messageCount = 0;
            globalRateLimit.lastMessageTime = currentTime;
        }
        
        // Check if we're over the limit
        if (globalRateLimit.messageCount >= GlobalRateLimit::MAX_MESSAGES) {
            LOG_DEBUG("module.llm_chat", "Global rate limit reached, skipping message");
            return false;
        }
        
        globalRateLimit.messageCount++;
    }

    // Per-player rate limiting
    static std::map<ObjectGuid, uint32> playerLastMessage;
    uint32 playerCooldown = 10000;  // Increased to 10 seconds between messages per player

    if (playerLastMessage.count(sender->GetGUID()) > 0) {
        if (currentTime - playerLastMessage[sender->GetGUID()] < playerCooldown) {
            LOG_DEBUG("module.llm_chat", "Player cooldown active for {}, skipping message", 
                sender->GetName());
            return false;
        }
    }
    playerLastMessage[sender->GetGUID()] = currentTime;

    return true;
}

// Modify SendAIResponse to use the new rate limiting
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team) {
    if (!sender || !sender->IsInWorld())
        return;

    // Check rate limits first
    if (!CanProcessMessage(sender))
        return;

    Map* map = sender->GetMap();
    if (!map)
        return;

    // Filter out short or spammy messages
    if (msg.length() < 5 || msg.length() > 200) {
        LOG_DEBUG("module.llm_chat", "Message length outside acceptable range, skipping");
        return;
    }

    // Check for message spam/repetition
    static std::map<ObjectGuid, std::string> lastPlayerMessage;
    if (lastPlayerMessage.count(sender->GetGUID()) > 0) {
        if (lastPlayerMessage[sender->GetGUID()] == msg) {
            LOG_DEBUG("module.llm_chat", "Repeated message from {}, skipping", sender->GetName());
            return;
        }
    }
    lastPlayerMessage[sender->GetGUID()] = msg;

    // Check queue size with increased restrictions
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (responseQueue.size() >= LLM_Config.MaxQueueSize / 2) {  // More restrictive queue limit
            LOG_DEBUG("module.llm_chat", "Response queue getting full, skipping response for {}", 
                sender->GetName());
            return;
        }
    }

    // Get all eligible bots that aren't on cooldown
    std::vector<Player*> eligibleBots;
    float maxDistance = (chatType == CHAT_MSG_YELL) ? 300.0f : LLM_Config.ChatRange;

    map->DoForAllPlayers([&](Player* player) {
        if (!player || !player->IsInWorld() || player == sender)
            return;

        // Skip if it's not a bot or if it's on cooldown
        if (!player->GetSession() || !player->GetSession()->IsBot() || IsOnCooldown(player))
            return;

        // Add distance check
        if (sender->GetDistance(player) > maxDistance)
            return;

        eligibleBots.push_back(player);
    });

    if (eligibleBots.empty()) {
        return;
    }

    // Randomly select 1-2 bots (reduced from 2-3)
    uint32 numResponders = std::min(urand(1, 2), static_cast<uint32>(eligibleBots.size()));
    std::vector<Player*> selectedBots;
    
    // Shuffle and take first numResponders
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(eligibleBots.begin(), eligibleBots.end(), g);
    selectedBots.assign(eligibleBots.begin(), eligibleBots.begin() + numResponders);

    // Add to queue
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        responseQueue.push({
            getMSTime(),
            sender,
            selectedBots,
            msg,
            chatType,
            team,
            0,              // No responses generated yet
            numResponders   // Total responses we want
        });
    }

    // Update cooldowns for all selected bots
    for (Player* bot : selectedBots) {
        UpdateCooldowns(bot);
    }
}

class LLMChatLogger {
public:
    static void Log(int32 level, std::string const& message) {
        if (LLM_Config.LogLevel >= level) {
            LOG_INFO("module.llm_chat", "{}", message);
        }
    }

    static void LogChat(std::string const& playerName, std::string const& input, std::string const& response) {
        if (LLM_Config.LogLevel >= 2) {
            std::string inputMsg = "Player " + playerName + " says: " + input;
            std::string responseMsg = "AI Response: " + response;
            LOG_INFO("module.llm_chat", "{}", inputMsg);
            LOG_INFO("module.llm_chat", "{}", responseMsg);
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
        LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Model", "krith/meta-llama-3.2-1b-instruct-uncensored:IQ3_M");
        LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLMChat.ChatRange", 25.0f);
        LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLMChat.ResponsePrefix", "");
        LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLMChat.LogLevel", 3);
        
        // New configuration options
        LLM_Config.MaxResponsesPerMessage = sConfigMgr->GetOption<uint32>("LLMChat.MaxResponsesPerMessage", 1);
        LLM_Config.MaxConversationRounds = sConfigMgr->GetOption<uint32>("LLMChat.MaxConversationRounds", 1);
        LLM_Config.ResponseChance = sConfigMgr->GetOption<uint32>("LLMChat.ResponseChance", 20);
        LLM_Config.ResponseCooldown = sConfigMgr->GetOption<uint32>("LLMChat.ResponseCooldown", 15);
        LLM_Config.GlobalCooldown = sConfigMgr->GetOption<uint32>("LLMChat.GlobalCooldown", 5);
        LLM_Config.MaxQueueSize = sConfigMgr->GetOption<uint32>("LLMChat.MaxQueueSize", 5);
        LLM_Config.QueueTimeout = sConfigMgr->GetOption<uint32>("LLMChat.QueueTimeout", 5); // seconds

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
        LOG_INFO("module.llm_chat", "Enabled: {}", LLM_Config.Enabled ? "true" : "false");
        LOG_INFO("module.llm_chat", "Provider: {}", LLM_Config.Provider);
        LOG_INFO("module.llm_chat", "Endpoint: {}", LLM_Config.OllamaEndpoint);
        LOG_INFO("module.llm_chat", "Model: {}", LLM_Config.OllamaModel);
        LOG_INFO("module.llm_chat", "Host: {}", LLM_Config.Host);
        LOG_INFO("module.llm_chat", "Port: {}", LLM_Config.Port);
        LOG_INFO("module.llm_chat", "Target: {}", LLM_Config.Target);
        LOG_INFO("module.llm_chat", "Response Prefix: '{}'", LLM_Config.ResponsePrefix);
        LOG_INFO("module.llm_chat", "Chat Range: {:.2f}", LLM_Config.ChatRange);
        LOG_INFO("module.llm_chat", "Log Level: {}", LLM_Config.LogLevel);
        LOG_INFO("module.llm_chat", "Max Responses Per Message: {}", LLM_Config.MaxResponsesPerMessage);
        LOG_INFO("module.llm_chat", "Max Conversation Rounds: {}", LLM_Config.MaxConversationRounds);
        LOG_INFO("module.llm_chat", "Response Chance: {}%", LLM_Config.ResponseChance);
        LOG_INFO("module.llm_chat", "Temperature: {:.2f}", LLM_Config.Temperature);
        LOG_INFO("module.llm_chat", "TopP: {:.2f}", LLM_Config.TopP);
        LOG_INFO("module.llm_chat", "NumPredict: {}", LLM_Config.NumPredict);
        LOG_INFO("module.llm_chat", "ContextSize: {}", LLM_Config.ContextSize);
        LOG_INFO("module.llm_chat", "RepeatPenalty: {:.2f}", LLM_Config.RepeatPenalty);
        LOG_INFO("module.llm_chat", "Max Queue Size: {}", LLM_Config.MaxQueueSize);
        LOG_INFO("module.llm_chat", "Queue Timeout: {} seconds", LLM_Config.QueueTimeout);
        LOG_INFO("module.llm_chat", "=== Personality System ===");
        
        // Log emotion types
        LOG_INFO("module.llm_chat", "Available Emotions:");
        for (const auto& [emotion, data] : g_emotion_types.items()) {
            LOG_INFO("module.llm_chat", "  {} - Intensity: {}", 
                emotion, data.value("intensity", "normal"));
        }

        // Log personalities
        LOG_INFO("module.llm_chat", "Loaded Personalities ({}):", g_personalities.size());
        for (const auto& personality : g_personalities) {
            LOG_INFO("module.llm_chat", "  === {} ({}) ===", personality.name, personality.id);
            LOG_INFO("module.llm_chat", "    Emotions: {}", JoinStrings(personality.emotions, ", "));
            
            // Log traits
            LOG_INFO("module.llm_chat", "    Traits:");
            for (const auto& [trait, value] : personality.traits.items()) {
                LOG_INFO("module.llm_chat", "      {}: {}", trait, value.get<std::string>());
            }
            
            // Log interests
            LOG_INFO("module.llm_chat", "    Interests: {}", JoinStrings(personality.interests, ", "));
            
            // Log chat style
            LOG_INFO("module.llm_chat", "    Chat Style:");
            for (const auto& [style, value] : personality.chat_style.items()) {
                if (value.is_boolean()) {
                    LOG_INFO("module.llm_chat", "      {}: {}", style, value.get<bool>() ? "yes" : "no");
                } else {
                    LOG_INFO("module.llm_chat", "      {}: {}", style, value.get<std::string>());
                }
            }
        }
        
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

        std::string logMsg = Acore::StringFormat("[Chat] Player '%s' says in %s: %s", 
            player->GetName().c_str(),
            GetChatTypeString(type).c_str(),
            msg.c_str());
        LOG_INFO("module.llm_chat", "{}", logMsg);

        // Add a small delay before processing
        uint32 delay = urand(100, 500);
        LOG_INFO("module.llm_chat", "[System] Adding AI response event for %s with %ums delay", 
            GetChatTypeString(type).c_str(), delay);

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

        std::string logMsg = Acore::StringFormat("[Chat] Player '%s' says in channel '%s': %s", 
            player->GetName().c_str(),
            channel->GetName().c_str(),
            msg.c_str());
        LOG_INFO("module.llm_chat", "{}", logMsg);

        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        if (!LLM_Config.Enabled || !player || !player->IsInWorld() || !group || msg.empty() || msg.length() < 2)
            return;

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        std::string logMsg = Acore::StringFormat("[Chat] Player '%s' says in group: %s", 
            player->GetName().c_str(),
            msg.c_str());
        LOG_INFO("module.llm_chat", "{}", logMsg);

        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* guild) override
    {
        if (!LLM_Config.Enabled || !player || !player->IsInWorld() || !guild || msg.empty() || msg.length() < 2)
            return;

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        std::string logMsg = Acore::StringFormat("[Chat] Player '%s' says in guild '%s': %s", 
            player->GetName().c_str(),
            guild->GetName().c_str(),
            msg.c_str());
        LOG_INFO("module.llm_chat", "{}", logMsg);

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

    void OnUpdate(uint32 diff) override {
        static uint32 updateTimer = 0;
        updateTimer += diff;

        // Process queue every second
        if (updateTimer >= 1000) {
            ProcessResponseQueue();
            updateTimer = 0;
        }
    }
};

void Add_LLMChatScripts()
{
    new LLMChat_WorldScript();
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatPlayerScript();
} 