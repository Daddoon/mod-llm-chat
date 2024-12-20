#include "ScriptMgr.h"
#include "mod_llm_chat.h"

// Add all scripts in function
void Add_LLMChatScripts();

// Add all scripts in function
void Addmod_llm_chatScripts()
{
    Add_LLMChatScripts();
}

// Trinity Script module initialization
void AddLLMChatScripts()
{
    Add_LLMChatScripts();
}

// Building dll hook
extern "C" void AC_EXPORT_SHARED AddScripts()
{
    AddLLMChatScripts();
} 