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
#include <condition_variable>

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

// Config Variables
struct LLMConfig
{
    // Core Settings
    bool Enabled;
    int32 LogLevel;
    bool Announce;

    // Provider Settings
    std::string Endpoint;
    std::string Model;
    std::string ApiKey;
    std::string ApiSecret;

    // Chat Behavior
    float ChatRange;
    std::string ResponsePrefix;
    uint32 MaxResponsesPerMessage;
    uint32 ResponseChance;

    // Performance & Rate Limiting
    struct {
        struct {
            uint32 WindowSize;
            uint32 MaxMessages;
        } GlobalRateLimit;

        struct {
            uint32 Player;
            uint32 Bot;
            uint32 Global;
        } Cooldowns;

        struct {
            uint32 MaxThreads;
            uint32 MaxApiCalls;
            uint32 ApiTimeout;
        } Threading;

        struct {
            uint32 Min;
            uint32 Max;
        } MessageLimits;

        struct {
            uint32 Min;
            uint32 Max;
            uint32 Pacified;
        } Delays;
    } Performance;

    // Queue Settings
    struct {
        uint32 Size;
        uint32 Timeout;
    } Queue;

    // LLM Parameters
    struct {
        float Temperature;
        float TopP;
        uint32 NumPredict;
        uint32 ContextSize;
        float RepeatPenalty;
    } LLM;

    // Memory System
    struct {
        bool Enable;
        uint32 MaxInteractionsPerPair;
        uint32 ExpirationTime;
        uint32 MaxContextLength;
    } Memory;

    // Personality System
    std::string PersonalityFile;

    // URL components (parsed from endpoint)
    std::string Host;
    std::string Port;
    std::string Target;

    // Database Settings
    struct {
        std::string CharacterDB;
        std::string WorldDB;
        std::string AuthDB;
        std::string CustomDB;  // For custom tables like RP profiles
    } Database;
};

extern LLMConfig LLM_Config;
LLMConfig LLM_Config;  // Actual definition

namespace {
    // Thread safety and control
    std::atomic<bool> g_moduleShutdown{false};
    std::mutex g_stateMutex;
    std::atomic<uint32> g_activeApiCalls{0};
    uint32 MAX_API_CALLS = 5; // Default value, will be updated from config

    // Queue structure
    struct QueuedResponse {
        uint32 timestamp;
        Player* sender;
        std::vector<Player*> responders;
        std::string message;
        uint32 chatType;
        TeamId team;
        uint32 responsesGenerated;
        uint32 maxResponses;
    };

    // Global queue and mutex
    std::queue<QueuedResponse> responseQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    uint32 const RESPONSE_TIMEOUT = 10000; // 10 seconds timeout

    // Worker thread control
    std::thread g_workerThread;
    bool g_workerRunning = false;

    // Add to global variables section at the top
    std::atomic<uint32> g_activeThreads{0};
}

// Add worker thread function declaration at the top with other declarations
void WorkerThread();

// Add the implementation
void WorkerThread() {
    while (!g_moduleShutdown) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCondition.wait(lock, []() {
                return !responseQueue.empty() || g_moduleShutdown;
            });

            if (g_moduleShutdown) {
                break;
            }

            // Process the front of the queue
            if (!responseQueue.empty()) {
                QueuedResponse& queuedResponse = responseQueue.front();
                
                // Check if response has timed out
                uint32 currentTime = getMSTime();
                if (currentTime - queuedResponse.timestamp > (LLM_Config.Queue.Timeout * 1000)) {
                    LOG_DEBUG("module.llm_chat", "Response timed out for {} after {}s", 
                        queuedResponse.sender->GetName(), LLM_Config.Queue.Timeout);
                    responseQueue.pop();
                    continue;
                }

                // Get next responder
                if (queuedResponse.responsesGenerated >= queuedResponse.maxResponses || 
                    queuedResponse.responsesGenerated >= queuedResponse.responders.size()) {
                    responseQueue.pop();
                    continue;
                }

                Player* currentResponder = queuedResponse.responders[queuedResponse.responsesGenerated];
                
                // Increment active threads before starting new one
                g_activeThreads++;
                
                // Create a copy of the necessary data for the thread
                Player* sender = queuedResponse.sender;
                std::string message = queuedResponse.message;
                uint32 chatType = queuedResponse.chatType;
                TeamId team = queuedResponse.team;
                
                // Process the response for this bot in a separate thread
                std::thread([sender, message, currentResponder, chatType, team]() {
                    std::string response = QueryLLM(message, sender->GetName(), currentResponder->GetName());
                    
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
                            team,
                            LLM_Config.Performance.Delays.Pacified
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
                        g_activeThreads--;
                    }
                }).detach();
            }
        }

        // Small delay to prevent tight loop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Add declaration at the top with other declarations
void ProcessResponseQueue();

// Add implementation before WorkerThread function
void ProcessResponseQueue() {
    std::lock_guard<std::mutex> lock(queueMutex);
    uint32 currentTime = getMSTime();

    // Don't process if we have too many active threads
    if (g_activeThreads >= LLM_Config.Performance.Threading.MaxThreads) {
        LOG_DEBUG("module.llm_chat", "Max concurrent threads reached ({}), waiting...", 
            LLM_Config.Performance.Threading.MaxThreads);
        return;
    }

    if (responseQueue.empty()) {
        return;
    }

    // Only process the front of the queue
    QueuedResponse& queuedResponse = responseQueue.front();
        
    // Check if response has timed out
    if (currentTime - queuedResponse.timestamp > (LLM_Config.Queue.Timeout * 1000)) {
        LOG_DEBUG("module.llm_chat", "Response timed out for {} after {}s", 
            queuedResponse.sender->GetName(), LLM_Config.Queue.Timeout);
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
    g_activeThreads++;
    
    // Create a copy of the necessary data for the thread
    Player* sender = queuedResponse.sender;
    std::string message = queuedResponse.message;
    uint32 chatType = queuedResponse.chatType;
    TeamId team = queuedResponse.team;
    
    // Process the response for this bot in a separate thread
    std::thread([sender, message, currentResponder, chatType, team]() {
        std::string response = QueryLLM(message, sender->GetName(), currentResponder->GetName());
        
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
                team,
                LLM_Config.Performance.Delays.Pacified
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
            g_activeThreads--;
        }
    }).detach();
}

// Logger class definition
class LLMChatLogger {
public:
    static void Log(int32 level, std::string const& message) {
        // Skip logging if disabled (level 0)
        if (LLM_Config.LogLevel == 0) {
            return;
        }
        
        // Only log if current level is high enough
        if (LLM_Config.LogLevel >= level) {
            LOG_INFO("module.llm_chat", "{}", message);
        }
    }

    static void LogChat(std::string const& playerName, std::string const& input, std::string const& response) {
        // Skip logging if disabled (level 0)
        if (LLM_Config.LogLevel == 0) {
            return;
        }
        
        // Only log chat at detailed level or higher
        if (LLM_Config.LogLevel >= 2) {
            std::string inputMsg = "Player " + playerName + " says: " + input;
            std::string responseMsg = "AI Response: " + response;
            LOG_INFO("module.llm_chat", "{}", inputMsg);
            LOG_INFO("module.llm_chat", "{}", responseMsg);
        }
    }

    static void LogError(std::string const& message) {
        // Always log errors unless logging is completely disabled
        if (LLM_Config.LogLevel > 0) {
            LOG_ERROR("module.llm_chat", "{}", message);
        }
    }

    static void LogDebug(std::string const& message) {
        // Only log debug messages at highest level
        if (LLM_Config.LogLevel >= 3) {
            LOG_DEBUG("module.llm_chat", "{}", message);
        }
    }
};

// Forward declarations
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team);
std::string QueryLLM(std::string const& message, const std::string& senderName, const std::string& responderName);
void AddToMemory(const std::string& sender, const std::string& responder, const std::string& message, const std::string& response);

// Event class definitions
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
    uint32 pacifiedDuration;

public:
    BotResponseEvent(Player* r, Player* s, std::string resp, uint32 t, std::string m, TeamId tm, uint32 pd) 
        : responder(r), originalSender(s), response(resp), chatType(t), message(m), team(tm), pacifiedDuration(pd) {}

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
                responder->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED);
                responder->m_Events.AddEvent(new RemovePacifiedEvent(responder), 
                    responder->m_Events.CalculateTime(pacifiedDuration));
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

// Add a conversation counter
std::map<std::string, uint32> conversationRounds;  // Key: channelName+originalMessage, Value: round count
std::map<ObjectGuid, uint32> botCooldowns;  // Track individual bot cooldowns
uint32 lastGlobalResponse = 0;  // Track last global response time

// Add to the global variables section
struct GlobalRateLimit {
    uint32 lastMessageTime{0};
    uint32 messageCount{0};
} globalRateLimit;

// Conversation memory structure
struct ConversationMemory {
    uint32 timestamp;
    std::string sender;
    std::string responder;
    std::string message;
    std::string response;
};

// Memory storage - key is sender+responder pair
std::map<std::string, std::vector<ConversationMemory>> g_conversationHistory;
std::mutex g_memoryMutex;

// Maximum number of remembered interactions per pair
const size_t MAX_MEMORY_PER_PAIR = 10;

// Helper function to get memory key
std::string GetMemoryKey(const std::string& sender, const std::string& responder) {
    return sender + ":" + responder;
}

// Function to add a conversation to memory
void AddToMemory(const std::string& sender, const std::string& responder, 
                 const std::string& message, const std::string& response) {
    if (!LLM_Config.Memory.Enable) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_memoryMutex);
    
    std::string key = GetMemoryKey(sender, responder);
    auto& history = g_conversationHistory[key];
    
    // Add new memory
    ConversationMemory memory;
    memory.timestamp = getMSTime();
    memory.sender = sender;
    memory.responder = responder;
    memory.message = message;
    memory.response = response;
    
    // Add to front of history
    history.insert(history.begin(), memory);
    
    // Remove expired memories and trim to max size
    uint32 currentTime = getMSTime();
    history.erase(
        std::remove_if(history.begin(), history.end(),
            [currentTime](const ConversationMemory& mem) {
                return LLM_Config.Memory.ExpirationTime > 0 && 
                       (currentTime - mem.timestamp) > (LLM_Config.Memory.ExpirationTime * 1000);
            }),
        history.end()
    );
    
    if (history.size() > LLM_Config.Memory.MaxInteractionsPerPair) {
        history.resize(LLM_Config.Memory.MaxInteractionsPerPair);
    }
}

// Function to get conversation history
std::string GetConversationContext(const std::string& sender, const std::string& responder) {
    if (!LLM_Config.Memory.Enable) {
        return "";
    }

    std::lock_guard<std::mutex> lock(g_memoryMutex);
    
    std::string key = GetMemoryKey(sender, responder);
    std::string context = "Previous conversations between you and " + sender + ":\n\n";
    
    if (g_conversationHistory.find(key) != g_conversationHistory.end()) {
        const auto& history = g_conversationHistory[key];
        
        // Remove expired memories first
        uint32 currentTime = getMSTime();
        std::vector<ConversationMemory> validMemories;
        std::copy_if(history.begin(), history.end(), std::back_inserter(validMemories),
            [currentTime](const ConversationMemory& mem) {
                return LLM_Config.Memory.ExpirationTime == 0 || 
                       (currentTime - mem.timestamp) <= (LLM_Config.Memory.ExpirationTime * 1000);
            });
        
        // Build context string with length limit
        size_t totalLength = context.length();
        for (const auto& memory : validMemories) {
            std::string interaction = 
                sender + ": " + memory.message + "\n" +
                "You: " + memory.response + "\n\n";
                
            if (totalLength + interaction.length() > LLM_Config.Memory.MaxContextLength) {
                break;
            }
            
            context += interaction;
            totalLength += interaction.length();
        }
    }
    
    return context;
}

// Add cleanup function for expired memories
void CleanupExpiredMemories() {
    if (!LLM_Config.Memory.Enable || LLM_Config.Memory.ExpirationTime == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_memoryMutex);
    uint32 currentTime = getMSTime();
    
    for (auto it = g_conversationHistory.begin(); it != g_conversationHistory.end();) {
        auto& history = it->second;
        
        history.erase(
            std::remove_if(history.begin(), history.end(),
                [currentTime](const ConversationMemory& mem) {
                    return (currentTime - mem.timestamp) > (LLM_Config.Memory.ExpirationTime * 1000);
                }),
            history.end()
        );
        
        if (history.empty()) {
            it = g_conversationHistory.erase(it);
        } else {
            ++it;
        }
    }
}

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
        
        // Check for Ollama format
        if (jsonResponse.contains("response")) {
            std::string response = jsonResponse["response"].get<std::string>();
            LOG_DEBUG("module.llm_chat", "Parsed Ollama response: {}", response);
            return response;
        }
        
        // Check for OpenAI format
        if (jsonResponse.contains("choices") && !jsonResponse["choices"].empty()) {
            if (jsonResponse["choices"][0].contains("message") && 
                jsonResponse["choices"][0]["message"].contains("content")) {
                std::string response = jsonResponse["choices"][0]["message"]["content"].get<std::string>();
                LOG_DEBUG("module.llm_chat", "Parsed OpenAI response: {}", response);
            return response;
            }
        }

        LOG_ERROR("module.llm_chat", "No valid response field found in JSON");
        return "Error: Invalid response format";
    }
    catch (const json::parse_error& e) {
        LOG_ERROR("module.llm_chat", "JSON parse error: {}", e.what());
        return "Error: Failed to parse response";
    }
    catch (const std::exception& e) {
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
    std::vector<std::string> knowledge_base;
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
            LLMChatLogger::LogError(Acore::StringFormat(
                "Failed to open personality file: {}", filename));
            return false;
        }

        json data = json::parse(file);
        g_personalities.clear();
        
        if (data.contains("personalities") && data["personalities"].is_array()) {
            for (const auto& item : data["personalities"]) {
                try {
                    Personality personality;
                    personality.id = item["id"].get<std::string>();
                    personality.name = item["name"].get<std::string>();
                    personality.prompt = item["prompt"].get<std::string>();
                    personality.emotions = item["emotions"].get<std::vector<std::string>>();
                    personality.traits = item["traits"];
                    personality.knowledge_base = item["knowledge_base"].get<std::vector<std::string>>();
                    personality.chat_style = item["chat_style"];
                    g_personalities.push_back(personality);
                }
                catch (const std::exception& e) {
                    LLMChatLogger::LogError(Acore::StringFormat(
                        "Error parsing personality: {}", e.what()));
                    continue;
                }
            }
        }

        LLMChatLogger::Log(2, Acore::StringFormat(
            "Loaded {} personalities from {}", 
            g_personalities.size(), filename));
        return true;
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError(Acore::StringFormat(
            "Error loading personalities: {}", e.what()));
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

// Modify QueryLLM to be safer
std::string QueryLLM(std::string const& message, const std::string& senderName, const std::string& responderName)
{
    if (g_moduleShutdown) {
        return "";
    }

    // Check active API calls
    if (g_activeApiCalls >= LLM_Config.Performance.Threading.MaxApiCalls) {
        LLMChatLogger::LogError(Acore::StringFormat(
            "Too many active API calls ({}), skipping request", 
            g_activeApiCalls.load()));
        return "";
    }

    if (message.empty() || senderName.empty() || responderName.empty()) {
        LLMChatLogger::LogError("Empty message, sender name, or responder name");
        return "";
    }

    // Increment active API calls
    ++g_activeApiCalls;

    try {
        // Create a promise to handle the async result
        std::promise<std::string> responsePromise;
        auto responseFuture = responsePromise.get_future();

        // Launch API request in a separate thread
        std::thread apiThread([message, senderName, responderName, &responsePromise]() {
            try {
                // Get conversation history between sender and responder
                std::string conversationHistory = GetConversationContext(senderName, responderName);
                
                // Detect emotion and select appropriate personality
                std::string emotion = DetectEmotion(message);
                Personality personality = SelectPersonality(emotion);
                
                LLMChatLogger::LogDebug(Acore::StringFormat(
                    "Detected emotion: {}, Selected personality: {}", 
                    emotion.c_str(), personality.name.c_str()));

                // Create context with selected personality and conversation history
        std::string contextPrompt = 
                    personality.prompt + "\n\n" +
                    "You are responding as " + responderName + " to " + senderName + "\n" +
                    conversationHistory +
                    "Additional guidelines:\n"
                    "- You are a real person playing WoW who can discuss any topic naturally\n"
                    "- Feel free to share personal opinions and experiences beyond just the game\n"
                    "- You can discuss real-world topics, jokes, or general conversation\n"
                    "- Don't force every response to be about WoW or lore\n"
                    "- Be natural and casual, like a real player chatting while gaming\n"
                    "- You can reference pop culture, current events, or other games\n"
                    "- If referring to the player, use their name: " + senderName + "\n\n"
                    "Chat naturally with " + senderName + " about: " + message;

                LLMChatLogger::LogDebug(Acore::StringFormat("Context prompt: {}", contextPrompt.c_str()));

                // Create the request payload using OpenAI format
        json requestJson = {
                    {"model", LLM_Config.Model},
                    {"messages", {
                        {
                            {"role", "system"},
                            {"content", contextPrompt}
                        },
                        {
                            {"role", "user"},
                            {"content", message}
                        }
                    }},
                    {"temperature", LLM_Config.LLM.Temperature},
                    {"max_tokens", LLM_Config.LLM.NumPredict},
                    {"top_p", LLM_Config.LLM.TopP},
                    {"frequency_penalty", 0.0},
                    {"presence_penalty", LLM_Config.LLM.RepeatPenalty},
                    {"stop", {"\n\n", "Human:", "Assistant:", "[", "<"}},
                    {"stream", false}
                };

        // Set up the IO context
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
                LLMChatLogger::LogDebug(Acore::StringFormat(
                    "Attempting to connect to {}:{}", 
                    LLM_Config.Host.c_str(), LLM_Config.Port.c_str()));

                // Make the connection with timeout
        beast::error_code ec;
        stream.connect(results, ec);
                if (ec) {
                    responsePromise.set_value("Error: Cannot connect to LLM service. Please check if the service is running.");
                    return;
                }

                // Set up the HTTP request
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");

                // Add authentication if provided
                if (!LLM_Config.ApiKey.empty()) {
                    req.set("Authorization", "Bearer " + LLM_Config.ApiKey);
                }
                if (!LLM_Config.ApiSecret.empty()) {
                    req.set("X-API-Secret", LLM_Config.ApiSecret);
                }

                // Set the request body
                req.body() = requestJson.dump();
        req.prepare_payload();

        // Send the HTTP request
        http::write(stream, req, ec);
                if (ec) {
                    responsePromise.set_value("Error: Failed to send request to LLM service");
                    return;
                }

        // This buffer is used for reading
        beast::flat_buffer buffer;
        http::response<http::string_body> res;

                // Receive the HTTP response with timeout
        http::read(stream, buffer, res, ec);
                if (ec) {
                    responsePromise.set_value("Error: Failed to get response from LLM service");
                    return;
                }

        // Gracefully close the socket
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

                if (res.result() != http::status::ok) {
                    std::string error = "Error: LLM service error - " + 
                        std::to_string(static_cast<int>(res.result())) +
                        " - " + res.body();
                    responsePromise.set_value(error);
                    return;
        }

        std::string response = ParseLLMResponse(res.body());

                // After getting response, store in memory
                if (!response.empty()) {
                    AddToMemory(senderName, responderName, message, response);
                }

                responsePromise.set_value(response);
            }
            catch (const std::exception& e) {
                LLMChatLogger::LogError(Acore::StringFormat("API thread error: {}", e.what()));
                responsePromise.set_value("");
            }
            
            // Always decrement active calls
            --g_activeApiCalls;
        });

        // Detach the thread
        apiThread.detach();

        // Wait for response with timeout
        if (responseFuture.wait_for(std::chrono::seconds(LLM_Config.Performance.Threading.ApiTimeout)) == std::future_status::timeout) {
            LLMChatLogger::LogError("API request timed out");
            --g_activeApiCalls;
            return "";
        }

        return responseFuture.get();
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError(Acore::StringFormat("QueryLLM error: {}", e.what()));
        --g_activeApiCalls;
        return "";
    }
}

// Add before SendAIResponse function
bool IsOnCooldown(Player* bot) {
    if (!bot) return true;

    uint32 currentTime = getMSTime();

    // Check global cooldown
    if (currentTime - lastGlobalResponse < (LLM_Config.Performance.Cooldowns.Global * 1000))
            return true;

    // Check individual bot cooldown
    auto it = botCooldowns.find(bot->GetGUID());
    if (it != botCooldowns.end()) {
        if (currentTime - it->second < (LLM_Config.Performance.Cooldowns.Player * 1000))
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

// Add to SendAIResponse before processing
bool CanProcessMessage(Player* sender) {
    if (!sender)
        return false;

    uint32 currentTime = getMSTime();

    // Global rate limiting
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        
        // Reset counter if window has passed
        if (currentTime - globalRateLimit.lastMessageTime > LLM_Config.Performance.GlobalRateLimit.WindowSize) {
            globalRateLimit.messageCount = 0;
            globalRateLimit.lastMessageTime = currentTime;
        }
        
        // Check if we're over the limit
        if (globalRateLimit.messageCount >= LLM_Config.Performance.GlobalRateLimit.MaxMessages) {
            LOG_DEBUG("module.llm_chat", "Global rate limit reached, skipping message");
            return false;
        }
        
        globalRateLimit.messageCount++;
    }

    // Per-player rate limiting
    static std::map<ObjectGuid, uint32> playerLastMessage;

    if (playerLastMessage.count(sender->GetGUID()) > 0) {
        if (currentTime - playerLastMessage[sender->GetGUID()] < LLM_Config.Performance.Cooldowns.Player) {
            LOG_DEBUG("module.llm_chat", "Player cooldown active for {}, skipping message", 
                sender->GetName());
            return false;
        }
    }
    playerLastMessage[sender->GetGUID()] = currentTime;

        return true;
    }

// Modify SendAIResponse to be safer
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team) {
    if (g_moduleShutdown || !LLM_Config.Enabled) {
        return;
    }

    try {
        if (!sender || !sender->IsInWorld()) {
            return;
        }

        // Check rate limits first
        if (!CanProcessMessage(sender)) {
            LLMChatLogger::LogDebug(Acore::StringFormat(
                "Rate limit exceeded for player {}", sender->GetName()));
            return;
        }

        Map* map = sender->GetMap();
        if (!map) {
            return;
        }

        // Use a shared lock for thread safety
        std::lock_guard<std::mutex> lock(g_stateMutex);

        // Filter out short or spammy messages
        if (msg.length() < LLM_Config.Performance.MessageLimits.Min || 
            msg.length() > LLM_Config.Performance.MessageLimits.Max) {
            LLMChatLogger::LogDebug("Message length outside acceptable range, skipping");
            return;
        }

        // Check for message spam/repetition
        static std::map<ObjectGuid, std::string> lastPlayerMessage;
        if (lastPlayerMessage.count(sender->GetGUID()) > 0) {
            if (lastPlayerMessage[sender->GetGUID()] == msg) {
                LLMChatLogger::LogDebug(Acore::StringFormat(
                    "Repeated message from {}, skipping", sender->GetName()));
                return;
            }
        }
        lastPlayerMessage[sender->GetGUID()] = msg;

        // Queue the response
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (responseQueue.size() >= LLM_Config.Queue.Size / 2) {
                LLMChatLogger::LogDebug(Acore::StringFormat(
                    "Response queue getting full, skipping response for {}", 
                    sender->GetName()));
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
    catch (const std::exception& e) {
        LLMChatLogger::LogError(Acore::StringFormat(
            "Error in SendAIResponse: {}", e.what()));
    }
}

class LLMChatAnnounce : public PlayerScript
{
public:
    LLMChatAnnounce() : PlayerScript("LLMChatAnnounce") {}

    void OnLogin(Player* player) override
    {
        try {
            if (!player || !player->IsInWorld()) {
                return;
            }

            LLMChatLogger::Log(2, Acore::StringFormat(
                "Player {} logging in - checking LLM Chat module status", 
                player->GetName()));

            if (!LLM_Config.Enabled) {
                LLMChatLogger::LogDebug(Acore::StringFormat(
                    "LLM Chat module is disabled - skipping initialization for {}", 
                    player->GetName()));
                return;
            }

            if (g_personalities.empty()) {
                LLMChatLogger::LogError("No personalities loaded - module may not function correctly");
                return;
            }

            if (sConfigMgr->GetOption<int32>("LLMChat.Announce", 0)) {
                ChatHandler(player->GetSession()).SendSysMessage("This server is running the LLM Chat module.");
            }

            LLMChatLogger::Log(2, Acore::StringFormat(
                "Player {} successfully initialized with LLM Chat module", 
                player->GetName()));
        }
        catch (const std::exception& e) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Error during player login for {}: {}", 
                player->GetName(), e.what()));
        }
    }
};

class LLMChatConfig : public WorldScript
{
public:
    LLMChatConfig() : WorldScript("LLMChatConfig") {}

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        LLMChatLogger::Log(1, "Loading LLM Chat configuration...");
        
        // Core Settings
        LLM_Config.Enabled = sConfigMgr->GetOption<bool>("LLMChat.Enable", false);
        LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLMChat.LogLevel", 2);
        LLM_Config.Announce = sConfigMgr->GetOption<bool>("LLMChat.Announce", false);

        // Only continue logging if not disabled
        if (LLM_Config.LogLevel > 0) {
            LLMChatLogger::Log(2, "=== LLM Chat Configuration ===");
            
            // Provider Settings
            LLM_Config.Endpoint = sConfigMgr->GetOption<std::string>("LLMChat.Endpoint", "http://localhost:11434/api/generate");
            LLM_Config.Model = sConfigMgr->GetOption<std::string>("LLMChat.Model", "socialnetwooky/llama3.2-abliterated:1b_q8");
            LLM_Config.ApiKey = sConfigMgr->GetOption<std::string>("LLMChat.ApiKey", "");
            LLM_Config.ApiSecret = sConfigMgr->GetOption<std::string>("LLMChat.ApiSecret", "");

            // Parse endpoint URL
            ParseEndpointURL(LLM_Config.Endpoint, LLM_Config);

            // Chat Behavior
            LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLMChat.ChatRange", 25.0f);
            LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLMChat.ResponsePrefix", "");
            LLM_Config.MaxResponsesPerMessage = sConfigMgr->GetOption<uint32>("LLMChat.MaxResponsesPerMessage", 1);
            LLM_Config.ResponseChance = sConfigMgr->GetOption<uint32>("LLMChat.ResponseChance", 30);

            // Performance & Rate Limiting
            LLM_Config.Performance.GlobalRateLimit.WindowSize = sConfigMgr->GetOption<uint32>("LLMChat.Performance.GlobalRateLimit.WindowSize", 10000);
            LLM_Config.Performance.GlobalRateLimit.MaxMessages = sConfigMgr->GetOption<uint32>("LLMChat.Performance.GlobalRateLimit.MaxMessages", 5);
            
            LLM_Config.Performance.Cooldowns.Player = sConfigMgr->GetOption<uint32>("LLMChat.Performance.PlayerCooldown", 10000);
            LLM_Config.Performance.Cooldowns.Bot = sConfigMgr->GetOption<uint32>("LLMChat.Performance.BotCooldown", 15000);
            LLM_Config.Performance.Cooldowns.Global = sConfigMgr->GetOption<uint32>("LLMChat.Performance.GlobalCooldown", 5000);
            
            LLM_Config.Performance.Threading.MaxThreads = sConfigMgr->GetOption<uint32>("LLMChat.Performance.MaxConcurrentThreads", 2);
            LLM_Config.Performance.Threading.MaxApiCalls = sConfigMgr->GetOption<uint32>("LLMChat.Performance.MaxActiveApiCalls", 5);
            MAX_API_CALLS = LLM_Config.Performance.Threading.MaxApiCalls; // Update the global variable
            LLM_Config.Performance.Threading.ApiTimeout = sConfigMgr->GetOption<uint32>("LLMChat.Performance.ApiTimeout", 3);
            
            LLM_Config.Performance.MessageLimits.Min = sConfigMgr->GetOption<uint32>("LLMChat.Performance.MinMessageLength", 5);
            LLM_Config.Performance.MessageLimits.Max = sConfigMgr->GetOption<uint32>("LLMChat.Performance.MaxMessageLength", 200);
            
            LLM_Config.Performance.Delays.Min = sConfigMgr->GetOption<uint32>("LLMChat.Performance.ResponseDelay.Min", 2000);
            LLM_Config.Performance.Delays.Max = sConfigMgr->GetOption<uint32>("LLMChat.Performance.ResponseDelay.Max", 1500);
            LLM_Config.Performance.Delays.Pacified = sConfigMgr->GetOption<uint32>("LLMChat.Performance.BotPacifiedDuration", 5000);

            // Queue Settings
            LLM_Config.Queue.Size = sConfigMgr->GetOption<uint32>("LLMChat.Queue.Size", 25);
            LLM_Config.Queue.Timeout = sConfigMgr->GetOption<uint32>("LLMChat.Queue.Timeout", 180);

            // LLM Parameters
            LLM_Config.LLM.Temperature = sConfigMgr->GetOption<float>("LLMChat.LLM.Temperature", 0.85f);
            LLM_Config.LLM.TopP = sConfigMgr->GetOption<float>("LLMChat.LLM.TopP", 0.9f);
            LLM_Config.LLM.NumPredict = sConfigMgr->GetOption<uint32>("LLMChat.LLM.NumPredict", 2048);
            LLM_Config.LLM.ContextSize = sConfigMgr->GetOption<uint32>("LLMChat.LLM.ContextSize", 4096);
            LLM_Config.LLM.RepeatPenalty = sConfigMgr->GetOption<float>("LLMChat.LLM.RepeatPenalty", 1.2f);

            // Memory System
            LLM_Config.Memory.Enable = sConfigMgr->GetOption<bool>("LLMChat.Memory.Enable", true);
            LLM_Config.Memory.MaxInteractionsPerPair = sConfigMgr->GetOption<uint32>("LLMChat.Memory.MaxInteractionsPerPair", 10);
            LLM_Config.Memory.ExpirationTime = sConfigMgr->GetOption<uint32>("LLMChat.Memory.ExpirationTime", 3600);
            LLM_Config.Memory.MaxContextLength = sConfigMgr->GetOption<uint32>("LLMChat.Memory.MaxContextLength", 2000);

            // Personality System
            LLM_Config.PersonalityFile = sConfigMgr->GetOption<std::string>("LLMChat.PersonalityFile", "mod_llm_chat/conf/personalities.json");

            // Database Settings
            LLM_Config.Database.CharacterDB = sConfigMgr->GetOption<std::string>("LLMChat.Database.Character", "characters");
            LLM_Config.Database.WorldDB = sConfigMgr->GetOption<std::string>("LLMChat.Database.World", "world");
            LLM_Config.Database.AuthDB = sConfigMgr->GetOption<std::string>("LLMChat.Database.Auth", "auth");
            LLM_Config.Database.CustomDB = sConfigMgr->GetOption<std::string>("LLMChat.Database.Custom", "LLMDB");

            // Log database configuration if debug level
            if (LLM_Config.LogLevel >= 3) {
                LLMChatLogger::LogDebug("=== Database Configuration ===");
                LLMChatLogger::LogDebug(Acore::StringFormat("Character DB: {}", LLM_Config.Database.CharacterDB));
                LLMChatLogger::LogDebug(Acore::StringFormat("World DB: {}", LLM_Config.Database.WorldDB));
                LLMChatLogger::LogDebug(Acore::StringFormat("Auth DB: {}", LLM_Config.Database.AuthDB));
                LLMChatLogger::LogDebug(Acore::StringFormat("Custom DB: {}", LLM_Config.Database.CustomDB));
            }

            // Log configuration if debug level
            if (LLM_Config.LogLevel >= 3) {
                LogConfiguration();
            }
        }
    }

private:
    void LogConfiguration() {
        LLMChatLogger::LogDebug("=== Detailed Configuration ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Endpoint: {}", LLM_Config.Endpoint.c_str()));
        LLMChatLogger::LogDebug(Acore::StringFormat("Model: {}", LLM_Config.Model.c_str()));
        LLMChatLogger::LogDebug(Acore::StringFormat("Chat Range: {}.2f", LLM_Config.ChatRange));
        LLMChatLogger::LogDebug(Acore::StringFormat("Max Responses Per Message: {}", LLM_Config.MaxResponsesPerMessage));
        LLMChatLogger::LogDebug(Acore::StringFormat("Response Chance: {}", LLM_Config.ResponseChance));
        LLMChatLogger::LogDebug("=== Performance Settings ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Max Threads: {}", LLM_Config.Performance.Threading.MaxThreads));
        LLMChatLogger::LogDebug(Acore::StringFormat("Max API Calls: {}", LLM_Config.Performance.Threading.MaxApiCalls));
        LLMChatLogger::LogDebug("=== Memory Settings ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Memory Enabled: {}", LLM_Config.Memory.Enable ? "Yes" : "No"));
        LLMChatLogger::LogDebug(Acore::StringFormat("Max Interactions Per Pair: {}", LLM_Config.Memory.MaxInteractionsPerPair));
        LLMChatLogger::LogDebug("=== LLM Parameters ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Temperature: {}.2f", LLM_Config.LLM.Temperature));
        LLMChatLogger::LogDebug(Acore::StringFormat("Context Size: {}", LLM_Config.LLM.ContextSize));
    }
};

class LLMChatPlayerScript : public PlayerScript
{
public:
    LLMChatPlayerScript() : PlayerScript("LLMChatPlayerScript") {}

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        try {
            if (!LLM_Config.Enabled) {
                return;
            }

            if (!player || !player->IsInWorld() || msg.empty() || msg.length() < 2) {
                    return;
            }

            // Skip if this is an AI response to prevent loops
            if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0) {
                    return;
            }

            // Log the chat event
            LLMChatLogger::LogDebug(Acore::StringFormat(
                "Processing chat message from {} (type: {}): {}", 
                player->GetName(), GetChatTypeString(type), msg));

            // Add a small delay before processing
            uint32 delay = urand(100, 500);

            // Create and add the event with safety checks
            if (player && player->IsInWorld()) {
                player->m_Events.AddEvent(new TriggerResponseEvent(player, msg, type), 
                    player->m_Events.CalculateTime(delay));
                LOG_DEBUG("module.llm_chat", "Added chat response event for {} with {}ms delay", 
                    player->GetName(), delay);
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("module.llm_chat", "Error processing chat for {}: {}", 
                player->GetName(), e.what());
        }
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        if (!LLM_Config.Enabled || !player || !player->IsInWorld() || !channel || msg.empty() || msg.length() < 2)
                    return;
      
        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
        return;

        std::string logMsg = Acore::StringFormat("[Chat] Player '{}' says in channel '{}': {}", 
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

        std::string logMsg = Acore::StringFormat("[Chat] Player '{}' says in group: {}", 
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

        std::string logMsg = Acore::StringFormat("[Chat] Player '{}' says in guild '{}': {}", 
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
        g_moduleShutdown = false;
        lastMemoryCleanup = getMSTime();
        
        // Start worker thread
        g_workerRunning = true;
        g_workerThread = std::thread(WorkerThread);
        
        std::string personalityFile = sConfigMgr->GetOption<std::string>("LLMChat.PersonalityFile", 
            "mod_llm_chat/conf/personalities.json");
        if (!LoadPersonalities(personalityFile)) {
            LOG_ERROR("module.llm_chat", "Failed to load personalities!");
        }
        
        LOG_INFO("module.llm_chat", "Worker thread started");
    }

    void OnShutdown() override {
        LLMChatLogger::Log(1, "Initiating module shutdown...");
        
        // Signal shutdown
        g_moduleShutdown = true;
        queueCondition.notify_one();
        
        // Wait for worker thread to finish
        if (g_workerThread.joinable()) {
            g_workerThread.join();
        }
        
        // Wait for any remaining API calls
        uint32 waitTime = 0;
        while (g_activeApiCalls > 0 && waitTime < LLM_Config.Performance.Threading.ApiTimeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waitTime += 100;
        }
        
        LLMChatLogger::Log(1, Acore::StringFormat(
            "Module shutdown complete. Active calls remaining: {}", 
            g_activeApiCalls.load()));
    }

    void OnUpdate(uint32 diff) override {
        if (!LLM_Config.Memory.Enable) {
            return;
        }

        // Periodic memory cleanup (every hour)
        uint32 currentTime = getMSTime();
        if (currentTime - lastMemoryCleanup > 3600000) {  // 1 hour in milliseconds
            CleanupExpiredMemories();
            lastMemoryCleanup = currentTime;
            
            // Log memory stats
            std::lock_guard<std::mutex> lock(g_memoryMutex);
            size_t totalMemories = 0;
            for (const auto& pair : g_conversationHistory) {
                totalMemories += pair.second.size();
            }
            LLMChatLogger::Log(2, Acore::StringFormat(
                "Memory cleanup complete. Active conversations: {}, Total memories: {}", 
                g_conversationHistory.size(), totalMemories));
        }
    }

private:
    uint32 lastMemoryCleanup;
};

void Add_LLMChatScripts()
{
    new LLMChat_WorldScript();
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatPlayerScript();
} 

void LoadConfig()
{
    if (!sConfigMgr->LoadModulesConfig("mod_llm_chat.conf")) {
        LOG_ERROR("module", "LLM Chat: Failed to load configuration.");
        return;
    }

    // Load personalities
    std::string personalitiesFile = sConfigMgr->GetStringDefault("LLMChat.PersonalitiesFile", "conf/personalities.json");
    if (!LLMChatPersonality::LoadPersonalities(personalitiesFile)) {
        LOG_ERROR("module", "LLM Chat: Failed to load personalities from %s", personalitiesFile.c_str());
    }

    // ... rest of config loading ...
} 