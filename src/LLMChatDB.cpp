#include "LLMChatDB.h"
#include "Field.h"
#include "QueryResult.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <sstream>
#include <memory>

bool LLMChatDB::Initialize()
{
    return true;
}

void LLMChatDB::LoadBotPersonality(uint32 guid, std::map<std::string, std::string>& personality)
{
    std::stringstream query;
    query << "SELECT `TraitName`, `TraitValue` FROM `bot_llmchat_personalities` WHERE `BotGUID` = " << guid;

    if (auto result = WorldDatabase.Query(query.str().c_str()))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string key = fields[0].Get<std::string>();
            std::string value = fields[1].Get<std::string>();
            if (!key.empty() && !value.empty())
            {
                personality[key] = value;
            }
        } while (result->NextRow());
    }
}

bool LLMChatDB::LoadBotBackstory(uint32 guid, std::string& backstory)
{
    std::stringstream query;
    query << "SELECT `Backstory` FROM `bot_llmchat_backstories` WHERE `BotGUID` = " << guid;

    if (auto result = WorldDatabase.Query(query.str().c_str()))
    {
        Field* fields = result->Fetch();
        std::string value = fields[0].Get<std::string>();
        if (!value.empty())
        {
            backstory = value;
            return true;
        }
    }
    return false;
}

std::string LLMChatDB::GetConversationHistory(uint32 botGuid, uint32 playerGuid)
{
    try
    {
        std::stringstream query;
        query << "SELECT `Message`, `Response`, `Timestamp` FROM `bot_llmchat_conversations` "
              << "WHERE `BotGUID` = " << botGuid << " AND `PlayerGUID` = " << playerGuid
              << " ORDER BY `Timestamp` DESC LIMIT 5";
            
        if (auto result = WorldDatabase.Query(query.str().c_str()))
        {
            std::stringstream history;
            do
            {
                Field* fields = result->Fetch();
                std::string message = fields[0].Get<std::string>();
                std::string response = fields[1].Get<std::string>();
                if (!message.empty() && !response.empty())
                {
                    history << "Player: " << message << "\n";
                    history << "Bot: " << response << "\n\n";
                }
            } while (result->NextRow());
            return history.str();
        }
    }
    catch (const std::exception& e)
    {
        printf("Error getting conversation history: %s\n", e.what());
    }
    return "";
}

std::pair<std::string, uint32> LLMChatDB::GetEmotionalState(uint32 guid)
{
    try
    {
        std::stringstream query;
        query << "SELECT `EmotionType`, `Intensity` FROM `bot_llmchat_emotions` "
              << "WHERE `BotGUID` = " << guid << " ORDER BY `Timestamp` DESC LIMIT 1";
            
        if (auto result = WorldDatabase.Query(query.str().c_str()))
        {
            Field* fields = result->Fetch();
            std::string emotion = fields[0].Get<std::string>();
            uint32 intensity = fields[1].Get<uint32>();
            return std::make_pair(!emotion.empty() ? emotion : "neutral", intensity);
        }
    }
    catch (const std::exception& e)
    {
        printf("Error getting emotional state: %s\n", e.what());
    }
    return std::make_pair("neutral", 0);
}

int32 LLMChatDB::GetRelationship(uint32 botGuid, uint32 playerGuid)
{
    try
    {
        std::stringstream query;
        query << "SELECT `Standing` FROM `bot_llmchat_relationships` "
              << "WHERE `BotGUID` = " << botGuid << " AND `PlayerGUID` = " << playerGuid;
            
        if (auto result = WorldDatabase.Query(query.str().c_str()))
        {
            Field* fields = result->Fetch();
            return fields[0].Get<int32>();
        }
    }
    catch (const std::exception& e)
    {
        printf("Error getting relationship: %s\n", e.what());
    }
    return 0;
}

bool LLMChatDB::ExecuteQuery(const char* sql)
{
    try
    {
        WorldDatabase.DirectExecute(sql);
        return true;
    }
    catch (const std::exception& e)
    {
        printf("Error executing query: %s\n", e.what());
        return false;
    }
} 