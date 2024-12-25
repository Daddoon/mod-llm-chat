#include "LLMChatQueue.h"
#include "LLMChatLogger.h"
#include "mod-llm-chat.h"

// Static member initialization
std::queue<QueuedResponse> LLMChatQueue::m_queue;
std::mutex LLMChatQueue::m_mutex;
bool LLMChatQueue::m_initialized = false;
bool LLMChatQueue::m_running = false;

bool LLMChatQueue::Initialize() {
    if (m_initialized)
        return true;

    m_initialized = true;
    m_running = true;
    return true;
}

void LLMChatQueue::Shutdown() {
    m_running = false;
    m_initialized = false;

    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) {
        auto& response = m_queue.front();
        response.responsePromise.set_value("Server shutting down");
        m_queue.pop();
    }
}

bool LLMChatQueue::EnqueueResponse(QueuedResponse&& response) {
    if (!m_initialized || !m_running)
        return false;

    if (IsFull())
        return false;

    if (!ValidateResponse(response))
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push(std::move(response));
    return true;
}

bool LLMChatQueue::IsFull() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size() >= LLM_Config.LLM.MaxQueueSize;
}

bool LLMChatQueue::IsEmpty() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
}

size_t LLMChatQueue::GetSize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void LLMChatQueue::ProcessQueue() {
    if (!m_initialized || !m_running)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return;

    auto& response = m_queue.front();
    
    try {
        // Get response from LLM
        std::string llmResponse = QueryLLM(response.message, 
            std::to_string(response.senderGUID), 
            std::to_string(response.targetGUID));

        // Set the response
        response.responsePromise.set_value(llmResponse);
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to process response: " + std::string(e.what()));
        response.responsePromise.set_value("Error processing response");
    }

    m_queue.pop();
}

bool LLMChatQueue::ValidateResponse(const QueuedResponse& response) {
    if (response.senderGUID == 0 || response.targetGUID == 0)
        return false;

    if (response.message.empty())
        return false;

    if (response.personality.empty())
        return false;

    return true;
} 