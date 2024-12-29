CU_SET_PATH("CMAKE_MOD_LLMCHAT_DIR" "${CMAKE_CURRENT_LIST_DIR}")

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/mod-llm-chat.cpp")
AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/LLMChatQueue.cpp")
AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/LLMChatEvents.cpp")
AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/LLMChatLogger.cpp")

# Find required packages
find_package(Boost REQUIRED COMPONENTS system)

# Check if playerbots module exists
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/")
    message("-- Found Playerbots module - Enabling playerbot support")
    add_definitions(-DHAS_PLAYERBOTS)
    include_directories("${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src")
else()
    message("-- Playerbots module not found - Bot detection will be disabled")
endif()

# Add Boost Beast header-only library
if(NOT TARGET Boost::beast)
    add_library(Boost::beast INTERFACE IMPORTED)
    set_target_properties(Boost::beast PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${Boost_INCLUDE_DIRS}
    )
endif()

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/server/game
        ${CMAKE_SOURCE_DIR}/src/server/game/Accounts
        ${CMAKE_SOURCE_DIR}/src/server/game/Achievements
        ${CMAKE_SOURCE_DIR}/src/server/game/AI
        ${CMAKE_SOURCE_DIR}/src/server/game/AI/CoreAI
        ${CMAKE_SOURCE_DIR}/src/server/game/Battlegrounds
        ${CMAKE_SOURCE_DIR}/src/server/game/Chat
        ${CMAKE_SOURCE_DIR}/src/server/game/Combat
        ${CMAKE_SOURCE_DIR}/src/server/game/Conditions
        ${CMAKE_SOURCE_DIR}/src/server/game/DataStores
        ${CMAKE_SOURCE_DIR}/src/server/game/DungeonFinding
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities/Creature
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities/GameObject
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities/Item
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities/Object
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities/Player
        ${CMAKE_SOURCE_DIR}/src/server/game/Entities/Unit
        ${CMAKE_SOURCE_DIR}/src/server/game/Events
        ${CMAKE_SOURCE_DIR}/src/server/game/Globals
        ${CMAKE_SOURCE_DIR}/src/server/game/Grids
        ${CMAKE_SOURCE_DIR}/src/server/game/Grids/Cells
        ${CMAKE_SOURCE_DIR}/src/server/game/Grids/Notifiers
        ${CMAKE_SOURCE_DIR}/src/server/game/Groups
        ${CMAKE_SOURCE_DIR}/src/server/game/Guilds
        ${CMAKE_SOURCE_DIR}/src/server/game/Handlers
        ${CMAKE_SOURCE_DIR}/src/server/game/Instances
        ${CMAKE_SOURCE_DIR}/src/server/game/Loot
        ${CMAKE_SOURCE_DIR}/src/server/game/Mails
        ${CMAKE_SOURCE_DIR}/src/server/game/Maps
        ${CMAKE_SOURCE_DIR}/src/server/game/Miscellaneous
        ${CMAKE_SOURCE_DIR}/src/server/game/Movement
        ${CMAKE_SOURCE_DIR}/src/server/game/Movement/MovementGenerators
        ${CMAKE_SOURCE_DIR}/src/server/game/Movement/Waypoints
        ${CMAKE_SOURCE_DIR}/src/server/game/Opcodes
        ${CMAKE_SOURCE_DIR}/src/server/game/OutdoorPvP
        ${CMAKE_SOURCE_DIR}/src/server/game/Pools
        ${CMAKE_SOURCE_DIR}/src/server/game/PrecompiledHeaders
        ${CMAKE_SOURCE_DIR}/src/server/game/Quests
        ${CMAKE_SOURCE_DIR}/src/server/game/Reputation
        ${CMAKE_SOURCE_DIR}/src/server/game/Scripting
        ${CMAKE_SOURCE_DIR}/src/server/game/Server
        ${CMAKE_SOURCE_DIR}/src/server/game/Server/Protocol
        ${CMAKE_SOURCE_DIR}/src/server/game/Skills
        ${CMAKE_SOURCE_DIR}/src/server/game/Spells
        ${CMAKE_SOURCE_DIR}/src/server/game/Spells/Auras
        ${CMAKE_SOURCE_DIR}/src/server/game/Tools
        ${CMAKE_SOURCE_DIR}/src/server/game/Warden
        ${CMAKE_SOURCE_DIR}/src/server/game/Warden/Modules
        ${CMAKE_SOURCE_DIR}/src/server/game/Weather
        ${CMAKE_SOURCE_DIR}/src/server/game/World
        ${CMAKE_SOURCE_DIR}/src/server/shared
        ${CMAKE_SOURCE_DIR}/src/server/shared/Configuration
        ${CMAKE_SOURCE_DIR}/src/server/shared/Cryptography
        ${CMAKE_SOURCE_DIR}/src/server/shared/Database
        ${CMAKE_SOURCE_DIR}/src/server/shared/DataStores
        ${CMAKE_SOURCE_DIR}/src/server/shared/Dynamic
        ${CMAKE_SOURCE_DIR}/src/server/shared/Networking
        ${CMAKE_SOURCE_DIR}/src/server/shared/Packets
        ${CMAKE_SOURCE_DIR}/src/server/shared/Threading
        ${CMAKE_SOURCE_DIR}/src/server/shared/Utilities
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src/common/Collision
        ${CMAKE_SOURCE_DIR}/src/common/Configuration
        ${CMAKE_SOURCE_DIR}/src/common/Cryptography
        ${CMAKE_SOURCE_DIR}/src/common/Debugging
        ${CMAKE_SOURCE_DIR}/src/common/Logging
        ${CMAKE_SOURCE_DIR}/src/common/Threading
        ${CMAKE_SOURCE_DIR}/src/common/Utilities
        ${CMAKE_SOURCE_DIR}/deps/fmt
        ${CMAKE_SOURCE_DIR}/deps/boost
        ${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        game-interface
        scripts-interface
        shared
        fmt::fmt
        Boost::system
        Boost::beast
        mod-playerbots
)
