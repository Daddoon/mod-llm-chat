# Debug path information
message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")
message(STATUS "Checking for playerbots at: ${CMAKE_SOURCE_DIR}/modules/mod-playerbots")
message(STATUS "Current module path: ${CMAKE_CURRENT_SOURCE_DIR}")

# Define the mod_llm_chat target
add_library(mod_llm_chat MODULE
    src/mod-llm-chat.cpp
    src/LLMChatQueue.cpp
    src/LLMChatPersonality.cpp
    src/LLMChatMemory.cpp
)

# Include directories
target_include_directories(mod_llm_chat
    PRIVATE
    ${CMAKE_SOURCE_DIR}/modules
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/strategy
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/strategy/rpg
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/PlayerbotAI
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/RandomPlayerbotMgr
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/shared
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/shared/value
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/shared/value/context
    ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/shared/context
    ${CMAKE_SOURCE_DIR}/modules/mod_llm_chat/src
)

# Debug available targets
get_property(targets DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
message(STATUS "Available targets:")
foreach(target ${targets})
    message(STATUS "  - ${target}")
endforeach()

# Check if playerbots module exists
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots")
    message(STATUS "Found Playerbots module - setting up dependency")
    # Add playerbots as a compile-time dependency
    target_link_libraries(mod_llm_chat
        PRIVATE
        game
        scripts
    )
    
    # Add compile definition to indicate playerbots is available
    target_compile_definitions(mod_llm_chat 
        PRIVATE 
        -DHAS_PLAYERBOTS
    )
else()
    message(STATUS "Attempted paths:")
    message(STATUS " - ${CMAKE_SOURCE_DIR}")
    message(STATUS " - ${CMAKE_SOURCE_DIR}/modules")
    message(STATUS " - ${CMAKE_SOURCE_DIR}/modules/mod-playerbots")
    message(FATAL_ERROR "Playerbots module not found - required for mod_llm_chat")
endif()

target_compile_features(mod_llm_chat 
    PRIVATE 
    cxx_std_20
)

target_compile_definitions(mod_llm_chat 
    PRIVATE 
    -DHAS_PLAYERBOTS
)
