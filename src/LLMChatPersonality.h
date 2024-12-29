#ifndef MOD_LLM_CHAT_PERSONALITY_H
#define MOD_LLM_CHAT_PERSONALITY_H

#include "LLMChatCharacter.h"
#include <string>
#include <map>
#include <vector>

struct Personality {
    std::string id;
    std::string name;
    std::string prompt;
    std::string base_context;
    std::vector<std::string> emotions;
    std::map<std::string, std::string> traits;
};

struct EmotionType {
    std::vector<std::string> typical_phrases;
    std::string response_style;
};

class LLMChatPersonality
{
public:
    static void Initialize();
    static bool LoadPersonalities(std::string const& filename);
    static std::string SelectPersonality(const CharacterDetails& details);
    static std::string GetPersonalityPrompt(const std::string& personality);
    static std::string DetectEmotion(const std::string& message);
    static std::string DetectTone(const std::string& message);
    static std::string GetMoodBasedResponse(const std::string& tone, Player* sender = nullptr, Player* recipient = nullptr);
    static std::string BuildContext(const Personality& personality, Player* player);
    static std::string GetPersonalityContext(const CharacterDetails& details);

private:
    static std::vector<Personality> g_personalities;
    static std::map<std::string, EmotionType> g_emotion_types;
    static std::map<std::string, std::string> g_personality_prompts;
    static std::map<std::string, std::map<std::string, std::string>> g_faction_data;
    static std::map<std::string, std::map<std::string, std::string>> g_race_data;
    static std::map<std::string, std::map<std::string, std::string>> g_class_data;
    static std::map<std::string, std::vector<std::string>> g_race_personalities;
    static std::map<std::string, std::vector<std::string>> g_class_personalities;
};

#endif // MOD_LLM_CHAT_PERSONALITY_H 