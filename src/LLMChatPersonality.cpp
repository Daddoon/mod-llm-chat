#include "LLMChatPersonality.h"
#include "LLMChatLogger.h"
#include "Random.h"
#include "Player.h"
#include "Group.h"
#include "Guild.h"
#include "DBCStores.h"
#include "SharedDefines.h"
#include "Config.h"
#include <fstream>
#include <algorithm>
#include <sstream>

// Static member initialization
std::vector<Personality> LLMChatPersonality::g_personalities;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_emotion_types;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_faction_data;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_race_data;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_class_data;

bool LLMChatPersonality::LoadPersonalities(const std::string& filename) {
    try {
        if (!sConfigMgr->LoadModulesConfigs()) {
            LLMChatLogger::LogError("Failed to load personality file: " + filename);
            return false;
        }

        g_personalities.clear();
        
        // Load personalities from config
        uint32 count = sConfigMgr->GetOption<uint32>("Personalities.Count", 0);
        for (uint32 i = 0; i < count; ++i) {
            try {
                std::string base = "Personality." + std::to_string(i) + ".";
                Personality personality;
                
                personality.id = sConfigMgr->GetOption<std::string>(base + "Id", "");
                if (personality.id.empty()) {
                    continue;
                }
                
                personality.name = sConfigMgr->GetOption<std::string>(base + "Name", "");
                personality.prompt = sConfigMgr->GetOption<std::string>(base + "Prompt", "");
                personality.base_context = sConfigMgr->GetOption<std::string>(base + "BaseContext", "");
                
                // Load emotions
                std::string emotions = sConfigMgr->GetOption<std::string>(base + "Emotions", "");
                std::istringstream iss(emotions);
                std::string emotion;
                while (std::getline(iss, emotion, ',')) {
                    personality.emotions.push_back(emotion);
                }
                
                g_personalities.push_back(personality);
            }
            catch (const std::exception& e) {
                LLMChatLogger::LogError("Error parsing personality " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
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
    
    for (const auto& emotion_pair : g_emotion_types) {
        const std::string& emotion = emotion_pair.first;
        const auto& data = emotion_pair.second;
        int score = 0;
        
        if (data.find("typical_phrases") != data.end()) {
            for (const auto& phrase : data.at("typical_phrases")) {
                std::string keyword{phrase};
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
    for (const auto& emotion_pair : g_emotion_types) {
        const std::string& emotion = emotion_pair.first;
        const auto& data = emotion_pair.second;
        int score = 0;
        if (data.find("typical_phrases") != data.end()) {
            for (const auto& phrase : data.at("typical_phrases")) {
                std::string keyword{phrase};
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
    if (g_emotion_types.find(tone) != g_emotion_types.end()) {
        const auto& tone_data = g_emotion_types.at(tone);
        if (tone_data.find("response_style") != tone_data.end()) {
            std::string style = tone_data.at("response_style");
            return "You are a " + style + " adventurer in World of Warcraft. " +
                   "Maintain this tone while staying true to the game's setting and lore.";
        }
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
    details.level = player->GetLevel();
    details.guild = GetGuildInfo(player);
    details.title = GetCharacterTitle(player);
    details.current_zone = GetZoneName(player->GetZoneId());
    details.is_in_combat = player->IsInCombat();
    details.is_in_group = player->GetGroup() != nullptr;
    details.is_in_raid = player->GetGroup() && player->GetGroup()->isRaidGroup();
    details.is_pvp_flagged = player->IsPvP();

    // Load RP profile if available
    LoadRPProfile(details, player->GetName());

    return details;
}

CharacterDetails LLMChatPersonality::GetCharacterDetailsFromDB(std::string const& name) {
    CharacterDetails details = QueryCharacterFromDB(name);
    if (!details.name.empty()) {
        LoadRPProfile(details, name);
    }
    return details;
}

std::string LLMChatPersonality::GetFactionName(TeamId faction) {
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

    // Get the player's chosen title ID
    uint32 titleId = player->GetUInt32Value(PLAYER_CHOSEN_TITLE);
    if (CharTitlesEntry const* titleEntry = sCharTitlesStore.LookupEntry(titleId))
        return titleEntry->nameMale[0];
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
    QueryResult result = CharacterDatabase.Query(
        "SELECT rp_profile FROM `{}`.character_rp_profiles WHERE name = '{}'",
        LLM_Config.Database.CustomDB.c_str(),
        character_name.c_str());

    if (result) {
        Field* fields = result->Fetch();
        details.rp_profile = fields[0].Get<std::string>();
        return true;
    }
    return false;
}

void LLMChatPersonality::SaveRPProfile(const CharacterDetails& details) {
    CharacterDatabase.Query(
        "REPLACE INTO `{}`.character_rp_profiles (name, rp_profile) VALUES ('{}', '{}')",
        LLM_Config.Database.CustomDB.c_str(),
        details.name.c_str(),
        details.rp_profile.c_str());
}

CharacterDetails LLMChatPersonality::QueryCharacterFromDB(std::string const& name) {
    CharacterDetails details;
    QueryResult result = CharacterDatabase.Query(
        "SELECT race, class, level, zone FROM `{}`.characters WHERE name = '{}'",
        LLM_Config.Database.CharacterDB.c_str(),
        name.c_str());

    if (result) {
        Field* fields = result->Fetch();
        details.name = name;
        details.race = GetRaceName(fields[0].Get<uint8>());
        details.class_type = GetClassName(fields[1].Get<uint8>());
        details.level = fields[2].Get<uint8>();
        details.current_zone = GetZoneName(fields[3].Get<uint32>());
    }
    return details;
}

 