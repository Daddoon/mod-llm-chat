#ifndef MOD_LLM_CHAT_QUEUE_H
#define MOD_LLM_CHAT_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <future>
#include "Player.h"
#include "mod_llm_chat_config.h"

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
    std::promise<std::string> responsePromise;
};

class LLMChatQueue {
public:
    static LLMChatQueue* instance();
    
    void Initialize();
    void Shutdown();
    
    bool EnqueueResponse(Player* sender, const std::vector<Player*>& responders,
                        const std::string& message, uint32 chatType, TeamId team);
    
    void ProcessQueue();
    void CleanupQueue();
    
    bool IsFull() const { return m_queue.size() >= LLM_Config.Queue.Size; }
    size_t GetSize() const { return m_queue.size(); }
    
private:
    LLMChatQueue() = default;
    ~LLMChatQueue();
    
    static LLMChatQueue* _instance;
    
    std::queue<QueuedResponse> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::atomic<bool> m_shutdown{false};
    std::thread m_workerThread;
    std::atomic<uint32> m_activeApiCalls{0};
    
    void WorkerThread();
    bool ProcessSingleResponse(QueuedResponse& response);
    void HandleFailedResponse(QueuedResponse& response);
    bool ValidateResponse(const QueuedResponse& response) const;
    void TryProcessNextBatch();
};

#define sLLMChatQueue LLMChatQueue::instance()

#endif // MOD_LLM_CHAT_QUEUE_H 