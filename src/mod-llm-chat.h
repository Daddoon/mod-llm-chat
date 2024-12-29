#ifndef MOD_LLM_CHAT_H
#define MOD_LLM_CHAT_H

#include "mod-llm-chat-config.h"
#include <atomic>

// Global shutdown flag
extern std::atomic<bool> g_moduleShutdown;

// Module functions
void LoadConfig();
void StartModule();
void StopModule();
bool QueryLLM(std::string const& message, std::string& response);

#endif // MOD_LLM_CHAT_H