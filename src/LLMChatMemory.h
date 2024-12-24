#ifndef MOD_LLM_CHAT_MEMORY_H
#define MOD_LLM_CHAT_MEMORY_H

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include "Define.h"  // For uint32/int32 types

struct ConversationMemory {
    uint32 timestamp;
    std::string sender;
    std::string responder;
    std::string message;
    std::string response;
};

class LLMChatMemory {
public:
    static void AddToMemory(const std::string& sender, const std::string& responder, 
                           const std::string& message, const std::string& response);
    static std::string GetConversationContext(const std::string& sender, const std::string& responder);
    static void CleanupExpiredMemories();

private:
    static std::map<std::string, std::vector<ConversationMemory>> g_conversationHistory;
    static std::mutex g_memoryMutex;
    static std::string GetMemoryKey(const std::string& sender, const std::string& responder);
};

#endif // MOD_LLM_CHAT_MEMORY_H 