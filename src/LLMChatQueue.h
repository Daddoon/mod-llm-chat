#ifndef MOD_LLM_CHAT_QUEUE_H
#define MOD_LLM_CHAT_QUEUE_H

#include "Player.h"
#include "mod_llm_chat_config.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <future>
#include <thread>
#include <atomic>

struct QueuedResponse {
    uint32 timestamp;
    Player* sender;
    std::vector<Player*> responders;
    std::string message;
    uint32 chatType;
    TeamId team;
    uint32 responsesGenerated;
    uint32 maxResponses;
    uint32 retryCount;

    // Default constructor
    QueuedResponse() = default;

    // Constructor with parameters
    QueuedResponse(uint32 ts, Player* s, std::vector<Player*> r, std::string m, 
                  uint32 ct, TeamId t, uint32 rg = 0, uint32 mr = 0, uint32 rc = 0)
        : timestamp(ts)
        , sender(s)
        , responders(std::move(r))
        , message(std::move(m))
        , chatType(ct)
        , team(t)
        , responsesGenerated(rg)
        , maxResponses(mr)
        , retryCount(rc)
    {}

    // Copy constructor
    QueuedResponse(const QueuedResponse& other)
        : timestamp(other.timestamp)
        , sender(other.sender)
        , responders(other.responders)
        , message(other.message)
        , chatType(other.chatType)
        , team(other.team)
        , responsesGenerated(other.responsesGenerated)
        , maxResponses(other.maxResponses)
        , retryCount(other.retryCount)
    {}

    // Copy assignment operator
    QueuedResponse& operator=(const QueuedResponse& other) {
        if (this != &other) {
            timestamp = other.timestamp;
            sender = other.sender;
            responders = other.responders;
            message = other.message;
            chatType = other.chatType;
            team = other.team;
            responsesGenerated = other.responsesGenerated;
            maxResponses = other.maxResponses;
            retryCount = other.retryCount;
        }
        return *this;
    }
};

class LLMChatQueue {
public:
    static LLMChatQueue* instance();
    void Initialize();
    void Shutdown();
    ~LLMChatQueue();

    bool EnqueueResponse(Player* sender, const std::vector<Player*>& responders,
                        const std::string& message, uint32 chatType, TeamId team);
    bool IsFull() const { return m_queue.size() >= 25; }

private:
    LLMChatQueue() = default;
    static LLMChatQueue* _instance;

    void WorkerThread();
    bool ProcessResponse(QueuedResponse& response);
    void HandleFailedResponse(QueuedResponse& response);
    bool ValidateResponse(const QueuedResponse& response) const;
    void CleanupQueue();

    std::queue<QueuedResponse> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::thread m_workerThread;
    bool m_shutdown{false};
    std::atomic<uint32> m_activeApiCalls{0};
};

#define sLLMChatQueue LLMChatQueue::instance()

#endif // MOD_LLM_CHAT_QUEUE_H 