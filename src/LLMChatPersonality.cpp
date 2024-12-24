#include "LLMChatPersonality.h"
#include "LLMChatLogger.h"
#include "Random.h"
#include <fstream>
#include <algorithm>

// Static member initialization
std::vector<Personality> LLMChatPersonality::g_personalities;
nlohmann::json LLMChatPersonality::g_emotion_types;

bool LLMChatPersonality::LoadPersonalities(const std::string& filename) {
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            LLMChatLogger::LogError("Failed to open personality file: " + filename);
            return false;
        }

        nlohmann::json data = nlohmann::json::parse(file);
        g_personalities.clear();
        
        // Load personalities
        if (data.contains("personalities")) {
            for (const auto& item : data["personalities"]) {
                Personality personality;
                try {
                    personality.id = item["id"].get<std::string>();
                    personality.name = item["name"].get<std::string>();
                    personality.prompt = item["prompt"].get<std::string>();
                    personality.emotions = item["emotions"].get<std::vector<std::string>>();
                    personality.traits = item["traits"];
                    personality.interests = item["interests"].get<std::vector<std::string>>();
                    personality.chat_style = item["chat_style"];
                    g_personalities.push_back(personality);
                }
                catch (const std::exception& e) {
                    LLMChatLogger::LogError("Error parsing personality: " + std::string(e.what()));
                    continue;
                }
            }
        }

        // Load emotion types
        if (data.contains("emotion_types")) {
            g_emotion_types = data["emotion_types"];
        }

        LLMChatLogger::Log(2, "Loaded " + std::to_string(g_personalities.size()) + 
                             " personalities from " + filename);
        return true;
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError("Error loading personalities: " + std::string(e.what()));
        return false;
    }
}

Personality LLMChatPersonality::SelectPersonality(const std::string& emotion,
                                                Player* sender,
                                                Player* recipient) {
    CharacterDetails senderDetails = GetCharacterDetails(sender);
    CharacterDetails recipientDetails = GetCharacterDetails(recipient);
    
    std::vector<Personality> matchingPersonalities;
    
    // Find personalities that handle this emotion well
    for (const auto& personality : g_personalities) {
        if (std::find(personality.emotions.begin(), 
                      personality.emotions.end(), 
                      emotion) != personality.emotions.end()) {
            matchingPersonalities.push_back(personality);
        }
    }
    
    // If no matching personalities, use all personalities
    if (matchingPersonalities.empty()) {
        matchingPersonalities = g_personalities;
    }
    
    // Select random personality from matches
    if (matchingPersonalities.empty()) {
        // Return a default personality if no personalities are loaded
        Personality defaultPersonality;
        defaultPersonality.id = "default";
        defaultPersonality.name = "Default";
        defaultPersonality.prompt = "You are a friendly World of Warcraft player.";
        return defaultPersonality;
    }
    
    return matchingPersonalities[urand(0, matchingPersonalities.size() - 1)];
}

std::string LLMChatPersonality::DetectEmotion(const std::string& message) {
    // Convert message to lowercase for comparison
    std::string lowerMsg = message;
    std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);

    // Count emotion keywords from emotion types
    std::map<std::string, int> emotionScores;
    
    for (const auto& [emotion, data] : g_emotion_types.items()) {
        int score = 0;
        if (data.contains("typical_phrases")) {
            for (const auto& phrase : data["typical_phrases"]) {
                std::string keyword = phrase.get<std::string>();
                std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::tolower);
                size_t pos = 0;
                while ((pos = lowerMsg.find(keyword, pos)) != std::string::npos) {
                    score++;
                    pos += keyword.length();
                }
            }
        }
        emotionScores[emotion] = score;
    }

    // Find emotion with highest score
    std::string dominantEmotion = "Friendly"; // Default
    int maxScore = 0;
    
    for (const auto& [emotion, score] : emotionScores) {
        if (score > maxScore) {
            maxScore = score;
            dominantEmotion = emotion;
        }
    }

    LLMChatLogger::LogDebug("Detected emotion '" + dominantEmotion + "' for message: " + message);
    return dominantEmotion;
}

std::string LLMChatPersonality::DetectTone(const std::string& message) {
    // Convert message to lowercase for easier matching
    std::string lowerMsg = message;
    std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);

    // Initialize scores for each tone
    std::map<std::string, int> toneScores;
    
    // Score each tone based on emotion types phrases
    for (const auto& [emotion, data] : g_emotion_types.items()) {
        int score = 0;
        if (data.contains("typical_phrases")) {
            for (const auto& phrase : data["typical_phrases"]) {
                std::string keyword = phrase.get<std::string>();
                std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::tolower);
                size_t pos = 0;
                while ((pos = lowerMsg.find(keyword, pos)) != std::string::npos) {
                    score++;
                    pos += keyword.length();
                }
            }
        }
        toneScores[emotion] = score;
    }

    // Find the dominant tone
    std::string dominantTone = "neutral";
    int maxScore = 0;
    
    for (const auto& [tone, score] : toneScores) {
        if (score > maxScore) {
            maxScore = score;
            dominantTone = tone;
        }
    }

    return dominantTone;
}

std::string LLMChatPersonality::GetMoodBasedResponse(const std::string& tone,
                                                    Player* sender,
                                                    Player* recipient) {
    CharacterDetails senderDetails = GetCharacterDetails(sender);
    CharacterDetails recipientDetails = GetCharacterDetails(recipient);
    
    // Get response style from emotion types if available
    if (g_emotion_types.contains(tone) && g_emotion_types[tone].contains("response_style")) {
        std::string style = g_emotion_types[tone]["response_style"].get<std::string>();
        return "You are a " + style + " adventurer in World of Warcraft. " +
               "Maintain this tone while staying true to the game's setting and lore.";
    }

    // Default response if tone not found
    return "You are a seasoned adventurer with many tales to share. "
           "Be natural but always stay true to the World of Warcraft setting.";
}

CharacterDetails LLMChatPersonality::GetCharacterDetails(Player* player) {
    if (!player) {
        LLMChatLogger::LogError("Attempted to get details for null player");
        return CharacterDetails();
    }

    CharacterDetails details;
    details.name = player->GetName();
    details.faction = GetFactionName(player->GetTeamId());
    details.race = GetRaceName(player->getRace());
    details.class_type = GetClassName(player->getClass());
    details.level = player->getLevel();
    details.guild = GetGuildInfo(player);
    details.title = GetCharacterTitle(player);
    details.current_zone = GetZoneName(player->GetZoneId());
    details.is_in_combat = player->IsInCombat();
    details.is_in_group = player->GetGroup() != nullptr;
    details.is_in_raid = player->GetGroup() && player->GetGroup()->isRaidGroup();
    details.is_pvp_flagged = player->IsPvP();

    // Load RP profile if it exists
    LoadRPProfile(details, player->GetName());
    
    // Load achievements and history
    LoadCharacterAchievements(details, player->GetGUID().GetCounter());
    LoadCharacterHistory(details, player->GetGUID().GetCounter());

    return details;
}

CharacterDetails LLMChatPersonality::GetCharacterDetailsFromDB(std::string const& name) {
    CharacterDetails details = QueryCharacterFromDB(name);
    if (!details.name.empty()) {
        LoadRPProfile(details, name);
        // Note: GUID would be fetched in QueryCharacterFromDB
        LoadCharacterAchievements(details, 0); // Use actual GUID from query
        LoadCharacterHistory(details, 0); // Use actual GUID from query
    }
    return details;
}

std::string LLMChatPersonality::GetFactionName(uint32 faction) {
    switch (faction) {
        case TEAM_ALLIANCE: return "Alliance";
        case TEAM_HORDE: return "Horde";
        default: return "Neutral";
    }
}

std::string LLMChatPersonality::GetRaceName(uint8 race) {
    switch (race) {
        case RACE_HUMAN: return "Human";
        case RACE_ORC: return "Orc";
        case RACE_DWARF: return "Dwarf";
        case RACE_NIGHTELF: return "Night Elf";
        case RACE_UNDEAD_PLAYER: return "Undead";
        case RACE_TAUREN: return "Tauren";
        case RACE_GNOME: return "Gnome";
        case RACE_TROLL: return "Troll";
        case RACE_BLOODELF: return "Blood Elf";
        case RACE_DRAENEI: return "Draenei";
        default: return "Unknown";
    }
}

std::string LLMChatPersonality::GetClassName(uint8 class_type) {
    switch (class_type) {
        case CLASS_WARRIOR: return "Warrior";
        case CLASS_PALADIN: return "Paladin";
        case CLASS_HUNTER: return "Hunter";
        case CLASS_ROGUE: return "Rogue";
        case CLASS_PRIEST: return "Priest";
        case CLASS_DEATH_KNIGHT: return "Death Knight";
        case CLASS_SHAMAN: return "Shaman";
        case CLASS_MAGE: return "Mage";
        case CLASS_WARLOCK: return "Warlock";
        case CLASS_DRUID: return "Druid";
        default: return "Unknown";
    }
}

std::string LLMChatPersonality::GetZoneName(uint32 zone_id) {
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone_id))
        return area->area_name[0];
    return "Unknown";
}

std::string LLMChatPersonality::GetCharacterTitle(Player* player) {
    if (!player)
        return "";

    if (CharTitlesEntry const* titleEntry = player->GetTitle())
        return titleEntry->name[0];
    return "";
}

std::string LLMChatPersonality::GetGuildInfo(Player* player) {
    if (!player)
        return "";

    if (Guild* guild = player->GetGuild())
        return guild->GetName();
    return "";
}

bool LLMChatPersonality::LoadRPProfile(CharacterDetails& details, std::string const& character_name) {
    // Query custom RP profile table if it exists
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT rp_profile, character_history FROM character_rp_profiles WHERE name = '%s'",
        character_name.c_str());

    if (result) {
        Field* fields = result->Fetch();
        details.rp_profile = fields[0].GetString();
        details.character_history = fields[1].GetString();
        return true;
    }
    return false;
}

void LLMChatPersonality::SaveRPProfile(const CharacterDetails& details) {
    CharacterDatabase.PExecute(
        "REPLACE INTO character_rp_profiles (name, rp_profile, character_history) VALUES ('%s', '%s', '%s')",
        details.name.c_str(),
        details.rp_profile.c_str(),
        details.character_history.c_str());
}

CharacterDetails LLMChatPersonality::QueryCharacterFromDB(std::string const& name) {
    CharacterDetails details;
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT race, class, level, zone FROM characters WHERE name = '%s'",
        name.c_str());

    if (result) {
        Field* fields = result->Fetch();
        details.name = name;
        details.race = GetRaceName(fields[0].GetUInt8());
        details.class_type = GetClassName(fields[1].GetUInt8());
        details.level = fields[2].GetUInt8();
        details.current_zone = GetZoneName(fields[3].GetUInt32());
    }
    return details;
}

bool LLMChatPersonality::LoadCharacterAchievements(CharacterDetails& details, uint32 guid) {
    // Query character_achievement table for notable achievements
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT achievement FROM character_achievement WHERE guid = %u ORDER BY date DESC LIMIT 5",
        guid);

    if (result) {
        std::string achievements;
        do {
            Field* fields = result->Fetch();
            uint32 achievementId = fields[0].GetUInt32();
            if (AchievementEntry const* achievement = sAchievementStore.LookupEntry(achievementId)) {
                if (!achievements.empty())
                    achievements += ", ";
                achievements += achievement->name[0];
            }
        } while (result->NextRow());

        details.notable_achievements = achievements;
        return true;
    }
    return false;
}

bool LLMChatPersonality::LoadCharacterHistory(CharacterDetails& details, uint32 guid) {
    // This could be expanded to include more character history data
    // For now, we'll just check if they're a veteran player
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT COUNT(*) FROM character_achievement WHERE guid = %u",
        guid);

    if (result) {
        Field* fields = result->Fetch();
        uint32 achievementCount = fields[0].GetUInt32();
        if (achievementCount > 100)
            details.character_history = "Veteran player with many achievements";
        else if (achievementCount > 50)
            details.character_history = "Experienced adventurer";
        else
            details.character_history = "Budding adventurer";
        return true;
    }
    return false;
} 