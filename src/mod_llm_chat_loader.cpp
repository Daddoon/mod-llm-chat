#include "ScriptMgr.h"
#include "mod_llm_chat.h"
#include "Define.h"

void Add_LLMChatScripts();

// This is the function AzerothCore looks for
void Addmod_llm_chatScripts()
{
    Add_LLMChatScripts();
}

// Add all scripts
extern "C" void AddSC_LLMChat()
{
    Add_LLMChatScripts();
} 