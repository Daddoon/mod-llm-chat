#ifndef MOD_LLM_CHAT_PERSONALITY_H
#define MOD_LLM_CHAT_PERSONALITY_H

#include "Define.h"
#include "Player.h"
#include "Guild.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "mod_llm_chat.h"
#include <string>
#include <vector>
#include <map>

struct CharacterDetails {
    std::string name;
    std::string faction;
    std::string race;
    std::string class_type;
    uint32 level;
    std::string guild;
    std::string title;
    std::string current_zone;
    std::string rp_profile;
    std::string character_history;
    std::string notable_achievements;
    bool is_in_combat;
    bool is_in_group;
    bool is_in_raid;
    bool is_pvp_flagged;
};

class LLMChatPersonality {
public:
    static bool LoadPersonalities(const std::string& filename);
    static std::string DetectEmotion(const std::string& message);
    static std::string DetectTone(const std::string& message);
    static std::string GetMoodBasedResponse(const std::string& tone, Player* sender, Player* recipient);
    static CharacterDetails GetCharacterDetails(Player* player);
    static CharacterDetails GetCharacterDetailsFromDB(std::string const& name);

private:
    static std::string GetFactionName(uint32 faction);
    static std::string GetRaceName(uint8 race);
    static std::string GetClassName(uint8 class_type);
    static std::string GetZoneName(uint32 zone_id);
    static std::string GetCharacterTitle(Player* player);
    static std::string GetGuildInfo(Player* player);
    static bool LoadRPProfile(CharacterDetails& details, std::string const& character_name);
    static void SaveRPProfile(const CharacterDetails& details);
    static CharacterDetails QueryCharacterFromDB(std::string const& name);
    static bool LoadCharacterAchievements(CharacterDetails& details, uint32 guid);
    static bool LoadCharacterHistory(CharacterDetails& details, uint32 guid);
};

#endif // MOD_LLM_CHAT_PERSONALITY_H 