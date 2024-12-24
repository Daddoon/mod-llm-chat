#include "LLMChatMemory.h"
#include "LLMChatLogger.h"
#include "mod_llm_chat_config.h"
#include "World.h"

// Static member initialization
std::map<std::string, std::vector<ConversationMemory>> LLMChatMemory::g_conversationHistory;
std::mutex LLMChatMemory::g_memoryMutex;

std::string LLMChatMemory::GetMemoryKey(const std::string& sender, const std::string& responder) {
    return sender + ":" + responder;
}

void LLMChatMemory::AddToMemory(const std::string& sender, const std::string& responder, 
                               const std::string& message, const std::string& response) {
    if (!LLM_Config.Memory.Enable) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_memoryMutex);
    
    std::string key = GetMemoryKey(sender, responder);
    auto& history = g_conversationHistory[key];
    
    // Add new memory
    ConversationMemory memory;
    memory.timestamp = getMSTime();
    memory.sender = sender;
    memory.responder = responder;
    memory.message = message;
    memory.response = response;
    
    // Add to front of history
    history.insert(history.begin(), memory);
    
    // Remove expired memories and trim to max size
    uint32 currentTime = getMSTime();
    history.erase(
        std::remove_if(history.begin(), history.end(),
            [currentTime](const ConversationMemory& mem) {
                return LLM_Config.Memory.ExpirationTime > 0 && 
                       (currentTime - mem.timestamp) > (LLM_Config.Memory.ExpirationTime * 1000);
            }),
        history.end()
    );
    
    if (history.size() > LLM_Config.Memory.MaxInteractionsPerPair) {
        history.resize(LLM_Config.Memory.MaxInteractionsPerPair);
    }
}

std::string LLMChatMemory::GetConversationContext(const std::string& sender, const std::string& responder) {
    if (!LLM_Config.Memory.Enable) {
        return "";
    }

    std::lock_guard<std::mutex> lock(g_memoryMutex);
    
    std::string key = GetMemoryKey(sender, responder);
    std::string context = "Previous conversations between you and " + sender + ":\n\n";
    
    if (g_conversationHistory.find(key) != g_conversationHistory.end()) {
        const auto& history = g_conversationHistory[key];
        
        // Remove expired memories first
        uint32 currentTime = getMSTime();
        std::vector<ConversationMemory> validMemories;
        std::copy_if(history.begin(), history.end(), std::back_inserter(validMemories),
            [currentTime](const ConversationMemory& mem) {
                return LLM_Config.Memory.ExpirationTime == 0 || 
                       (currentTime - mem.timestamp) <= (LLM_Config.Memory.ExpirationTime * 1000);
            });
        
        // Build context string with length limit
        size_t totalLength = context.length();
        for (const auto& memory : validMemories) {
            std::string interaction = 
                sender + ": " + memory.message + "\n" +
                "You: " + memory.response + "\n\n";
                
            if (totalLength + interaction.length() > LLM_Config.Memory.MaxContextLength) {
                break;
            }
            
            context += interaction;
            totalLength += interaction.length();
        }
    }
    
    return context;
}

void LLMChatMemory::CleanupExpiredMemories() {
    if (!LLM_Config.Memory.Enable || LLM_Config.Memory.ExpirationTime == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_memoryMutex);
    uint32 currentTime = getMSTime();
    
    for (auto it = g_conversationHistory.begin(); it != g_conversationHistory.end();) {
        auto& history = it->second;
        
        history.erase(
            std::remove_if(history.begin(), history.end(),
                [currentTime](const ConversationMemory& mem) {
                    return (currentTime - mem.timestamp) > (LLM_Config.Memory.ExpirationTime * 1000);
                }),
            history.end()
        );
        
        if (history.empty()) {
            it = g_conversationHistory.erase(it);
        } else {
            ++it;
        }
    }
} 