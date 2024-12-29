#ifndef MOD_LLM_CHAT_QUEUE_H
#define MOD_LLM_CHAT_QUEUE_H

#include "Common.h"
#include "Define.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "mod-llm-chat.h"
#include "Playerbots.h"
#include "LLMChatEvents.h"
#include <queue>
#include <string>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>

// Forward declarations
class BotResponseEvent;

struct LLMRequest
{
    Player* responder;
    std::string message;
    uint32 chatType;
    BotResponseEvent* responseEvent;
};

struct QueuedResponse
{
    uint64 senderGuid;
    uint64 responderGuid;
    std::string message;
    std::string personality;

    QueuedResponse(uint64 sender, uint64 responder, std::string const& msg, std::string const& pers)
        : senderGuid(sender), responderGuid(responder), message(msg), personality(pers) {}
};

class LLMChatQueue
{
public:
    static bool Initialize();
    static void Shutdown();
    static void EnqueueResponse(Player* responder, std::string const& message, std::string const& chatType);

private:
    static void ProcessQueueWorker();
    static void QueryLLM(std::string const& message, Player* responder, Player* sender, std::string const& chatType);
    static void SendDefaultResponse(Player* responder, Player* sender);
    static uint32 GetChatTypeFromString(const std::string& chatType, Player* responder);

    static std::queue<QueuedResponse> responses;
    static std::mutex m_mutex;
    static bool m_initialized;
    static bool m_running;
    static std::atomic<bool> m_processingQueue;
    static std::thread m_workerThread;
    static LLMChatQueue* s_instance;
    static std::queue<std::shared_ptr<LLMRequest>> s_requestQueue;
    static std::mutex s_queueMutex;
};

#endif 