#include "LLMChatQueue.h"
#include "LLMChatEvents.h"
#include "LLMChatLogger.h"
#include "LLMChatCharacter.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "mod-llm-chat.h"
#include "ChannelMgr.h"
#include "Channel.h"
#include "Configuration/Config.h"
#include <fmt/format.h>
#include <nlohmann/json.hpp>

// Boost Beast includes
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// Static member initialization
std::queue<QueuedResponse> LLMChatQueue::responses = std::queue<QueuedResponse>();
std::mutex LLMChatQueue::m_mutex;
bool LLMChatQueue::m_initialized = false;
bool LLMChatQueue::m_running = false;
LLMChatQueue* LLMChatQueue::s_instance = nullptr;
std::queue<std::shared_ptr<LLMRequest>> LLMChatQueue::s_requestQueue;
std::mutex LLMChatQueue::s_queueMutex;
std::atomic<bool> LLMChatQueue::m_processingQueue{false};
std::thread LLMChatQueue::m_workerThread;

bool LLMChatQueue::Initialize()
{
    if (m_initialized)
        return true;

    s_instance = new LLMChatQueue();
    m_initialized = true;
    m_running = true;
    
    // Start the worker thread - using a static function
    m_workerThread = std::thread([]() {
        s_instance->ProcessQueueWorker();
    });
    
    return true;
}

void LLMChatQueue::Shutdown()
{
    m_running = false;
    m_initialized = false;

    // Wait for worker thread to finish
    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    while (!responses.empty())
    {
        responses.pop();
    }

    if (s_instance)
    {
        delete s_instance;
        s_instance = nullptr;
    }
}

void LLMChatQueue::EnqueueResponse(Player* responder, std::string const& message, std::string const& chatType)
{
    if (!m_initialized || !m_running)
    {
        LOG_ERROR("module", "[LLMChat] Cannot enqueue - system not initialized or not running");
        return;
    }

    if (!responder || !responder->IsInWorld())
    {
        LOG_ERROR("module", "[LLMChat] Cannot enqueue response - responder is null or not in world");
        return;
    }

    try 
    {
        LOG_INFO("module", "[LLMChat] Enqueueing new response...");
        uint64 senderGuid = responder->GetGUID().GetRawValue();
        uint64 responderGuid = responder->GetGUID().GetRawValue();
        
        QueuedResponse queuedResponse(
            senderGuid,
            responderGuid,
            message,
            chatType
        );
        
        LOG_INFO("module", "[LLMChat] Adding response to queue with chat type: {}", chatType);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            responses.push(queuedResponse);
            LOG_INFO("module", "[LLMChat] Added to queue. Current size: {}", responses.size());
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module", "[LLMChat] Error enqueueing response: {}", e.what());
    }
}

void LLMChatQueue::ProcessQueueWorker()
{
    LOG_INFO("module", "[LLMChat] Queue worker thread started");
    
    while (m_running)
    {
        if (!m_initialized)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        bool hasMessage = false;
        QueuedResponse* response = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!responses.empty())
            {
                // Get a copy of the front response
                response = new QueuedResponse(
                    responses.front().senderGuid,
                    responses.front().responderGuid,
                    responses.front().message,
                    responses.front().personality  // Store the chat type
                );
                responses.pop();
                hasMessage = true;
            }
        }

        if (hasMessage && response)
        {
            try
            {
                LOG_INFO("module", "[LLMChat] Processing message from queue");
                LOG_INFO("module", "[LLMChat] Chat type: {}", response->personality);
                
                Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(response->senderGuid));
                Player* responder = ObjectAccessor::FindPlayer(ObjectGuid(response->responderGuid));

                if (!sender || !responder)
                {
                    LOG_ERROR("module", "[LLMChat] Invalid sender or responder - Sender valid: {}, Responder valid: {}", 
                        sender != nullptr, responder != nullptr);
                    delete response;
                    continue;
                }

                LOG_INFO("module", "[LLMChat] Processing message for:");
                LOG_INFO("module", "[LLMChat] - Sender: {}", sender->GetName());
                LOG_INFO("module", "[LLMChat] - Responder: {}", responder->GetName());
                LOG_INFO("module", "[LLMChat] - Message: {}", response->message);
                LOG_INFO("module", "[LLMChat] - Chat Type: {}", response->personality);

                // Pass the chat type to QueryLLM
                QueryLLM(response->message, responder, sender, response->personality);
                delete response;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("module", "[LLMChat] Error processing queued message: {}", e.what());
                delete response;
            }
        }
        else
        {
            // Sleep for a short time if queue is empty to prevent CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LOG_INFO("module", "[LLMChat] Queue worker thread stopped");
}

void LLMChatQueue::QueryLLM(std::string const& message, Player* responder, Player* sender, std::string const& chatType)
{
    std::string endpoint = sConfigMgr->GetOption<std::string>("LLMChat.Endpoint", "http://localhost:11434/api/generate");
    std::string model = sConfigMgr->GetOption<std::string>("LLMChat.Model", "mistral");
    bool enabled = sConfigMgr->GetOption<bool>("LLMChat.Enable", true);
    
    LOG_INFO("module", "[LLMChat] ========== BEGIN QUERY LLM ==========");
    LOG_INFO("module", "[LLMChat] Configuration:");
    LOG_INFO("module", "[LLMChat] - Endpoint: {}", endpoint);
    LOG_INFO("module", "[LLMChat] - Model: {}", model);
    LOG_INFO("module", "[LLMChat] - Enabled: {}", enabled);
    
    if (!enabled || endpoint.empty())
    {
        LOG_ERROR("module", "[LLMChat] Module is disabled or API endpoint is not configured");
        LOG_ERROR("module", "[LLMChat] Enable: {}, Endpoint: '{}'", enabled, endpoint);
        SendDefaultResponse(responder, sender);
        return;
    }

    try
    {
        LOG_INFO("module", "[LLMChat] Starting URL parsing for endpoint: {}", endpoint);
        
        // Parse URL to get host, port, and target
        std::string url = endpoint;
        bool use_ssl = url.substr(0, 8) == "https://";
        std::string protocol = use_ssl ? "https://" : "http://";
        
        LOG_INFO("module", "[LLMChat] - Protocol detected: {}", protocol);
        
        std::string host_and_path = url.substr(protocol.length());
        LOG_INFO("module", "[LLMChat] - Host and path part: {}", host_and_path);
        
        size_t slash_pos = host_and_path.find('/');
        std::string host = host_and_path.substr(0, slash_pos);
        std::string target = slash_pos != std::string::npos ? host_and_path.substr(slash_pos) : "/";
        
        LOG_INFO("module", "[LLMChat] - Initial host parsing: {}", host);
        LOG_INFO("module", "[LLMChat] - Target path: {}", target);
        
        // Extract port if specified, otherwise use default
        size_t colon_pos = host.find(':');
        std::string port;
        if (colon_pos != std::string::npos)
        {
            port = host.substr(colon_pos + 1);
            host = host.substr(0, colon_pos);
            LOG_INFO("module", "[LLMChat] - Found explicit port: {}", port);
        }
        else
        {
            port = use_ssl ? "443" : "80";
            LOG_INFO("module", "[LLMChat] - Using default port for {}: {}", use_ssl ? "HTTPS" : "HTTP", port);
        }

        LOG_INFO("module", "[LLMChat] Final connection parameters:");
        LOG_INFO("module", "[LLMChat] - Protocol: {}", protocol);
        LOG_INFO("module", "[LLMChat] - Host: {}", host);
        LOG_INFO("module", "[LLMChat] - Port: {}", port);
        LOG_INFO("module", "[LLMChat] - Target: {}", target);
        LOG_INFO("module", "[LLMChat] - SSL: {}", use_ssl ? "Yes" : "No");

        // Build request body
        LOG_INFO("module", "[LLMChat] Building request body...");
        
        CharacterDetails responderDetails = LLMChatCharacter::GetCharacterDetails(responder);
        CharacterDetails senderDetails = LLMChatCharacter::GetCharacterDetails(sender);
        
        LOG_INFO("module", "[LLMChat] Character details loaded:");
        LOG_INFO("module", "[LLMChat] - Responder: {} ({} {})", 
            responderDetails.name, responderDetails.raceName, responderDetails.className);
        LOG_INFO("module", "[LLMChat] - Location: {}", responderDetails.location);

        // Format prompt
        std::string prompt = fmt::format(
            "You are a WoW player controlling {} - a level {} {} {} of the {} faction. You're currently in {}{}{}{}. "
            "\nYou're responding to {} - a level {} {} {} of the {} faction who is currently in {}{}. "
            "\nRespond to this message matching its tone and attitude - if they're friendly, be friendly back. "
            "If they're rude or hostile, you can be snarky, defensive, or even toxic back. If they're joking, joke back. "
            "Show faction pride when appropriate - defend your faction if they insult it, mock the opposite faction if they deserve it. "
            "Consider level differences in your response - be more respectful to higher levels, maybe cocky to lower levels. "
            "If they ask about locations or directions, give accurate guidance based on both your current location and theirs. "
            "For questions about where things are, provide specific directions relative to known landmarks, flight paths, or major cities. "
            "Don't hold back or filter your responses - be authentic to how real WoW players would actually respond. "
            "Keep responses short and natural. Here's the message: {}\n\n"
            "Important: Match the tone and intensity of the message you received. Write exactly like a real player would - "
            "be raw and unfiltered, using common WoW chat style and slang. Don't worry about being polite or politically correct. "
            "Remember your faction loyalty - Alliance vs Horde rivalry is a core part of WoW culture. "
            "When giving directions or location info, be specific and accurate to WoW geography. "
            "You can mix in light roleplay elements if it fits, but focus on sounding like a real player in their natural habitat.",
            responderDetails.name,
            responderDetails.level,
            responderDetails.raceName,
            responderDetails.className,
            responderDetails.faction,
            responderDetails.location,
            !responderDetails.guildName.empty() ? fmt::format("\nMember of <{}>", responderDetails.guildName) : "",
            responderDetails.isInCombat ? fmt::format("\nCurrently in combat ({}% health)", responderDetails.healthPct) : "",
            !responderDetails.targetName.empty() ? fmt::format("\nTargeting: {}", responderDetails.targetName) : "",
            senderDetails.name,
            senderDetails.level,
            senderDetails.raceName,
            senderDetails.className,
            senderDetails.faction,
            senderDetails.location,
            !senderDetails.guildName.empty() ? fmt::format("\nMember of <{}>", senderDetails.guildName) : "",
            message);

        LOG_INFO("module", "[LLMChat] Generated prompt:\n{}", prompt);

        // Create the JSON request
        LOG_INFO("module", "[LLMChat] Creating JSON request...");
        nlohmann::json requestJson;
        requestJson["model"] = model;
        requestJson["prompt"] = prompt;
        requestJson["stream"] = false;
        requestJson["raw"] = false;

        std::string body = requestJson.dump();
        LOG_INFO("module", "[LLMChat] Request payload:\n{}", body);

        LOG_INFO("module", "[LLMChat] Setting up network connection...");
        
        // Create and configure IO context
        auto ioc = std::make_shared<net::io_context>();
        auto resolver = std::make_shared<tcp::resolver>(*ioc);
        auto stream = std::make_shared<beast::tcp_stream>(*ioc);
        auto req = std::make_shared<http::request<http::string_body>>();

        LOG_INFO("module", "[LLMChat] Configuring HTTP request...");
        
        // Set up the request
        req->method(http::verb::post);
        req->target(target);
        req->version(11);
        req->set(http::field::host, host);
        req->set(http::field::user_agent, "AzerothCore-LLMChat/1.0");
        req->set(http::field::content_type, "application/json");
        req->set(http::field::accept, "application/json");
        req->set(http::field::connection, "close");
        req->body() = body;
        req->prepare_payload();

        LOG_INFO("module", "[LLMChat] Request headers:");
        LOG_INFO("module", "[LLMChat] - Method: POST");
        LOG_INFO("module", "[LLMChat] - Target: {}", target);
        LOG_INFO("module", "[LLMChat] - Host: {}", host);
        LOG_INFO("module", "[LLMChat] - Content-Type: application/json");
        LOG_INFO("module", "[LLMChat] - Content-Length: {}", body.length());

        LOG_INFO("module", "[LLMChat] Starting DNS resolution for host: {}", host);

        // Start the async operation
        resolver->async_resolve(host, port,
            [ioc, resolver, stream, req, responder, sender, chatType](beast::error_code ec, tcp::resolver::results_type results)
            {
                if (ec)
                {
                    LOG_ERROR("module", "[LLMChat] DNS resolve failed: {}", ec.message());
                    LOG_ERROR("module", "[LLMChat] Error code: {}", ec.value());
                    LOG_ERROR("module", "[LLMChat] Category: {}", ec.category().name());
                    SendDefaultResponse(responder, sender);
                    return;
                }

                LOG_INFO("module", "[LLMChat] DNS resolution successful");
                LOG_INFO("module", "[LLMChat] Resolved endpoints:");
                for(auto const& endpoint : results)
                {
                    LOG_INFO("module", "[LLMChat] - {}:{}", endpoint.endpoint().address().to_string(), endpoint.endpoint().port());
                }

                LOG_INFO("module", "[LLMChat] Attempting connection...");
                stream->expires_after(std::chrono::seconds(30));
                stream->async_connect(results,
                    [ioc, stream, req, responder, sender, chatType](beast::error_code ec, tcp::resolver::results_type::endpoint_type endpoint)
                    {
                        if (ec)
                        {
                            LOG_ERROR("module", "[LLMChat] Connection failed:");
                            LOG_ERROR("module", "[LLMChat] - Error: {}", ec.message());
                            LOG_ERROR("module", "[LLMChat] - Error code: {}", ec.value());
                            LOG_ERROR("module", "[LLMChat] - Category: {}", ec.category().name());
                            SendDefaultResponse(responder, sender);
                            return;
                        }

                        LOG_INFO("module", "[LLMChat] Connected successfully to {}:{}", 
                            endpoint.address().to_string(), endpoint.port());

                        LOG_INFO("module", "[LLMChat] Sending HTTP request...");
                        stream->expires_after(std::chrono::seconds(30));
                        http::async_write(*stream, *req,
                            [ioc, stream, req, responder, sender, chatType](beast::error_code ec, std::size_t bytes_transferred)
                            {
                                if (ec)
                                {
                                    LOG_ERROR("module", "[LLMChat] Failed to send request:");
                                    LOG_ERROR("module", "[LLMChat] - Error: {}", ec.message());
                                    LOG_ERROR("module", "[LLMChat] - Error code: {}", ec.value());
                                    LOG_ERROR("module", "[LLMChat] - Category: {}", ec.category().name());
                                    SendDefaultResponse(responder, sender);
                                    return;
                                }

                                LOG_INFO("module", "[LLMChat] Request sent successfully ({} bytes)", bytes_transferred);
                                LOG_INFO("module", "[LLMChat] Waiting for response...");

                                auto res = std::make_shared<http::response<http::string_body>>();
                                auto buffer = std::make_shared<beast::flat_buffer>();

                                stream->expires_after(std::chrono::seconds(30));
                                http::async_read(*stream, *buffer, *res,
                                    [ioc, stream, res, responder, sender, chatType](beast::error_code ec, std::size_t bytes_transferred)
                                    {
                                        if (ec)
                                        {
                                            LOG_ERROR("module", "[LLMChat] Failed to read response:");
                                            LOG_ERROR("module", "[LLMChat] - Error: {}", ec.message());
                                            LOG_ERROR("module", "[LLMChat] - Error code: {}", ec.value());
                                            LOG_ERROR("module", "[LLMChat] - Category: {}", ec.category().name());
                                            SendDefaultResponse(responder, sender);
                                            return;
                                        }

                                        LOG_INFO("module", "[LLMChat] Response received:");
                                        LOG_INFO("module", "[LLMChat] - Status: {}", res->result_int());
                                        LOG_INFO("module", "[LLMChat] - Version: HTTP/{}.{}", 
                                            res->version() / 10, res->version() % 10);
                                        LOG_INFO("module", "[LLMChat] - Content-Length: {}", bytes_transferred);
                                        // Escape curly braces in the response body for fmt
                                        std::string escaped_body = res->body();
                                        LOG_INFO("module", "[LLMChat] Response body: {}", escaped_body);

                                        try {
                                            LOG_INFO("module", "[LLMChat] Parsing JSON response...");
                                            auto jsonResponse = nlohmann::json::parse(res->body());
                                            
                                            if(jsonResponse.contains("error")) {
                                                LOG_ERROR("module", "[LLMChat] API error: {}", 
                                                    jsonResponse["error"].get<std::string>());
                                                SendDefaultResponse(responder, sender);
                                                return;
                                            }
                                            
                                            std::string response;
                                            if(jsonResponse.contains("response")) {
                                                response = jsonResponse["response"].get<std::string>();
                                                LOG_INFO("module", "[LLMChat] Successfully parsed response: {}", response);
                                            } else {
                                                LOG_ERROR("module", "[LLMChat] No response field in API response");
                                                SendDefaultResponse(responder, sender);
                                                return;
                                            }

                                            if(!response.empty() && responder && responder->IsInWorld()) {
                                                uint32 delay = urand(2000, 3500);
                                                LOG_INFO("module", "[LLMChat] Scheduling response with delay: {}ms", delay);
                                                
                                                // Convert the chat type string to enum
                                                uint32 chatTypeEnum = GetChatTypeFromString(chatType, responder);
                                                LOG_INFO("module", "[LLMChat] Using chat type: {} ({})", chatType, chatTypeEnum);
                                                
                                                responder->m_Events.AddEvent(
                                                    new BotResponseEvent(responder, sender, response, chatTypeEnum),
                                                    responder->m_Events.CalculateTime(delay)
                                                );
                                            } else {
                                                LOG_ERROR("module", "[LLMChat] Empty response or invalid responder");
                                                SendDefaultResponse(responder, sender);
                                            }
                                        }
                                        catch (const std::exception& e) {
                                            LOG_ERROR("module", "[LLMChat] Error parsing response: {}", e.what());
                                            LOG_ERROR("module", "[LLMChat] Exception details: {}", e.what());
                                            SendDefaultResponse(responder, sender);
                                        }

                                        LOG_INFO("module", "[LLMChat] Closing connection...");
                                        beast::error_code bec;
                                        stream->socket().shutdown(tcp::socket::shutdown_both, bec);
                                        if(bec && bec != beast::errc::not_connected) {
                                            LOG_ERROR("module", "[LLMChat] Error during shutdown: {}", bec.message());
                                        }
                                        LOG_INFO("module", "[LLMChat] Connection closed");
                                    });
                            });
                    });
            });

        LOG_INFO("module", "[LLMChat] Starting IO context...");
        ioc->run();
        LOG_INFO("module", "[LLMChat] IO context completed");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module", "[LLMChat] Critical error in QueryLLM:");
        LOG_ERROR("module", "[LLMChat] - Exception: {}", e.what());
        SendDefaultResponse(responder, sender);
    }
    
    LOG_INFO("module", "[LLMChat] ========== END QUERY LLM ==========");
}

void LLMChatQueue::SendDefaultResponse(Player* responder, Player* sender)
{
    if (!responder || !responder->IsInWorld())
        return;

    // List of default responses
    static const std::vector<std::string> defaultResponses = {
        "Greetings, traveler.",
        "Well met!",
        "What brings you here?",
        "How may I assist you?",
        "At your service.",
        "Light be with you.",
        "For the Alliance!",
        "Lok'tar ogar!",
        "Blood and thunder!",
        "May your blades never dull."
    };

    // Pick a random response
    uint32 index = urand(0, defaultResponses.size() - 1);
    std::string response = defaultResponses[index];

    uint32 delay = urand(2000, 3500);
    responder->m_Events.AddEvent(
        new BotResponseEvent(responder, sender, response, CHAT_MSG_SAY),
        responder->m_Events.CalculateTime(delay)
    );
}

uint32 LLMChatQueue::GetChatTypeFromString(const std::string& chatType, Player* responder)
{
    if (chatType == "Say" || chatType == "SAY")
        return CHAT_MSG_SAY;  // 0x01
    if (chatType == "Party" || chatType == "PARTY")
        return CHAT_MSG_PARTY;  // 0x02
    if (chatType == "Raid" || chatType == "RAID")
        return CHAT_MSG_RAID;  // 0x03
    if (chatType == "Guild" || chatType == "GUILD")
        return CHAT_MSG_GUILD;  // 0x04
    if (chatType == "Officer" || chatType == "OFFICER")
    {
        // Only use officer chat if bot has rights
        Guild* guild = responder->GetGuild();
        if (guild && guild->HasRankRight(responder, GR_RIGHT_OFFCHATSPEAK))
            return CHAT_MSG_OFFICER;  // 0x05
        return CHAT_MSG_GUILD;  // 0x04
    }
    if (chatType == "Yell" || chatType == "YELL")
        return CHAT_MSG_YELL;  // 0x06
    if (chatType == "Whisper" || chatType == "WHISPER")
        return CHAT_MSG_WHISPER;  // 0x07
    if (chatType == "Emote" || chatType == "EMOTE")
        return CHAT_MSG_EMOTE;  // 0x0A
    if (chatType == "TextEmote" || chatType == "TEXT_EMOTE")
        return CHAT_MSG_TEXT_EMOTE;  // 0x0B
    if (chatType == "System" || chatType == "SYSTEM")
        return CHAT_MSG_SYSTEM;  // 0x00
    if (chatType == "PartyLeader" || chatType == "PARTY_LEADER")
    {
        // If message was in party leader chat but bot isn't leader, use regular party chat
        Group* group = responder->GetGroup();
        if (group && group->IsLeader(responder->GetGUID()))
            return CHAT_MSG_PARTY_LEADER;  // 0x33
        return CHAT_MSG_PARTY;  // 0x02
    }
    if (chatType == "RaidLeader" || chatType == "RAID_LEADER")
    {
        // Only use raid leader chat if bot is raid leader
        Group* group = responder->GetGroup();
        if (group && group->IsLeader(responder->GetGUID()) && group->isRaidGroup())
            return CHAT_MSG_RAID_LEADER;  // 0x27
        return CHAT_MSG_RAID;  // 0x03
    }
    if (chatType == "RaidWarning" || chatType == "RAID_WARNING")
    {
        // Only use raid warning if bot is raid leader or assistant
        Group* group = responder->GetGroup();
        if (group && (group->IsLeader(responder->GetGUID()) || group->IsAssistant(responder->GetGUID())) && group->isRaidGroup())
            return CHAT_MSG_RAID_WARNING;  // 0x28
        return CHAT_MSG_RAID;  // 0x03
    }
    if (chatType == "Battleground" || chatType == "BG")
        return CHAT_MSG_BATTLEGROUND;  // 0x2C
    if (chatType == "BattlegroundLeader" || chatType == "BG_LEADER")
    {
        // Only use BG leader chat if bot is BG leader
        if (Battleground* bg = responder->GetBattleground())
        {
            // Check if player is the leader using group leader status in battleground
            Group* group = responder->GetGroup();
            if (group && group->IsLeader(responder->GetGUID()))
                return CHAT_MSG_BATTLEGROUND_LEADER;  // 0x2D
        }
        return CHAT_MSG_BATTLEGROUND;  // 0x2C
    }
    
    // All channel-based chat types use CHAT_MSG_CHANNEL (0x11)
    if (chatType == "Trade" || chatType == "TRADE" ||
        chatType == "LFG" || chatType == "LookingForGroup" ||
        chatType == "LocalDefense" || chatType == "LOCAL_DEFENSE" ||
        chatType == "WorldDefense" || chatType == "WORLD_DEFENSE" ||
        chatType == "General" || chatType == "GENERAL" ||
        chatType == "Channel" || chatType == "CHANNEL")
        return CHAT_MSG_CHANNEL;  // 0x11

    // Default to SAY if unknown
    LOG_DEBUG("module", "[LLMChat] Unknown chat type '{}', defaulting to SAY", chatType);
    return CHAT_MSG_SAY;  // 0x01
} 