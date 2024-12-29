#ifndef MOD_LLM_CHAT_CHARACTER_H
#define MOD_LLM_CHAT_CHARACTER_H

#include "Define.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectMgr.h"
#include <string>
#include <map>
#include <vector>

struct CharacterDetails {
    std::string name;
    uint32 level;
    std::string className;
    std::string raceName;
    std::string faction;     // Alliance/Horde/Neutral
    std::string description;  // Character's description including race/class flavor
    std::string location;     // Current zone/area name
    std::string guildName;
    bool isInCombat;
    float healthPct;
    std::string targetName;
};

class LLMChatCharacter {
public:
    static CharacterDetails GetCharacterDetails(Player* player);
    static CharacterDetails GetCharacterDetailsFromDB(std::string const& name);
    static std::string BuildCharacterContext(const CharacterDetails& details);
    static bool SaveRPProfile(const CharacterDetails& details);
    static bool LoadRPProfile(CharacterDetails& details, std::string const& character_name);

private:
    static std::string GetFactionName(TeamId faction);
    static std::string GetRaceName(uint8 race);
    static std::string GetClassName(uint8 class_type);
    static std::string GetZoneName(uint32 zone_id);
    static std::string GetCharacterTitle(Player* player);
    static std::string GetGuildInfo(Player* player);
    static CharacterDetails QueryCharacterFromDB(std::string const& name);

    // Data maps for character information
    static std::map<std::string, std::map<std::string, std::string>> g_faction_data;
    static std::map<std::string, std::map<std::string, std::string>> g_race_data;
    static std::map<std::string, std::map<std::string, std::string>> g_class_data;
};

#endif // MOD_LLM_CHAT_CHARACTER_H 