#ifndef MOD_LLM_CHAT_DB_H
#define MOD_LLM_CHAT_DB_H

#include "Define.h"
#include <map>
#include <string>
#include <utility>

class LLMChatDB
{
public:
    static bool Initialize();
    static void LoadBotPersonality(uint32 guid, std::map<std::string, std::string>& personality);
    static void SaveBotPersonality(uint32 guid, const std::map<std::string, std::string>& personality);
    static bool LoadBotBackstory(uint32 guid, std::string& backstory);
    static void SaveBotBackstory(uint32 guid, const std::string& backstory);
    static void SaveConversationHistory(uint32 botGuid, uint32 playerGuid, const std::string& conversation);
    static std::string GetConversationHistory(uint32 botGuid, uint32 playerGuid);
    static void SaveEmotionalState(uint32 guid, const std::string& emotion, uint32 intensity);
    static std::pair<std::string, uint32> GetEmotionalState(uint32 guid);
    static void SaveRelationship(uint32 botGuid, uint32 playerGuid, int32 standing);
    static int32 GetRelationship(uint32 botGuid, uint32 playerGuid);

private:
    static bool ExecuteQuery(const char* sql);
};

#endif // MOD_LLM_CHAT_DB_H 