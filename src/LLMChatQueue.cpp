#include "LLMChatQueue.h"
#include "LLMChatLogger.h"
#include "LLMChatMemory.h"
#include "mod_llm_chat_config.h"
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
    
    // Create response using the constructor
    QueuedResponse response(getMSTime(), sender, responders, message, chatType, team, 
                          0, responders.size(), 0);
    
    m_queue.push(std::move(response));
    m_queueCondition.notify_one();
    
    return true;
}

void LLMChatQueue::WorkerThread()
{
    while (!m_shutdown) {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_queueCondition.wait(lock, [this] { 
            return !m_queue.empty() || m_shutdown; 
        });

        if (m_shutdown) {
            break;
        }

        // Get the next response to process
        QueuedResponse response = std::move(m_queue.front());
        m_queue.pop();
        lock.unlock();

        // Process the response
        ProcessResponse(response);
    }
}

bool LLMChatQueue::ProcessResponse(QueuedResponse& response)
{
    if (!response.sender || !response.sender->IsInWorld()) {
        return false;
    }

    if (response.responsesGenerated >= response.maxResponses) {
        return false;
    }

    if (response.responders.empty() || 
        response.responsesGenerated >= response.responders.size()) {
        return false;
    }

    Player* currentResponder = response.responders[response.responsesGenerated];
    if (!currentResponder || !currentResponder->IsInWorld()) {
        return false;
    }

    uint32 currentTime = getMSTime();
    if (currentTime - response.timestamp > 10000) { // 10 second timeout
        return false;
    }

    // Process the response here...
    return true;
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

// Implement QueuedResponse move operations
QueuedResponse::QueuedResponse(QueuedResponse&& other) noexcept
    : timestamp(other.timestamp)
    , sender(other.sender)
    , responders(std::move(other.responders))
    , message(std::move(other.message))
    , chatType(other.chatType)
    , team(other.team)
    , responsesGenerated(other.responsesGenerated)
    , maxResponses(other.maxResponses)
    , retryCount(other.retryCount)
    , responsePromise(std::move(other.responsePromise))
{
}

QueuedResponse& QueuedResponse::operator=(QueuedResponse&& other) noexcept
{
    if (this != &other)
    {
        timestamp = other.timestamp;
        sender = other.sender;
        responders = std::move(other.responders);
        message = std::move(other.message);
        chatType = other.chatType;
        team = other.team;
        responsesGenerated = other.responsesGenerated;
        maxResponses = other.maxResponses;
        retryCount = other.retryCount;
        responsePromise = std::move(other.responsePromise);
    }
    return *this;
} 