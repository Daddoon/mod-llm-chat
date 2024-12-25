#ifndef _LLMCHAT_PERSONALITY_H
#define _LLMCHAT_PERSONALITY_H

#include "Define.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "mod-llm-chat-config.h"
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
    bool is_in_combat;
    bool is_in_group;
    bool is_in_raid;
    bool is_pvp_flagged;
};

struct Personality {
    std::string id;
    std::string name;
    std::string prompt;
    std::string base_context;
    std::vector<std::string> emotions;
    std::map<std::string, std::string> traits;
    struct ChatStyle {
        std::vector<std::string> typical_phrases;
    } chat_style;
};

struct EmotionType {
    std::vector<std::string> typical_phrases;
    std::string response_style;
};

class LLMChatPersonality {
public:
    static bool LoadPersonalities(const std::string& filename);
    static Personality SelectPersonality(const std::string& emotion, Player* sender = nullptr, Player* recipient = nullptr);
    static std::string DetectEmotion(const std::string& message);
    static std::string DetectTone(const std::string& message);
    static std::string GetMoodBasedResponse(const std::string& tone, Player* sender = nullptr, Player* recipient = nullptr);
    static CharacterDetails GetCharacterDetails(Player* player);
    static CharacterDetails GetCharacterDetailsFromDB(std::string const& name);
    static std::string BuildContext(const Personality& personality, Player* player);
    static size_t GetLoadedPersonalitiesCount() { return g_personalities.size(); }

private:
    static std::string GetFactionName(TeamId faction);
    static std::string GetRaceName(uint8 race);
    static std::string GetClassName(uint8 class_type);
    static std::string GetZoneName(uint32 zone_id);
    static std::string GetCharacterTitle(Player* player);
    static std::string GetGuildInfo(Player* player);
    static bool LoadRPProfile(CharacterDetails& details, std::string const& character_name);
    static void SaveRPProfile(const CharacterDetails& details);
    static CharacterDetails QueryCharacterFromDB(std::string const& name);

    static std::vector<Personality> g_personalities;
    static std::map<std::string, EmotionType> g_emotion_types;
    static std::map<std::string, std::map<std::string, std::string>> g_faction_data;
    static std::map<std::string, std::map<std::string, std::string>> g_race_data;
    static std::map<std::string, std::map<std::string, std::string>> g_class_data;
};

#endif // _LLMCHAT_PERSONALITY_H 