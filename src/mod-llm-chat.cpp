/*
** Made by Krazor
** AzerothCore 2019 http://www.azerothcore.org/
** Based on LLM Chat integration
*/

#include "mod-llm-chat.h"
#include "LLMChatEvents.h"
#include "LLMChatPersonality.h"
#include "LLMChatLogger.h"
#include "LLMChatDB.h"
#include "LLMChatQueue.h"
#include "LLMChatMemory.h"

// AzerothCore includes
#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "World.h"
#include "Channel.h"
#include "Guild.h"
#include "ChannelMgr.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Cell.h"
#include "Map.h"
#include "ObjectGridLoader.h"
#include "GridDefines.h"
#include "TypeContainerVisitor.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "CharacterCache.h"

// Playerbot includes
#include "strategy/Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotAIConfig.h"

// Standard includes
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
#include <filesystem>
#include <chrono>

// Boost includes
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

// JSON library
#include <nlohmann/json.hpp>

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "World.h"
#include "Channel.h"
#include "Guild.h"
#include "ChannelMgr.h"
#include "mod-llm-chat.h"

#include "strategy/Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotAIConfig.h"

#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "LLMChatEvents.h"
#include "LLMChatPersonality.h"
#include "LLMChatLogger.h"
#include "LLMChatDB.h"
#include "LLMChatQueue.h"
#include "LLMChatMemory.h"
#include "Cell.h"
#include "Map.h"
#include "ObjectGridLoader.h"
#include "GridDefines.h"
#include "TypeContainerVisitor.h"
#include "GridNotifiers.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "CharacterCache.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

// Use existing chrono types
#include <chrono>
using namespace std::chrono;

// Config Variables
LLMConfig LLM_Config;  // Actual definition

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

// Forward declarations
uint32 GetNearbyPlayerCount(Player* player);
bool IsInCombat(Player* player);

// Helper function to get PlayerbotAI safely
inline PlayerbotAI* GetBotAI(Player* player) {
    if (!player || !player->GetSession())
        return nullptr;
        
    try {
        return sPlayerbotsMgr->GetPlayerbotAI(player);
    } catch (...) {
        return nullptr;
    }
}

inline bool IsBot(Player* player) {
    if (!player || !player->GetSession()) 
        return false;
    
    // First check if it's a playerbot
    if (PlayerbotAI* botAI = GetBotAI(player)) {
        return !botAI->IsRealPlayer();
    }

    // Also check if the session is marked as a bot
    if (player->GetSession()->IsBot()) {
        return true;
    }

    return false;
}

// Helper function to check if a player is a real player (not a bot)
inline bool IsRealPlayer(Player* player) {
    return player && !IsBot(player);
}

namespace {
    // Thread safety and control
    std::atomic<bool> g_moduleShutdown{false};
    std::atomic<uint32> g_activeApiCalls{0};
    uint32 MAX_API_CALLS = 5;

    // Constants
    constexpr uint32 RESPONSE_TIMEOUT = 10000; // 10 seconds timeout

    // Renamed to avoid conflict with LLMChatQueue.h
    struct LocalQueuedResponse {
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
    std::queue<LocalQueuedResponse> responseQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;

    // Worker thread control
    std::thread g_workerThread;
    bool g_workerRunning = false;
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

            // Safety check for empty queue after wait
            if (responseQueue.empty()) {
                continue;
            }

            // Process the front of the queue
            LocalQueuedResponse& queuedResponse = responseQueue.front();
                
                // Check if response has timed out
                uint32 currentTime = getMSTime();
            if (currentTime - queuedResponse.timestamp > RESPONSE_TIMEOUT) {
                    LOG_DEBUG("module.llm_chat", "Response timed out for {} after {}", 
                    queuedResponse.sender ? queuedResponse.sender->GetName() : "Unknown", 
                    LLM_Config.Queue.Timeout);
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
                
            // Debug log to see if we're getting here
            LLMChatLogger::Log(1, Acore::StringFormat(
                "WorkerThread processing request - Sender: {}, Bot: {}, Message: {}", 
                queuedResponse.sender ? queuedResponse.sender->GetName() : "Unknown",
                currentResponder ? currentResponder->GetName() : "Unknown",
                queuedResponse.message.c_str()));

            try {
                // Set up the API request
                asio::io_context ioc;
                tcp::resolver resolver(ioc);
                beast::tcp_stream stream(ioc);

                // Debug log the connection attempt
                LLMChatLogger::Log(1, Acore::StringFormat(
                    "Attempting Ollama connection - Host: {}, Port: {}, Target: {}", 
                    LLM_Config.Host.c_str(), LLM_Config.Port.c_str(), LLM_Config.Target.c_str()));

                auto const results = resolver.resolve(LLM_Config.Host, LLM_Config.Port);
                stream.connect(results);

                http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
                req.set(http::field::host, LLM_Config.Host);
                req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                req.set(http::field::content_type, "application/json");

                // Create the request body with more context
                nlohmann::json request_body = {
                    {"model", LLM_Config.Model},
                    {"prompt", Acore::StringFormat(
                        "You are a character named {} in World of Warcraft.\n"
                        "Current location: {} (Map: {}, Zone: {})\n"
                        "Your level: {}, Class: {}, Race: {}\n"
                        "Nearby players: {}\n"
                        "Combat state: {}\n"
                        "\nRespond to this message from {} (Level {} {} {}): {}\n"
                        "\nKeep your response in character and relevant to World of Warcraft lore.",
                        currentResponder->GetName(),
                        (sAreaTableStore.LookupEntry(currentResponder->GetZoneId()) ? sAreaTableStore.LookupEntry(currentResponder->GetZoneId())->area_name[0] : "Unknown Zone"),
                        currentResponder->GetMapId(),
                        currentResponder->GetZoneId(),
                        currentResponder->GetLevel(),
                        LLMChatPersonality::GetCharacterDetails(currentResponder).class_type.c_str(),
                        LLMChatPersonality::GetCharacterDetails(currentResponder).race.c_str(),
                        GetNearbyPlayerCount(currentResponder),
                        IsInCombat(currentResponder) ? "In Combat" : "Not in Combat",
                        queuedResponse.sender->GetName(),
                        queuedResponse.sender->GetLevel(),
                        LLMChatPersonality::GetCharacterDetails(queuedResponse.sender).class_type.c_str(),
                        LLMChatPersonality::GetCharacterDetails(queuedResponse.sender).race.c_str(),
                        queuedResponse.message.c_str())},
                    {"stream", false}
                };

                std::string request_str = request_body.dump();
                LLMChatLogger::Log(1, Acore::StringFormat("Sending Ollama request: {}", request_str.c_str()));

                req.body() = request_str;
                req.prepare_payload();

                // Send the request and log the attempt
                LLMChatLogger::Log(1, "Sending request to Ollama API...");
                http::write(stream, req);

                // Get the response
                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(stream, buffer, res);

                LLMChatLogger::Log(1, Acore::StringFormat(
                    "Ollama API Response - Status: {}, Body: {}", 
                    res.result_int(), res.body().c_str()));

                // Parse the response
                try {
                    json response_json = json::parse(res.body());
                    if (response_json.contains("response")) {
                        std::string response = response_json["response"].get<std::string>();
        if (!response.empty()) {
                            // Schedule the response
                            uint32 delay = urand(2000, 3500);
                            
                            // Create and schedule the event
            BotResponseEvent* event = new BotResponseEvent(
                currentResponder, 
                                queuedResponse.sender,
                response, 
                                queuedResponse.chatType,
                                queuedResponse.message,
                                queuedResponse.team,
                LLM_Config.Performance.Delays.Pacified
            );

            if (currentResponder && currentResponder->IsInWorld()) {
                currentResponder->m_Events.AddEvent(event, 
                    currentResponder->m_Events.CalculateTime(delay));
            } else {
                delete event;
            }
        }
                    } else {
                        LLMChatLogger::LogError("Ollama response missing 'response' field");
                    }
                } catch (const json::parse_error& e) {
                    LLMChatLogger::LogError(Acore::StringFormat(
                        "Failed to parse Ollama response: {}", e.what()));
                }

                // Increment the responses generated count
                queuedResponse.responsesGenerated++;
            } catch (const std::exception& e) {
                LLMChatLogger::LogError(Acore::StringFormat(
                    "Error making Ollama API request: {}", e.what()));
            }
        }

        // Small delay to prevent tight loop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Forward declarations
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team);
std::string QueryLLM(std::string const& message, const std::string& senderName, const std::string& responderName);
void AddToMemory(const std::string& sender, const std::string& responder, const std::string& message, const std::string& response);

// Add a conversation counter
std::map<std::string, uint32> conversationRounds;  // Key: channelName+originalMessage, Value: round count
std::map<ObjectGuid, uint32> botCooldowns;  // Track individual bot cooldowns
uint32 lastGlobalResponse = 0;  // Track last global response time

// Add to the global variables section
struct GlobalRateLimit {
    uint32 lastMessageTime{0};
    uint32 messageCount{0};
} globalRateLimit;

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
    
    if (history.size() > MAX_MEMORY_PER_PAIR) {
        history.resize(MAX_MEMORY_PER_PAIR);
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
            LOG_ERROR("module.llm_chat", "Invalid endpoint URL (no protocol): {}", endpoint.c_str());
            return;
        }

        std::string url = endpoint.substr(protocolEnd + 3);
        size_t pathStart = url.find('/');
        if (pathStart == std::string::npos) {
            LOG_ERROR("module.llm_chat", "Invalid endpoint URL (no path): {}", endpoint.c_str());
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
                    LOG_ERROR("module.llm_chat", "Invalid port number: {}", config.Port.c_str());
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
                config.Host.c_str(), config.Port.c_str(), config.Target.c_str());
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
        LOG_DEBUG("module.llm_chat", "Parsing raw response: {}", rawResponse.c_str());
        
        json response_json = json::parse(rawResponse);
        
        // Check for Ollama format
        if (response_json.contains("response")) {
            std::string response = response_json["response"].get<std::string>();
            LOG_DEBUG("module.llm_chat", "Parsed Ollama response: {}", response.c_str());
            return response;
        }
        
        // Check for OpenAI format
        if (response_json.contains("choices") && !response_json["choices"].empty()) {
            if (response_json["choices"][0].contains("message") && 
                response_json["choices"][0]["message"].contains("content")) {
                std::string response = response_json["choices"][0]["message"]["content"].get<std::string>();
                LOG_DEBUG("module.llm_chat", "Parsed OpenAI response: {}", response.c_str());
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

// Global variables
std::vector<Personality> g_personalities;
json g_emotion_types;

// Load personalities from JSON file
bool LoadPersonalities(std::string const& filename) {
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Failed to open personality file: {}", filename.c_str()));
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
                    personality.base_context = item["base_context"].get<std::string>();
                    
                    // Load emotions array
                    personality.emotions = item["emotions"].get<std::vector<std::string>>();
                    
                    // Load traits
                    auto traits = item["traits"];
                    for (auto it = traits.begin(); it != traits.end(); ++it) {
                        personality.traits[it.key()] = it.value();
                    }

                    // Load chat style
                    personality.chat_style.typical_phrases = 
                        item["chat_style"]["typical_phrases"].get<std::vector<std::string>>();

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
            g_personalities.size(), filename.c_str()));
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
        dominantEmotion.c_str(), message.c_str());
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
        asio::io_context ioc;
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
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team) 
{
    try {
        if (!sender || !sender->IsInWorld()) {
            return;
        }

        LLMChatLogger::Log(1, Acore::StringFormat(
            "SendAIResponse called - Sender: {}, Message: {}, ChatType: {}", 
            sender->GetName(), msg.c_str(), chatType));

        // Get nearby bots
        std::list<Player*> nearbyBots;
        try {
            // Get players in range
        Map* map = sender->GetMap();
        if (!map) {
            return;
        }

            // Get nearby players based on chat type
            float searchRange = (chatType == CHAT_MSG_SAY) ? 25.0f : 
                              (chatType == CHAT_MSG_YELL) ? 300.0f : 
                              LLM_Config.ChatRange;

            map->DoForAllPlayers([&nearbyBots, sender, searchRange](Player* player) {
                if (player && player != sender && 
                    player->IsInWorld() && 
                    player->GetSession()) 
                {
                    PlayerbotAI* botAI = GetBotAI(player);
                    if (!botAI || botAI->IsRealPlayer()) {
                        return;  // Skip real players
                    }

                    // For say/yell, check distance
                    if (player->IsWithinDist(sender, searchRange)) {
                        LLMChatLogger::Log(1, Acore::StringFormat(
                            "Found nearby bot: {} at distance %.2f", 
                            player->GetName(),
                            player->GetDistance(sender)));
                        nearbyBots.push_back(player);
                    }
                }
            });

        } catch (const std::exception& e) {
            LLMChatLogger::LogError(Acore::StringFormat("Error finding nearby players: {}", e.what()));
            return;
        }

        if (nearbyBots.empty()) {
            LLMChatLogger::Log(1, "No nearby bots found to respond");
                return;
        }

        // Limit the number of bots to prevent overload
        if (nearbyBots.size() > 10) {  // Arbitrary limit
            // Convert list to vector for shuffling
            std::vector<Player*> tempBots(nearbyBots.begin(), nearbyBots.end());
            
            // Use modern shuffle with a random device
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(tempBots.begin(), tempBots.end(), g);
            
            // Clear list and add back first 10 elements
            nearbyBots.clear();
            for (size_t i = 0; i < 10 && i < tempBots.size(); ++i) {
                nearbyBots.push_back(tempBots[i]);
            }
        }

        // Create queued response
        LocalQueuedResponse queuedResponse;
        queuedResponse.timestamp = getMSTime();
        queuedResponse.sender = sender;
        queuedResponse.message = msg;
        queuedResponse.chatType = chatType;
        queuedResponse.team = team;
        queuedResponse.responsesGenerated = 0;
        queuedResponse.maxResponses = std::min(
            static_cast<size_t>(LLM_Config.MaxResponsesPerMessage), 
            nearbyBots.size()
        );

        // Add responders
        for (Player* bot : nearbyBots) {
            queuedResponse.responders.push_back(bot);
        }

        // Add to queue
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            // Check queue size to prevent memory issues
            if (responseQueue.size() >= LLM_Config.Queue.Size) {
                LLMChatLogger::LogError("Response queue full, dropping request");
                return;
            }
            responseQueue.push(queuedResponse);
            queueCondition.notify_one();
        }
    } catch (const std::exception& e) {
        LLMChatLogger::LogError(Acore::StringFormat("Error in SendAIResponse: {}", e.what()));
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
                LLMChatLogger::LogDebug(Acore::StringFormat("Character DB: {}", LLM_Config.Database.CharacterDB.c_str()));
                LLMChatLogger::LogDebug(Acore::StringFormat("World DB: {}", LLM_Config.Database.WorldDB.c_str()));
                LLMChatLogger::LogDebug(Acore::StringFormat("Auth DB: {}", LLM_Config.Database.AuthDB.c_str()));
                LLMChatLogger::LogDebug(Acore::StringFormat("Custom DB: {}", LLM_Config.Database.CustomDB.c_str()));
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
        LLMChatLogger::LogDebug(Acore::StringFormat("Chat Range: %.2f", LLM_Config.ChatRange));
        LLMChatLogger::LogDebug(Acore::StringFormat("Max Responses Per Message: {}", LLM_Config.MaxResponsesPerMessage));
        LLMChatLogger::LogDebug(Acore::StringFormat("Response Chance: {}", LLM_Config.ResponseChance));
        LLMChatLogger::LogDebug("=== Performance Settings ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Max Threads: {}", LLM_Config.Performance.Threading.MaxThreads));
        LLMChatLogger::LogDebug(Acore::StringFormat("Max API Calls: {}", LLM_Config.Performance.Threading.MaxApiCalls));
        LLMChatLogger::LogDebug("=== Memory Settings ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Memory Enabled: {}", LLM_Config.Memory.Enable ? "Yes" : "No"));
        LLMChatLogger::LogDebug(Acore::StringFormat("Max Interactions Per Pair: {}", LLM_Config.Memory.MaxInteractionsPerPair));
        LLMChatLogger::LogDebug("=== LLM Parameters ===");
        LLMChatLogger::LogDebug(Acore::StringFormat("Temperature: %.2f", LLM_Config.LLM.Temperature));
        LLMChatLogger::LogDebug(Acore::StringFormat("Context Size: {}", LLM_Config.LLM.ContextSize));
    }
};

class LLMChatPlayerScript : public PlayerScript
{
public:
    LLMChatPlayerScript() : PlayerScript("LLMChatPlayerScript") {}

    void OnLogin(Player* player) override 
    {
        if (!player) return;

        static bool initialLoad = false;
        static std::mutex loadMutex;
        std::lock_guard<std::mutex> lock(loadMutex);

        LLMChatLogger::Log(1, Acore::StringFormat(
            "Player {} logging in - checking LLM Chat module status", player->GetName()));

        // Get config path
        std::string currentPath = std::filesystem::current_path().string();
        std::string configPath = currentPath + "/../../../modules/mod_llm_chat/conf/personalities.json";

        // Check current personality count first
        size_t currentCount = LLMChatPersonality::GetLoadedPersonalitiesCount();
        if (initialLoad && currentCount > 0) {
            LLMChatLogger::Log(1, Acore::StringFormat(
                "Module ready - {} personalities loaded", currentCount));
            return;  // Exit if personalities are already loaded
        }

        if (!std::filesystem::exists(configPath)) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Could not find personalities.json at: {}\n"
                "Please ensure the file exists in the conf directory of the module.",
                configPath.c_str()));
            return;
        }

        // Only try to load if we don't have any personalities
        LLMChatLogger::Log(1, "Attempting to load personalities...");
        if (!LLMChatPersonality::LoadPersonalities(configPath)) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Failed to load personalities from {} during player login", configPath.c_str()));
            return;
        }

        // Check if loading was successful
        currentCount = LLMChatPersonality::GetLoadedPersonalitiesCount();
        if (currentCount > 0) {
            initialLoad = true;
            LLMChatLogger::Log(1, Acore::StringFormat(
                "Successfully loaded {} personalities", currentCount));
        } else {
            LLMChatLogger::LogError("No personalities loaded - module may not function correctly");
        }
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        try {
            if (!LLM_Config.Enabled) {
                return;
            }

            if (!player || msg.empty()) {
                    return;
            }

            // Skip if this is an AI response to prevent loops
            if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0) {
                    return;
            }

            LLMChatLogger::LogDebug(Acore::StringFormat(
                "Processing chat message from {} (type: {}): {}", 
                player->GetName(), type, msg.c_str()));

            // Directly send AI response
            SendAIResponse(player, msg, type, player->GetTeamId());
        }
        catch (const std::exception& e) {
            LOG_ERROR("module.llm_chat", "Error processing chat for {}: {}", 
                player->GetName(), e.what());
        }
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        if (!LLM_Config.Enabled || !player || !channel || msg.empty())
                    return;
      
        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
        return;

        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        if (!LLM_Config.Enabled || !player || !group || msg.empty())
        return;

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        SendAIResponse(player, msg, type, player->GetTeamId());
    }

    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* guild) override
    {
        if (!LLM_Config.Enabled || !player || !guild || msg.empty())
            return;   

        // Skip if this is an AI response
        if (!LLM_Config.ResponsePrefix.empty() && msg.find(LLM_Config.ResponsePrefix) == 0)
            return;

        SendAIResponse(player, msg, type, player->GetTeamId());
    }
}; 

class LLMChat_WorldScript : public WorldScript {
public:
    LLMChat_WorldScript() : WorldScript("LLMChat_WorldScript") {}

    void OnStartup() override {
        g_moduleShutdown = false;
        lastMemoryCleanup = getMSTime();
        
        LLMChatLogger::Log(1, "Starting LLM Chat module initialization...");
        
        // Start worker thread
        g_workerRunning = true;
        g_workerThread = std::thread(WorkerThread);
        
        // Get module path and construct personalities file path
        std::string currentPath = std::filesystem::current_path().string();
        std::string modulePath = currentPath + "/../../../modules/mod_llm_chat";
        std::string personalitiesPath = modulePath + "/conf/personalities.json";
        
        LLMChatLogger::Log(1, "=== Path Information ===");
        LLMChatLogger::Log(1, Acore::StringFormat("Current working directory: {}", currentPath.c_str()));
        LLMChatLogger::Log(1, Acore::StringFormat("Module path: {}", modulePath.c_str()));
        LLMChatLogger::Log(1, Acore::StringFormat("Personalities file path: {}", personalitiesPath.c_str()));
        LLMChatLogger::Log(1, Acore::StringFormat("Path exists: {}", 
            std::filesystem::exists(personalitiesPath) ? "Yes" : "No"));
        
        if (std::filesystem::exists(modulePath)) {
            LLMChatLogger::Log(1, "Listing module directory contents:");
            for (const auto& entry : std::filesystem::directory_iterator(modulePath)) {
                LLMChatLogger::Log(1, Acore::StringFormat("  {}", entry.path().string().c_str()));
            }
        }
        
        // Check if file exists before trying to load
        if (!std::filesystem::exists(personalitiesPath)) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Personalities file not found at: {}. Directory contents:", personalitiesPath.c_str()));
            
            // List directory contents
            try {
                for (const auto& entry : std::filesystem::directory_iterator(modulePath + "/conf")) {
                    LLMChatLogger::Log(1, Acore::StringFormat("Found file: {}", entry.path().string().c_str()));
                }
            } catch (const std::exception& e) {
                LLMChatLogger::LogError(Acore::StringFormat("Error listing directory: {}", e.what()));
            }
        }
        
        // Try to load personalities
        if (!LLMChatPersonality::LoadPersonalities(personalitiesPath)) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Failed to load personalities from {}", personalitiesPath.c_str()));
        }
        
        // Log final initialization status
        LLMChatLogger::Log(1, Acore::StringFormat(
            "LLM Chat module initialization complete. Personalities loaded: {}", 
            LLMChatPersonality::GetLoadedPersonalitiesCount()));
        
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

    void OnUpdate(uint32 /*diff*/) override {
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

void Addmod_llm_chatScripts()
{
    new LLMChat_WorldScript();
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatPlayerScript();
} 

// Helper class for counting nearby players
class PlayerCounter {
public:
    uint32& count;
    Player* source;
    float range;

    PlayerCounter(uint32& c, Player* s, float r) : count(c), source(s), range(r) {}

    void Visit(PlayerMapType& m) {
        for (auto itr = m.begin(); itr != m.end(); ++itr) {
            if (Player* target = itr->GetSource()) {
                // Only count real players, not bots
                if (target != source && target->IsInWorld() && 
                    !target->IsGameMaster() && IsRealPlayer(target) &&
                    target->GetDistance(source) <= range) {
                    ++count;
                }
            }
        }
    }

    template<class SKIP>
    void Visit(GridRefMgr<SKIP>&) {}
};

uint32 GetNearbyPlayerCount(Player* player) {
    if (!player || !player->IsInWorld())
        return 0;

    uint32 count = 0;
    float range = 50.0f; // 50 yards range

    Map* map = player->GetMap();
    if (!map)
        return 0;

    map->DoForAllPlayers([&count, player, range](Player* target) {
        if (target && target != player && 
            target->IsInWorld() && 
            !target->IsGameMaster() && 
            IsRealPlayer(target) &&
            target->IsWithinDist(player, range)) {
            ++count;
        }
    });

    return count;
} 

bool IsInCombat(Player* player) {
    return player && player->IsInCombat();
} 