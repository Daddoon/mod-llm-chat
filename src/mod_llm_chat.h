#ifndef _MOD_LLM_CHAT_H_
#define _MOD_LLM_CHAT_H_

#include "Define.h"
#include "mod_llm_chat_config.h"
#include <string>

// Function declarations
void LoadConfig();
std::string QueryLLM(const std::string& message, const std::string& sender, const std::string& recipient);
bool InitializeModule();
void CleanupModule();
void Addmod_llm_chatScripts();

#endif // _MOD_LLM_CHAT_H_