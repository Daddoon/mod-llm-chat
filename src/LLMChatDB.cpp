#include "LLMChatDB.h"
#include "LLMChatLogger.h"
#include "mod_llm_chat.h"
#include <sstream>

bool LLMChatDB::Initialize() {
    // No need to create database anymore since we're using characters database
    return true;
}

void LLMChatDB::LoadBotPersonality(uint32 guid, std::map<std::string, std::string>& personality) {
    try {
        QueryResult result = CharacterDatabase.Query(
            "SELECT trait_key, trait_value FROM bot_llmchat_personalities WHERE guid = %u",
            guid);

        if (result) {
            do {
                Field* fields = result->Fetch();
                personality[fields[0].GetString()] = fields[1].GetString();
            } while (result->NextRow());
        }
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to load bot personality: " + std::string(e.what()));
    }
}

void LLMChatDB::SaveBotPersonality(uint32 guid, const std::map<std::string, std::string>& personality) {
    try {
        for (const auto& [key, value] : personality) {
            CharacterDatabase.Query(
                "REPLACE INTO bot_llmchat_personalities (guid, trait_key, trait_value) VALUES (%u, '%s', '%s')",
                guid, key.c_str(), value.c_str());
        }
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to save bot personality: " + std::string(e.what()));
    }
}

bool LLMChatDB::LoadBotBackstory(uint32 guid, std::string& backstory) {
    try {
        QueryResult result = CharacterDatabase.Query(
            "SELECT backstory FROM bot_llmchat_backstories WHERE guid = %u",
            guid);

        if (result) {
            Field* fields = result->Fetch();
            backstory = fields[0].GetString();
            return true;
        }
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to load bot backstory: " + std::string(e.what()));
    }
    return false;
}

void LLMChatDB::SaveBotBackstory(uint32 guid, const std::string& backstory) {
    try {
        CharacterDatabase.Query(
            "REPLACE INTO bot_llmchat_backstories (guid, backstory) VALUES (%u, '%s')",
            guid, backstory.c_str());
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to save bot backstory: " + std::string(e.what()));
    }
}

void LLMChatDB::SaveConversationHistory(uint32 botGuid, uint32 playerGuid, const std::string& conversation) {
    try {
        CharacterDatabase.Query(
            "INSERT INTO bot_llmchat_conversations (bot_guid, player_guid, conversation) VALUES (%u, %u, '%s')",
            botGuid, playerGuid, conversation.c_str());
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to save conversation history: " + std::string(e.what()));
    }
}

std::string LLMChatDB::GetConversationHistory(uint32 botGuid, uint32 playerGuid) {
    std::string history;
    try {
        QueryResult result = CharacterDatabase.Query(
            "SELECT conversation FROM bot_llmchat_conversations "
            "WHERE bot_guid = %u AND player_guid = %u "
            "ORDER BY timestamp DESC LIMIT 10",
            botGuid, playerGuid);

        if (result) {
            do {
                Field* fields = result->Fetch();
                history += fields[0].GetString() + "\n";
            } while (result->NextRow());
        }
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to get conversation history: " + std::string(e.what()));
    }
    return history;
}

void LLMChatDB::SaveEmotionalState(uint32 guid, const std::string& emotion, uint32 intensity) {
    try {
        CharacterDatabase.Query(
            "REPLACE INTO bot_llmchat_emotions (guid, emotion, intensity) VALUES (%u, '%s', %u)",
            guid, emotion.c_str(), intensity);
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to save emotional state: " + std::string(e.what()));
    }
}

std::pair<std::string, uint32> LLMChatDB::GetEmotionalState(uint32 guid) {
    try {
        QueryResult result = CharacterDatabase.Query(
            "SELECT emotion, intensity FROM bot_llmchat_emotions WHERE guid = %u",
            guid);

        if (result) {
            Field* fields = result->Fetch();
            return {fields[0].GetString(), fields[1].GetUInt32()};
        }
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to get emotional state: " + std::string(e.what()));
    }
    return {"neutral", 0};
}

void LLMChatDB::SaveRelationship(uint32 botGuid, uint32 playerGuid, int32 standing) {
    try {
        CharacterDatabase.Query(
            "INSERT INTO bot_llmchat_relationships (bot_guid, target_guid, standing, interaction_count) "
            "VALUES (%u, %u, %d, 1) "
            "ON DUPLICATE KEY UPDATE "
            "standing = standing + %d, "
            "interaction_count = interaction_count + 1, "
            "last_interaction = CURRENT_TIMESTAMP",
            botGuid, playerGuid, standing, standing);
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to save relationship: " + std::string(e.what()));
    }
}

int32 LLMChatDB::GetRelationship(uint32 botGuid, uint32 playerGuid) {
    try {
        QueryResult result = CharacterDatabase.Query(
            "SELECT standing FROM bot_llmchat_relationships "
            "WHERE bot_guid = %u AND target_guid = %u",
            botGuid, playerGuid);

        if (result) {
            Field* fields = result->Fetch();
            return fields[0].GetInt32();
        }
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to get relationship: " + std::string(e.what()));
    }
    return 0;
}

bool LLMChatDB::ExecuteQuery(const char* sql) {
    try {
        CharacterDatabase.Execute(sql);
        return true;
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Failed to execute query: " + std::string(e.what()));
        return false;
    }
} 