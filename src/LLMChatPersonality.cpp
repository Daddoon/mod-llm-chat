#include "LLMChatPersonality.h"
#include "LLMChatLogger.h"
#include "Random.h"
#include "Player.h"
#include "Group.h"
#include "Guild.h"
#include "DBCStores.h"
#include "SharedDefines.h"
#include "Config.h"
#include "World.h"
#include "GameTime.h"
#include "Weather.h"
#include "WeatherMgr.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <fmt/format.h>
#include <chrono>

using json = nlohmann::json;

// Static member initialization
std::vector<Personality> LLMChatPersonality::g_personalities;
std::map<std::string, EmotionType> LLMChatPersonality::g_emotion_types;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_faction_data;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_race_data;
std::map<std::string, std::map<std::string, std::string>> LLMChatPersonality::g_class_data;

bool LLMChatPersonality::LoadPersonalities(std::string const& filename) {
    try {
        LLMChatLogger::Log(1, Acore::StringFormat("=== Starting Personality Load ==="));
        LLMChatLogger::Log(1, Acore::StringFormat("Loading from file: {}", filename));
        LLMChatLogger::Log(1, Acore::StringFormat("Current personality count: {}", g_personalities.size()));
        
        // Check if file exists
        if (!std::filesystem::exists(filename)) {
            LLMChatLogger::LogError(Acore::StringFormat(
                "Personalities file not found at: {}", filename));
            return false;
        }

        std::ifstream file(filename);
        if (!file.is_open()) {
            LLMChatLogger::LogError("Failed to open personalities file");
            return false;
        }

        LLMChatLogger::Log(1, "File opened successfully, parsing JSON...");
        json jsonData;
        try {
            file >> jsonData;
            LLMChatLogger::Log(1, "JSON parsed successfully");
        } catch (const json::parse_error& e) {
            LLMChatLogger::LogError(Acore::StringFormat("JSON parse error: {}", e.what()));
            return false;
        }

        g_personalities.clear();
        
        if (jsonData.contains("personalities") && jsonData["personalities"].is_array()) {
            size_t totalPersonalities = jsonData["personalities"].size();
            LLMChatLogger::Log(1, Acore::StringFormat(
                "Found {} personalities in JSON", totalPersonalities));

            for (const auto& item : jsonData["personalities"]) {
                try {
                    Personality personality;
                    personality.id = item["id"].get<std::string>();
                    personality.name = item["name"].get<std::string>();
                    personality.prompt = item["prompt"].get<std::string>();
                    personality.base_context = item["base_context"].get<std::string>();
                    personality.emotions = item["emotions"].get<std::vector<std::string>>();
                    
                    // Parse traits
                    if (item.contains("traits") && item["traits"].is_object()) {
                        for (const auto& [key, value] : item["traits"].items()) {
                            personality.traits[key] = value.get<std::string>();
                        }
                    }

                    g_personalities.push_back(personality);
                    LLMChatLogger::Log(1, Acore::StringFormat(
                        "Successfully loaded personality: {} ({})", 
                        personality.name, personality.id));
                }
                catch (const std::exception& e) {
                    LLMChatLogger::LogError(Acore::StringFormat(
                        "Error parsing personality: {}", e.what()));
                    continue;
                }
            }
        } else {
            LLMChatLogger::LogError("No personalities array found in JSON");
            return false;
        }

        LLMChatLogger::Log(1, Acore::StringFormat(
            "=== Personality Load Complete ===\n"
            "Total personalities loaded: {}\n"
            "Emotion types loaded: {}", 
            g_personalities.size(), 
            g_emotion_types.size()));

        return true;
    }
    catch (const std::exception& e) {
        LLMChatLogger::LogError(Acore::StringFormat(
            "Error loading personalities: {}", e.what()));
        return false;
    }
}

std::string LLMChatPersonality::SelectPersonality(const CharacterDetails& details)
{
    // Default personality if no match is found
    std::string selectedPersonality = "default";

    // Check faction-specific personalities
    auto factionIt = g_faction_data.find(details.faction);
    if (factionIt != g_faction_data.end()) {
        // Use faction-specific personality if available
        selectedPersonality = factionIt->second["personality"];
    }

    // Check race-specific personalities
    auto raceIt = g_race_personalities.find(details.raceName);
    if (raceIt != g_race_personalities.end() && !raceIt->second.empty()) {
        // Randomly select from available race personalities
        size_t index = rand() % raceIt->second.size();
        selectedPersonality = raceIt->second[index];
    }

    // Check class-specific personalities
    auto classIt = g_class_personalities.find(details.className);
    if (classIt != g_class_personalities.end() && !classIt->second.empty()) {
        // Randomly select from available class personalities
        size_t index = rand() % classIt->second.size();
        selectedPersonality = classIt->second[index];
    }

    return selectedPersonality;
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
        
        for (const auto& phrase : data.typical_phrases) {
            std::string keyword{phrase};
            std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::tolower);
            size_t pos = 0;
            while ((pos = lowerMsg.find(keyword, pos)) != std::string::npos) {
                score++;
                pos += keyword.length();
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
        for (const auto& phrase : data.typical_phrases) {
            std::string keyword{phrase};
            std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::tolower);
            size_t pos = 0;
            while ((pos = lowerMsg.find(keyword, pos)) != std::string::npos) {
                score++;
                pos += keyword.length();
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
    CharacterDetails senderDetails = LLMChatCharacter::GetCharacterDetails(sender);
    CharacterDetails recipientDetails = LLMChatCharacter::GetCharacterDetails(recipient);
    
    // Get response style from emotion types if available
    auto it = g_emotion_types.find(tone);
    if (it != g_emotion_types.end()) {
        const auto& tone_data = it->second;
        return "You are a " + tone_data.response_style + " adventurer in World of Warcraft. " +
               "Maintain this tone while staying true to the game's setting and lore.";
    }

    // Default response if tone not found
    return "You are a seasoned adventurer with many tales to share. "
           "Be natural but always stay true to the World of Warcraft setting.";
}

std::string LLMChatPersonality::BuildContext(const Personality& personality, Player* player)
{
    std::stringstream context;
    
    // Add personality base context
    context << personality.base_context;
    
    if (player)
    {
        // Add player details
        CharacterDetails details = LLMChatCharacter::GetCharacterDetails(player);
        context << "\n\n" << LLMChatCharacter::BuildCharacterContext(details);
    }
    
    // Add personality traits
    if (!personality.traits.empty())
    {
        context << "\n\nPersonality traits:";
        for (const auto& [trait, value] : personality.traits)
        {
            context << "\n- " << trait << ": " << value;
        }
    }
    
    return context.str();
}

std::string LLMChatPersonality::GetPersonalityContext(const CharacterDetails& details) {
    // Get faction-specific personality traits
    auto factionIt = g_faction_data.find(details.raceName);
    if (factionIt != g_faction_data.end()) {
        // Add faction traits
        std::string traits;
        for (const auto& trait : factionIt->second) {
            traits += trait.first + ": " + trait.second + "\n";
        }
        return traits;
    }
    return "";
}

 