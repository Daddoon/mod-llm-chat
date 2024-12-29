#include "LLMChatCharacter.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Log.h"
#include "World.h"
#include "Map.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "mod-llm-chat-config.h"
#include <fmt/format.h>

// Initialize static members
std::map<std::string, std::map<std::string, std::string>> LLMChatCharacter::g_faction_data;
std::map<std::string, std::map<std::string, std::string>> LLMChatCharacter::g_race_data;
std::map<std::string, std::map<std::string, std::string>> LLMChatCharacter::g_class_data;

CharacterDetails LLMChatCharacter::GetCharacterDetails(Player* player) {
    CharacterDetails details;
    if (!player)
        return details;

    details.name = player->GetName();
    details.level = player->GetLevel();
    details.className = player->getClass() ? sChrClassesStore.LookupEntry(player->getClass())->name[0] : "Unknown";
    details.raceName = player->getRace() ? sChrRacesStore.LookupEntry(player->getRace())->name[0] : "Unknown";
    details.faction = GetFactionName(player->GetTeamId());
    
    // Build description combining race and class flavor
    details.description = fmt::format("{} {}", details.raceName, details.className);
    
    // Get current zone/area name
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId()))
        details.location = area->area_name[0];
    else
        details.location = "Unknown Location";

    // Get guild info if any
    if (Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId()))
        details.guildName = guild->GetName();
    
    details.isInCombat = player->IsInCombat();
    details.healthPct = player->GetHealthPct();
    
    // Get target info if any
    if (Unit* target = player->GetSelectedUnit())
        details.targetName = target->GetName();

    return details;
}

std::string LLMChatCharacter::GetFactionName(TeamId faction) {
    switch (faction) {
        case TEAM_ALLIANCE:  // 0
            return "Alliance";
        case TEAM_HORDE:     // 1
            return "Horde";
        case TEAM_NEUTRAL:   // 2
            return "Neutral";
        default:
            return "Unknown";
    }
}

std::string LLMChatCharacter::GetRaceName(uint8 race) {
    switch (race) {
        case RACE_HUMAN:         // 1
            return "Human";
        case RACE_ORC:           // 2
            return "Orc";
        case RACE_DWARF:         // 3
            return "Dwarf";
        case RACE_NIGHTELF:      // 4
            return "Night Elf";
        case RACE_UNDEAD_PLAYER: // 5
            return "Undead";
        case RACE_TAUREN:        // 6
            return "Tauren";
        case RACE_GNOME:         // 7
            return "Gnome";
        case RACE_TROLL:         // 8
            return "Troll";
        case RACE_BLOODELF:      // 10
            return "Blood Elf";
        case RACE_DRAENEI:       // 11
            return "Draenei";
        default:
            return "Unknown";
    }
}

std::string LLMChatCharacter::GetClassName(uint8 class_type) {
    switch (class_type) {
        case CLASS_WARRIOR:      // 1
            return "Warrior";
        case CLASS_PALADIN:      // 2
            return "Paladin";
        case CLASS_HUNTER:       // 3
            return "Hunter";
        case CLASS_ROGUE:        // 4
            return "Rogue";
        case CLASS_PRIEST:       // 5
            return "Priest";
        case CLASS_DEATH_KNIGHT: // 6
            return "Death Knight";
        case CLASS_SHAMAN:       // 7
            return "Shaman";
        case CLASS_MAGE:         // 8
            return "Mage";
        case CLASS_WARLOCK:      // 9
            return "Warlock";
        case CLASS_DRUID:        // 11
            return "Druid";
        default:
            return "Unknown";
    }
}

std::string LLMChatCharacter::GetZoneName(uint32 zone_id) {
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zone_id)) {
        return zone->area_name[0];
    }
    return "Unknown";
}

std::string LLMChatCharacter::GetCharacterTitle(Player* player) {
    if (!player) {
        return "";
    }

    // Get the currently selected title
    uint32 titleId = player->GetUInt32Value(PLAYER_CHOSEN_TITLE);
    if (titleId == 0) {
        return "";
    }

    if (CharTitlesEntry const* titleInfo = sCharTitlesStore.LookupEntry(titleId)) {
        return titleInfo->nameMale[0];
    }
    return "";
}

std::string LLMChatCharacter::GetGuildInfo(Player* player) {
    if (!player) {
        return "";
    }

    if (Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId())) {
        return guild->GetName();
    }
    return "";
}

bool LLMChatCharacter::SaveRPProfile(const CharacterDetails& details) {
    try {
        CharacterDatabase.Execute(
            "INSERT INTO `{}`.character_rp_profiles (name, profile) VALUES ('{}', '{}') "
            "ON DUPLICATE KEY UPDATE profile = '{}'",
            LLM_Config.Database.CustomDB.c_str(),
            details.name.c_str(), 
            details.description.c_str(), 
            details.description.c_str());
        return true;
    }
    catch (std::exception& e) {
        LOG_ERROR("module.llm_chat", "Failed to save RP profile for {}: {}", details.name, e.what());
        return false;
    }
}

bool LLMChatCharacter::LoadRPProfile(CharacterDetails& details, std::string const& character_name) {
    try {
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT profile FROM `{}`.character_rp_profiles WHERE name = '{}'",
                LLM_Config.Database.CustomDB.c_str(),
                character_name.c_str())) {
            Field* fields = result->Fetch();
            details.description = fields[0].Get<std::string>();
            return true;
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("module.llm_chat", "Failed to load RP profile for {}: {}", character_name, e.what());
    }
    return false;
}

std::string LLMChatCharacter::BuildCharacterContext(const CharacterDetails& details) {
    std::string context = fmt::format("Character: {} - Level {} {} {}", 
        details.name, details.level, details.raceName, details.className);

    if (!details.guildName.empty()) {
        context += fmt::format("\nGuild: {}", details.guildName);
    }

    context += fmt::format("\nLocation: {}", details.location);

    std::vector<std::string> status;
    if (details.isInCombat) status.push_back("in combat");
    if (!details.targetName.empty()) status.push_back(fmt::format("targeting {}", details.targetName));

    if (!status.empty()) {
        context += "\nStatus: Currently " + status[0];
        for (size_t i = 1; i < status.size(); ++i) {
            context += ", " + status[i];
        }
    }

    if (!details.description.empty()) {
        context += "\nDescription: " + details.description;
    }

    return context;
}

CharacterDetails LLMChatCharacter::GetCharacterDetailsFromDB(std::string const& name) {
    return QueryCharacterFromDB(name);
}

CharacterDetails LLMChatCharacter::QueryCharacterFromDB(std::string const& name) {
    CharacterDetails details;
    try {
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT name, race, class, level, guild_id FROM `{}`.characters WHERE name = '{}'",
                LLM_Config.Database.CharacterDB.c_str(),
                name.c_str())) {
            Field* fields = result->Fetch();
            details.name = fields[0].Get<std::string>();
            details.raceName = GetRaceName(fields[1].Get<uint8>());
            details.className = GetClassName(fields[2].Get<uint8>());
            details.level = fields[3].Get<uint32>();
            
            uint32 guildId = fields[4].Get<uint32>();
            if (guildId > 0) {
                if (QueryResult guildResult = CharacterDatabase.Query(
                        "SELECT name FROM `{}`.guild WHERE guildid = {}",
                        LLM_Config.Database.CharacterDB.c_str(),
                        guildId)) {
                    details.guildName = guildResult->Fetch()[0].Get<std::string>();
                }
            }

            LoadRPProfile(details, name);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("module.llm_chat", "Failed to query character details for {}: {}", name, e.what());
    }
    return details;
} 