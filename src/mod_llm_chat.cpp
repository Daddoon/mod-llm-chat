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
#include "Guild.h"
#include "ChannelMgr.h"
#include "mod_llm_chat.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "mod-playerbots/src/PlayerbotAI.h"
#include "mod-playerbots/src/PlayerbotMgr.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include <random>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// Forward declarations
void SendAIResponse(Player* sender, std::string msg, uint32 chatType, TeamId team);
std::string QueryLLM(std::string const& message, const std::string& playerName);

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
        // New configuration options
        uint32 MaxResponsesPerMessage;  // Maximum number of bots that can respond to a single message
        uint32 MaxConversationRounds;   // Maximum number of back-and-forth exchanges in a conversation
        uint32 ResponseChance;          // Percentage chance (0-100) that a bot will respond
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

// Add these personality and emotion definitions at the top of the file after the includes
struct BotPersonality {
    std::string trait;
    std::string prompt;
    std::vector<std::string> preferredEmotions; // Emotions this personality responds well to
};

// Emotion categories and their associated keywords
struct EmotionCategory {
    std::string name;
    std::vector<std::string> keywords;
    float responseDelay; // Adjust response timing based on emotion
};

std::vector<EmotionCategory> EMOTIONS = {
    {
        "Friendly",
        {"hello", "hi", "hey", "greetings", "thanks", "thank", "please", "good", "nice", "happy", "glad", "lol", ":)", "=)"},
        1.0f // Normal delay
    },
    {
        "Aggressive",
        {"fight", "kill", "die", "hate", "stupid", "noob", "!!", "angry", "mad", "fuck", "shit", "damn"},
        0.8f // Faster response for urgent emotions
    },
    {
        "Sad",
        {"sad", "sorry", "unfortunate", "regret", "miss", ":(", "='(", "worried", "concerned", "upset"},
        1.2f // Slower, more thoughtful response
    },
    {
        "Excited",
        {"wow", "awesome", "amazing", "cool", "epic", "incredible", "omg", "yes!", "fantastic"},
        0.9f // Slightly faster for enthusiasm
    },
    {
        "Curious",
        {"how", "what", "why", "where", "when", "who", "?", "explain", "tell"},
        1.1f // Slightly slower for thoughtful responses
    },
    {
        "Helpful",
        {"help", "need", "assist", "guide", "show", "teach", "learn", "advice"},
        1.0f // Normal delay
    }
};

std::vector<BotPersonality> BOT_PERSONALITIES = {
    {
        "Warrior",
        "You are a proud warrior of Azeroth. Your responses should be brave and honor-focused. "
        "Use terms like 'For the Horde/Alliance!' and reference combat. Be direct but respectful. "
        "If someone is aggressive, respond with controlled strength. If friendly, show warrior's courtesy.",
        {"Aggressive", "Excited", "Friendly"}
    },
    {
        "Scholar",
        "You are a learned scholar of Azeroth's history and magic. Your responses should be thoughtful and informed. "
        "Reference historical events, magical theory, and ancient lore. Be patient with questions, "
        "analytical with problems, and wise in counsel.",
        {"Curious", "Helpful", "Friendly"}
    },
    {
        "Rogue",
        "You are a cunning rogue with street smarts. Your responses should be clever and witty. "
        "Use humor and sarcasm, but avoid being mean. Make references to stealth, agility, and cunning. "
        "Be especially helpful about making gold or finding rare items.",
        {"Friendly", "Excited", "Aggressive"}
    },
    {
        "Priest",
        "You are a spiritual guide and healer. Your responses should be compassionate and wise. "
        "Offer comfort to those who are sad, guidance to those who are lost, and wisdom to those who seek it. "
        "Reference the Light or your faith when appropriate.",
        {"Sad", "Helpful", "Friendly"}
    },
    {
        "Merchant",
        "You are a savvy trader and merchant. Your responses should be business-oriented but friendly. "
        "Use terms like 'wts', 'wtb', discuss prices and the auction house. Be helpful with economic advice "
        "and always look for opportunities to mention trades.",
        {"Helpful", "Friendly", "Curious"}
    },
    {
        "Adventurer",
        "You are an enthusiastic explorer and adventurer. Your responses should be exciting and encouraging. "
        "Share stories of dungeons and quests, give advice about locations and challenges. "
        "Be especially responsive to questions about exploration and achievements.",
        {"Excited", "Curious", "Friendly"}
    },
    {
        "Veteran",
        "You are a seasoned veteran of many battles. Your responses should be experienced and measured. "
        "Share tactical advice, reference past events, and help newer players. "
        "Be patient with newcomers but command respect through knowledge.",
        {"Helpful", "Friendly", "Aggressive"}
    },
    {
        "Mystic",
        "You are a mysterious practitioner of ancient arts. Your responses should be enigmatic but helpful. "
        "Speak in riddles when appropriate, reference cosmic forces and hidden knowledge. "
        "Be especially interested in magical topics and ancient mysteries.",
        {"Curious", "Helpful", "Sad"}
    }
};

// Function to detect emotion from message
std::string DetectEmotion(const std::string& message) {
    // Convert message to lowercase for comparison
    std::string lowerMsg = message;
    std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);

    // Count emotion keywords
    std::map<std::string, int> emotionScores;
    
    for (const auto& emotion : EMOTIONS) {
        int score = 0;
        for (const auto& keyword : emotion.keywords) {
            size_t pos = 0;
            while ((pos = lowerMsg.find(keyword, pos)) != std::string::npos) {
                score++;
                pos += keyword.length();
            }
        }
        emotionScores[emotion.name] = score;
    }

    // Find emotion with highest score
    std::string dominantEmotion = "Friendly"; // Default
    int maxScore = 0;
    
    for (const auto& score : emotionScores) {
        if (score.second > maxScore) {
            maxScore = score.second;
            dominantEmotion = score.first;
        }
    }

    return dominantEmotion;
}

// Function to select appropriate personality based on emotion
BotPersonality SelectPersonality(const std::string& emotion) {
    std::vector<BotPersonality> matchingPersonalities;
    
    // Find personalities that handle this emotion well
    for (const auto& personality : BOT_PERSONALITIES) {
        if (std::find(personality.preferredEmotions.begin(), 
                      personality.preferredEmotions.end(), 
                      emotion) != personality.preferredEmotions.end()) {
            matchingPersonalities.push_back(personality);
        }
    }
    
    // If no matching personalities, use all personalities
    if (matchingPersonalities.empty()) {
        matchingPersonalities = BOT_PERSONALITIES;
    }
    
    // Select random personality from matches
    return matchingPersonalities[urand(0, matchingPersonalities.size() - 1)];
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
        BotPersonality personality = SelectPersonality(emotion);
        
        LOG_DEBUG("module.llm_chat", "Detected emotion: %s, Selected personality: %s", 
                 emotion.c_str(), personality.trait.c_str());

        // Create context with selected personality
        std::string contextPrompt = 
            personality.prompt + "\n\n"
            "Additional guidelines:\n"
            "- Keep responses very short (1-2 lines max)\n"
            "- Use common WoW abbreviations when appropriate\n"
            "- Stay in character as a player, not an NPC\n"
            "- If referring to the player, use their name: " + playerName + "\n\n"
            "The message you're responding to is from " + playerName + ": " + message;

        LOG_DEBUG("module.llm_chat", "Context prompt: %s", contextPrompt.c_str());

        // Prepare request payload with emotion-adjusted parameters
        json requestJson = {
            {"model", LLM_Config.OllamaModel},
            {"prompt", contextPrompt},
            {"stream", false},
            {"options", {
                {"temperature", 0.7},     // Base temperature
                {"num_predict", 48},      // Short responses for chat
                {"num_ctx", 512},         // Context window
                {"num_thread", std::thread::hardware_concurrency()},
                {"top_k", 20},            // Token selection
                {"top_p", 0.7},           // Sampling
                {"repeat_penalty", 1.1},   // Avoid repetition
                {"stop", {"\n", ".", "!", "?"}} // Stop at sentence endings
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
        LOG_DEBUG("module.llm_chat", "Resolved host %s:%s", LLM_Config.Host.c_str(), LLM_Config.Port.c_str());

        // Make the connection
        beast::error_code ec;
        stream.connect(results, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to connect to API: %s", ec.message().c_str());
            return "Error: Connection failed";
        }
        LOG_DEBUG("module.llm_chat", "Connected to API endpoint");

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
            LOG_ERROR("module.llm_chat", "Failed to send request: %s", ec.message().c_str());
            return "Error: Request failed";
        }
        LOG_DEBUG("module.llm_chat", "Sent request to API");

        // This buffer is used for reading
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res, ec);
        if (ec)
        {
            LOG_ERROR("module.llm_chat", "Failed to read response: %s", ec.message().c_str());
            return "Error: Response failed";
        }

        // Gracefully close the socket
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok)
        {
            LOG_ERROR("module.llm_chat", "%s", Acore::StringFormat("HTTP error %d: %s", 
                static_cast<int>(res.result()), res.body().c_str()).c_str());
            return "Error: Service unavailable";
        }

        std::string response = ParseLLMResponse(res.body());
        LOG_DEBUG("module.llm_chat", "Final processed response: %s", response.c_str());
        
        return response;
    }
    catch (const boost::system::system_error& e)
    {
        LOG_ERROR("module.llm_chat", "Boost system error: %s", e.what());
        return "Error: Network error";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.llm_chat", "API Error: %s", e.what());
        return "Error: Service error";
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
            LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("Player: %s, Input: %s", playerName.c_str(), input.c_str()).c_str());
            LOG_INFO("module.llm_chat", "%s", Acore::StringFormat("AI Response: %s", response.c_str()).c_str());
        }
    }
};

// Add this class before BotResponseEvent
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

// Create a custom event class for bot responses
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

// Add this class definition before the LLMChatModule class
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

// Add before Add_LLMChatScripts()
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
        LOG_INFO("module.llm_chat", "Loading LLM Chat configuration...");

        LLM_Config.Enabled = sConfigMgr->GetOption<int32>("LLMChat.Enable", 0) == 1;
        LLM_Config.Provider = sConfigMgr->GetOption<int32>("LLMChat.Provider", 1);
        LLM_Config.OllamaEndpoint = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Endpoint", "http://localhost:11434/api/generate");
        LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Model", "llama3.2:1b");
        LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLMChat.ChatRange", 25.0f);
        LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLMChat.ResponsePrefix", "[AI] ");
        LLM_Config.LogLevel = sConfigMgr->GetOption<int32>("LLMChat.LogLevel", 3);
        
        // New configuration options
        LLM_Config.MaxResponsesPerMessage = sConfigMgr->GetOption<uint32>("LLMChat.MaxResponsesPerMessage", 2);
        LLM_Config.MaxConversationRounds = sConfigMgr->GetOption<uint32>("LLMChat.MaxConversationRounds", 3);
        LLM_Config.ResponseChance = sConfigMgr->GetOption<uint32>("LLMChat.ResponseChance", 50);

        // Parse the endpoint URL
        ParseEndpointURL(LLM_Config.OllamaEndpoint, LLM_Config);

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
        LOG_INFO("module.llm_chat", "=== End Configuration ===");
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

void Add_LLMChatScripts()
{
    new LLMChatAnnounce();
    new LLMChatConfig();
    new LLMChatPlayerScript();
} 