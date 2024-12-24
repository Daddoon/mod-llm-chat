#ifndef _LLM_CHAT_QUEUE_H_
#define _LLM_CHAT_QUEUE_H_

#include "Define.h"
#include <string>
#include <queue>
#include <mutex>
#include <future>

struct QueuedResponse {
    uint32 senderGUID;
    uint32 targetGUID;
    std::string message;
    std::string personality;
    std::promise<std::string> responsePromise;

    QueuedResponse(uint32 sender, uint32 target, const std::string& msg, const std::string& pers)
        : senderGUID(sender), targetGUID(target), message(msg), personality(pers) {}

    // Copy constructor
    QueuedResponse(const QueuedResponse& other) = delete;

    // Move constructor
    QueuedResponse(QueuedResponse&& other) noexcept
        : senderGUID(other.senderGUID)
        , targetGUID(other.targetGUID)
        , message(std::move(other.message))
        , personality(std::move(other.personality))
        , responsePromise(std::move(other.responsePromise)) {}

    // Copy assignment
    QueuedResponse& operator=(const QueuedResponse& other) = delete;

    // Move assignment
    QueuedResponse& operator=(QueuedResponse&& other) noexcept {
        if (this != &other) {
            senderGUID = other.senderGUID;
            targetGUID = other.targetGUID;
            message = std::move(other.message);
            personality = std::move(other.personality);
            responsePromise = std::move(other.responsePromise);
        }
        return *this;
    }
};

class LLMChatQueue {
public:
    static bool Initialize();
    static void Shutdown();
    
    static bool EnqueueResponse(QueuedResponse&& response);
    static bool IsFull();
    static bool IsEmpty();
    static size_t GetSize();
    static void ProcessQueue();

private:
    static std::queue<QueuedResponse> m_queue;
    static std::mutex m_mutex;
    static bool m_initialized;
    static bool m_running;

    static bool ValidateResponse(const QueuedResponse& response);
};

#endif // _LLM_CHAT_QUEUE_H_ 