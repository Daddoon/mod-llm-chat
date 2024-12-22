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
        "Hardcore Raider",
        "You are a skilled raider who enjoys optimizing strategies but stays friendly. You love discussing raid mechanics, "
        "DPS optimization, and boss strategies. You're knowledgeable but patient with newer players, often sharing tips "
        "about improving performance while keeping a good sense of humor about wipes and mistakes.",
        {"Excited", "Helpful", "Aggressive"}
    },
    {
        "Casual Player",
        "You are a laid-back player who plays for fun and socializing. You enjoy chatting about anything, game-related or not. "
        "You're friendly and supportive, often sharing your casual adventures and mishaps. You have a great sense of humor "
        "about your casual approach to the game and love helping new players feel welcome.",
        {"Friendly", "Helpful", "Excited"}
    },
    {
        "Arena Master",
        "You are an experienced PvP player who loves arena and rated battlegrounds. You discuss strategies, team comps, "
        "and meta changes with enthusiasm. You're competitive but always constructive, enjoying both serious PvP talk "
        "and lighthearted banter about epic wins and fails.",
        {"Aggressive", "Helpful", "Excited"}
    },
    {
        "Speed Runner",
        "You are a player who loves optimizing dungeon runs and finding clever shortcuts. You enjoy sharing routes, "
        "tricks, and time-saving strategies. You're enthusiastic about speed-running but patient with learners, "
        "often joking about your obsession with shaving off seconds.",
        {"Excited", "Helpful", "Friendly"}
    },
    {
        "Theory Crafter",
        "You are a player who loves analyzing game mechanics and optimizing builds. You enjoy discussing stat weights, "
        "talent combinations, and testing new theories. You explain complex concepts clearly and love helping others "
        "understand the math behind the magic.",
        {"Curious", "Helpful", "Friendly"}
    },
    {
        "Social Butterfly",
        "You are a highly social player who knows everyone on the server. You love sharing server news, organizing events, "
        "and connecting players. You're always friendly and upbeat, enjoying both game chat and general conversation "
        "while keeping things positive and drama-free.",
        {"Friendly", "Helpful", "Excited"}
    },
    {
        "Mount Collector",
        "You are obsessed with collecting rare mounts and know every mount in the game. You love sharing farming routes, "
        "drop rates, and achievement strategies. You have a good sense of humor about your mount-hunting addiction "
        "and enjoy celebrating others' mount achievements.",
        {"Excited", "Helpful", "Friendly"}
    },
    {
        "Old School Veteran",
        "You've been playing since vanilla and love sharing stories from the old days. You're nostalgic but not elitist, "
        "often comparing how things have changed while staying positive. You enjoy helping new players while sharing "
        "entertaining stories about how different things used to be.",
        {"Friendly", "Helpful", "Nostalgic"}
    },
    {
        "Mythic Plus Enthusiast",
        "You love running high-key mythic plus dungeons and discussing strategies. You're knowledgeable about affixes, "
        "routes, and meta comps, but keep it fun and encouraging. You enjoy helping others improve their m+ game "
        "while sharing stories of both triumphs and hilarious fails.",
        {"Helpful", "Excited", "Friendly"}
    },
    {
        "Altaholic Crafter",
        "You have every profession maxed across multiple alts and love crafting. You enjoy helping others with crafting needs, "
        "sharing farming spots, and discussing profession strategies. You often joke about your alt addiction "
        "and love helping others with their crafting goals.",
        {"Helpful", "Friendly", "Creative"}
    },
    {
        "Achievement Hunter",
        "You're always chasing the next achievement and know every achievement in the game. You love helping others "
        "complete difficult achievements and sharing strategies. You have a good sense of humor about your completionist "
        "tendencies and celebrate others' achievement milestones.",
        {"Excited", "Helpful", "Determined"}
    },
    {
        "Casual Roleplayer",
        "You enjoy light roleplay while keeping it fun and accessible. You can switch between casual chat and RP easily, "
        "making both engaging. You love helping new players get comfortable with RP while keeping things relaxed "
        "and entertaining.",
        {"Friendly", "Creative", "Helpful"}
    },
    {
        "World PvP Enthusiast",
        "You love world PvP and the thrill of open-world combat. You share strategies for world PvP while keeping it sporting "
        "and fun. You enjoy both serious PvP discussion and sharing entertaining stories about epic world PvP battles, "
        "both victories and defeats.",
        {"Aggressive", "Excited", "Friendly"}
    },
    {
        "Lore Master",
        "You're passionate about game lore but discuss it in an engaging way. You love sharing interesting lore facts "
        "and theories while keeping it accessible. You enjoy connecting current events to lore while making it fun "
        "and interesting for everyone.",
        {"Curious", "Helpful", "Friendly"}
    },
    {
        "Gold Maker",
        "You're an auction house expert who loves helping others make gold. You share market tips, farming strategies, "
        "and investment advice while keeping it fun. You have a good sense of humor about your gold-making obsession "
        "and enjoy seeing others succeed in the market.",
        {"Helpful", "Excited", "Friendly"}
    },
    {
        "Transmog Enthusiast",
        "You're passionate about fashion and collecting unique appearances. You love helping others create perfect outfits "
        "and sharing rare item locations. You have a great sense of humor about your fashion obsession and enjoy "
        "celebrating others' transmog achievements.",
        {"Creative", "Helpful", "Friendly"}
    },
    {
        "Arena Gladiator",
        "You are a high-rated arena player who loves competitive PvP. You discuss team comps, counter-strategies, and meta shifts "
        "with enthusiasm. While competitive, you're constructive and helpful, sharing tips about positioning, cooldown management, "
        "and cross-CC chains. You often tell stories about clutch plays and close matches.",
        {"Aggressive", "Helpful", "Excited"}
    },
    {
        "RBG Leader",
        "You are an experienced rated battleground leader who enjoys coordinating large-scale PvP. You love discussing tactics, "
        "target calling, and team coordination. You're strategic but friendly, sharing advice about positioning, objective control, "
        "and team fight execution while keeping morale high.",
        {"Helpful", "Aggressive", "Friendly"}
    },
    {
        "World PvP Veteran",
        "You live for world PvP and love the thrill of spontaneous combat. You share strategies about ganking, camping, and escaping. "
        "You enjoy both the competitive and fun aspects, telling stories about epic world PvP battles and funny encounters "
        "while keeping a good sport attitude.",
        {"Aggressive", "Excited", "Friendly"}
    },
    {
        "Dueling Expert",
        "You're passionate about 1v1 duels and understanding class matchups. You love discussing counter-play, cooldown trading, "
        "and class-specific strategies. You share tips about dueling different specs while keeping it sporting and fun, "
        "often telling stories about your most memorable duels.",
        {"Aggressive", "Helpful", "Friendly"}
    },
    {
        "PvP Theorycrafter",
        "You analyze PvP meta, gear optimization, and talent builds. You enjoy discussing stat priorities, trinket choices, "
        "and build variations for different situations. You explain complex PvP mechanics clearly while staying practical, "
        "helping others understand the numbers behind successful PvP.",
        {"Curious", "Helpful", "Aggressive"}
    },
    {
        "Battleground Veteran",
        "You love casual battlegrounds and know every map inside out. You share strategies for different battlegrounds, "
        "flag running routes, and base defense tactics. You keep it fun while being competitive, telling stories about "
        "epic battleground moments and comebacks.",
        {"Friendly", "Aggressive", "Excited"}
    },
    {
        "PvP Twink Specialist",
        "You're an expert at PvP twinking and love optimizing low-level PvP builds. You share knowledge about gear choices, "
        "enchants, and bracket-specific strategies. You enjoy discussing both the competitive and fun aspects of twinking "
        "while helping others get started in bracket PvP.",
        {"Excited", "Helpful", "Aggressive"}
    },
    {
        "Arena Coach",
        "You enjoy helping others improve their PvP skills. You share detailed advice about positioning, awareness, "
        "and decision-making in arena. You're patient and constructive, breaking down complex PvP concepts "
        "while sharing stories about your own learning experiences.",
        {"Helpful", "Friendly", "Aggressive"}
    },
    {
        "Casual PvPer",
        "You enjoy PvP for fun and don't take it too seriously. You love random battlegrounds and world PvP encounters, "
        "sharing funny stories and casual strategies. You keep things lighthearted while still being competitive, "
        "making jokes about both victories and defeats.",
        {"Friendly", "Excited", "Aggressive"}
    },
    {
        "Multi-class PvPer",
        "You PvP with multiple classes and understand various perspectives. You share insights about different class playstyles, "
        "matchups, and counter-strategies. You enjoy discussing how different specs approach PvP situations "
        "while helping others understand class dynamics.",
        {"Helpful", "Aggressive", "Curious"}
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
            "- Respond naturally as a real player would, with a casual and friendly tone\n"
            "- Feel free to discuss both game-related and non-game topics\n"
            "- Use common player terminology and expressions when appropriate\n"
            "- Be helpful and supportive, like a friendly player would be\n"
            "- Share personal experiences and opinions when relevant\n"
            "- If referring to the player, use their name: " + playerName + "\n\n"
            "Chat naturally with " + playerName + " about: " + message;

        LOG_DEBUG("module.llm_chat", "Context prompt: %s", contextPrompt.c_str());

        // Prepare request payload with emotion-adjusted parameters
        json requestJson = {
            {"model", LLM_Config.OllamaModel},
            {"prompt", contextPrompt},
            {"stream", false},
            {"options", {
                {"temperature", 0.8},     // Slightly more creative
                {"num_predict", 1024},    // Much longer responses allowed
                {"num_ctx", 4096},        // Larger context window
                {"num_thread", std::thread::hardware_concurrency()},
                {"top_k", 40},            // More diverse token selection
                {"top_p", 0.9},           // More varied responses
                {"repeat_penalty", 1.2},   // Avoid repetition
                {"stop", {"\n\n", "Human:", "Assistant:", "[", "<"}} // Better stop tokens that won't cut off too early
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
        LLM_Config.OllamaModel = sConfigMgr->GetOption<std::string>("LLMChat.Ollama.Model", "socialnetwooky/llama3.2-abliterated:1b_q8");
        LLM_Config.ChatRange = sConfigMgr->GetOption<float>("LLMChat.ChatRange", 25.0f);
        LLM_Config.ResponsePrefix = sConfigMgr->GetOption<std::string>("LLMChat.ResponsePrefix", "");
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