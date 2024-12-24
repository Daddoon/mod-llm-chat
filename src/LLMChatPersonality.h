#ifndef MOD_LLM_CHAT_PERSONALITY_H
#define MOD_LLM_CHAT_PERSONALITY_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Player.h"
#include "ObjectMgr.h"
#include "Guild.h"
#include "DatabaseEnv.h"

struct CharacterDetails {
    std::string name;
    std::string faction;
    std::string race;
    std::string class_type;
    uint32_t level;
    std::string guild;
    std::string title;
    std::string current_zone;
    bool is_in_combat;
    bool is_in_group;
    bool is_in_raid;
    bool is_pvp_flagged;

    // Optional RP details
    std::string rp_profile;
    std::string character_history;
    std::string notable_achievements;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CharacterDetails, name, faction, race, class_type, level, 
                                  guild, title, current_zone, is_in_combat, is_in_group, 
                                  is_in_raid, is_pvp_flagged, rp_profile, character_history, 
                                  notable_achievements)
};

struct Personality {
    std::string id;
    std::string name;
    std::string prompt;
    std::string base_context;
    std::vector<std::string> emotions;
    nlohmann::json traits;
    std::vector<std::string> knowledge_base;
    nlohmann::json chat_style;
    nlohmann::json response_patterns;

    // Faction and race-specific responses
    nlohmann::json faction_responses;
    nlohmann::json race_responses;
    nlohmann::json class_responses;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Personality, id, name, prompt, base_context, emotions, 
                                  traits, knowledge_base, chat_style, response_patterns, 
                                  faction_responses, race_responses, class_responses)
};

class LLMChatPersonality {
public:
    static bool LoadPersonalities(const std::string& filename);
    
    // Updated to use Player object directly
    static Personality SelectPersonality(const std::string& emotion,
                                       Player* sender,
                                       Player* recipient);
    
    static std::string DetectEmotion(const std::string& message);
    static std::string DetectTone(const std::string& message);
    
    // Updated to use Player object directly
    static std::string GetMoodBasedResponse(const std::string& tone,
                                          Player* sender,
                                          Player* recipient);

    // Character detail fetching methods
    static CharacterDetails GetCharacterDetails(Player* player);
    static CharacterDetails GetCharacterDetailsFromDB(std::string const& name);
    
    // Helper methods for character details
    static std::string GetFactionName(uint32 faction);
    static std::string GetRaceName(uint8 race);
    static std::string GetClassName(uint8 class_type);
    static std::string GetZoneName(uint32 zone_id);
    static std::string GetCharacterTitle(Player* player);
    static std::string GetGuildInfo(Player* player);
    
    // RP profile methods
    static bool LoadRPProfile(CharacterDetails& details, std::string const& character_name);
    static void SaveRPProfile(const CharacterDetails& details);

    // Existing response methods
    static std::string GetFactionSpecificResponse(const std::string& faction,
                                                const std::string& response_type);
    static std::string GetRaceSpecificResponse(const std::string& race,
                                             const std::string& response_type);
    static std::string GetClassSpecificResponse(const std::string& class_type,
                                              const std::string& response_type);
    static std::string BuildCharacterContext(const CharacterDetails& character);

private:
    static std::vector<Personality> g_personalities;
    static nlohmann::json g_emotion_types;
    static nlohmann::json g_faction_data;
    static nlohmann::json g_race_data;
    static nlohmann::json g_class_data;

    // Helper methods for database operations
    static CharacterDetails QueryCharacterFromDB(std::string const& name);
    static bool LoadCharacterAchievements(CharacterDetails& details, uint32 guid);
    static bool LoadCharacterHistory(CharacterDetails& details, uint32 guid);
};

#endif // MOD_LLM_CHAT_PERSONALITY_H 