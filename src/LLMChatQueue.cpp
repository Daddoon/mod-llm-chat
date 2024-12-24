#include "LLMChatQueue.h"
#include "LLMChatLogger.h"
#include "LLMChatMemory.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

LLMChatQueue* LLMChatQueue::_instance = nullptr;

LLMChatQueue* LLMChatQueue::instance()
{
    if (!_instance)
        _instance = new LLMChatQueue();
    return _instance;
}

void LLMChatQueue::Initialize()
{
    m_shutdown = false;
    m_workerThread = std::thread(&LLMChatQueue::WorkerThread, this);
    LLMChatLogger::Log(1, "LLM Chat Queue initialized");
}

void LLMChatQueue::Shutdown()
{
    m_shutdown = true;
    m_queueCondition.notify_all();
    
    if (m_workerThread.joinable())
        m_workerThread.join();
        
    // Wait for any remaining API calls
    uint32 waitTime = 0;
    while (m_activeApiCalls > 0 && waitTime < LLM_Config.Performance.Threading.ApiTimeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waitTime += 100;
    }
    
    LLMChatLogger::Log(1, "LLM Chat Queue shutdown complete");
}

LLMChatQueue::~LLMChatQueue()
{
    Shutdown();
}

bool LLMChatQueue::EnqueueResponse(Player* sender, const std::vector<Player*>& responders,
                                 const std::string& message, uint32 chatType, TeamId team)
{
    if (!sender || responders.empty() || message.empty())
        return false;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Check queue size
    if (IsFull()) {
        LLMChatLogger::LogDebug("Queue is full, rejecting response");
        return false;
    }
    
    // Check per-player pending limit
    uint32 pendingCount = 0;
    std::queue<QueuedResponse> tempQueue = m_queue;
    while (!tempQueue.empty()) {
        if (tempQueue.front().sender == sender)
            pendingCount++;
        tempQueue.pop();
    }
    
    if (pendingCount >= LLM_Config.Queue.MaxPendingPerPlayer) {
        LLMChatLogger::LogDebug("Player has too many pending responses");
        return false;
    }
    
    QueuedResponse response;
    response.timestamp = getMSTime();
    response.sender = sender;
    response.responders = responders;
    response.message = message;
    response.chatType = chatType;
    response.team = team;
    response.responsesGenerated = 0;
    response.maxResponses = responders.size();
    response.retryCount = 0;
    
    m_queue.push(std::move(response));
    m_queueCondition.notify_one();
    
    return true;
}

void LLMChatQueue::WorkerThread()
{
    while (!m_shutdown) {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        
        m_queueCondition.wait(lock, [this]() {
            return !m_queue.empty() || m_shutdown;
        });
        
        if (m_shutdown)
            break;
            
        if (!m_queue.empty()) {
            QueuedResponse response = std::move(m_queue.front());
            m_queue.pop();
            lock.unlock();
            
            if (ValidateResponse(response)) {
                if (!ProcessSingleResponse(response)) {
                    HandleFailedResponse(response);
                }
            }
        }
        
        // Process next batch after a small delay
        std::this_thread::sleep_for(std::chrono::milliseconds(
            LLM_Config.Performance.Threading.QueueProcessInterval));
    }
}

bool LLMChatQueue::ProcessSingleResponse(QueuedResponse& response)
{
    try {
        // Check if we can make an API call
        if (m_activeApiCalls >= LLM_Config.Performance.Threading.MaxApiCalls) {
            return false;
        }
        
        ++m_activeApiCalls;
        
        // Get conversation context
        std::string context = LLMChatMemory::GetConversationContext(
            response.sender->GetName(),
            response.responders[response.responsesGenerated]->GetName());
            
        // Prepare API request based on provider
        json requestJson;
        if (LLM_Config.UseOllama) {
            requestJson = {
                {"model", LLM_Config.Model},
                {"prompt", context + "\n\n" + response.message},
                {"temperature", LLM_Config.LLM.Temperature},
                {"top_p", LLM_Config.LLM.TopP},
                {"num_predict", LLM_Config.LLM.NumPredict},
                {"stop", {"\n\n", "Human:", "Assistant:", "[", "<"}},
                {"repeat_penalty", LLM_Config.LLM.RepeatPenalty}
            };
        } else {
            requestJson = {
                {"model", LLM_Config.Model},
                {"messages", {
                    {
                        {"role", "system"},
                        {"content", context}
                    },
                    {
                        {"role", "user"},
                        {"content", response.message}
                    }
                }},
                {"temperature", LLM_Config.LLM.Temperature},
                {"max_tokens", LLM_Config.LLM.MaxTokens},
                {"top_p", LLM_Config.LLM.TopP},
                {"frequency_penalty", 0.0},
                {"presence_penalty", LLM_Config.LLM.RepeatPenalty},
                {"stop", LLM_Config.LLM.StopSequence}
            };
        }

        // Set up the IO context
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(
            LLM_Config.UseOllama ? LLM_Config.OllamaEndpoint : LLM_Config.Host,
            LLM_Config.Port);

        // Make the connection
        stream.connect(results);

        // Set up the HTTP request
        http::request<http::string_body> req{http::verb::post, LLM_Config.Target, 11};
        req.set(http::field::host, LLM_Config.Host);
        req.set(http::field::user_agent, "AzerothCore-LLMChat/1.0");
        req.set(http::field::content_type, "application/json");

        if (!LLM_Config.UseOllama) {
            req.set("Authorization", "Bearer " + LLM_Config.ApiKey);
            if (!LLM_Config.ApiVersion.empty()) {
                req.set("OpenAI-Version", LLM_Config.ApiVersion);
            }
        }

        req.body() = requestJson.dump();
        req.prepare_payload();

        // Send the HTTP request
        http::write(stream, req);

        // Receive the HTTP response
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        // Process the response
        if (res.result() == http::status::ok) {
            json responseJson = json::parse(res.body());
            std::string aiResponse;
            
            if (LLM_Config.UseOllama) {
                aiResponse = responseJson["response"].get<std::string>();
            } else {
                aiResponse = responseJson["choices"][0]["message"]["content"].get<std::string>();
            }

            // Store in memory
            LLMChatMemory::AddToMemory(
                response.sender->GetName(),
                response.responders[response.responsesGenerated]->GetName(),
                response.message,
                aiResponse
            );

            // Set the response promise
            response.responsePromise.set_value(aiResponse);
            
            --m_activeApiCalls;
            return true;
        }
        
        --m_activeApiCalls;
        return false;
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Error processing response: " + std::string(e.what()));
        --m_activeApiCalls;
        return false;
    }
}

void LLMChatQueue::HandleFailedResponse(QueuedResponse& response)
{
    if (response.retryCount < LLM_Config.Queue.RetryAttempts) {
        response.retryCount++;
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(std::move(response));
        
        // Add exponential backoff
        std::this_thread::sleep_for(std::chrono::milliseconds(
            LLM_Config.Performance.Delays.QueueRetry * (1 << response.retryCount)));
    } else {
        LLMChatLogger::LogError("Max retry attempts reached for response");
        response.responsePromise.set_value(""); // Empty response indicates failure
    }
}

bool LLMChatQueue::ValidateResponse(const QueuedResponse& response) const
{
    if (!response.sender || !response.sender->IsInWorld())
        return false;
        
    if (response.responsesGenerated >= response.maxResponses)
        return false;
        
    if (!response.responders[response.responsesGenerated] || 
        !response.responders[response.responsesGenerated]->IsInWorld())
        return false;
        
    uint32 currentTime = getMSTime();
    if (currentTime - response.timestamp > LLM_Config.Queue.Timeout)
        return false;
        
    return true;
}

void LLMChatQueue::CleanupQueue()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    std::queue<QueuedResponse> tempQueue;
    uint32 currentTime = getMSTime();
    
    while (!m_queue.empty()) {
        QueuedResponse& response = m_queue.front();
        if (currentTime - response.timestamp <= LLM_Config.Queue.Timeout) {
            tempQueue.push(std::move(response));
        }
        m_queue.pop();
    }
    
    m_queue = std::move(tempQueue);
} 