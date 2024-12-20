CU_ADD_LIBRARY(mod_llm_chat PRIVATE
  src/mod_llm_chat.cpp
  src/mod_llm_chat.h
  src/mod_llm_chat_loader.cpp
)

target_include_directories(mod_llm_chat
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server/game
    ${CMAKE_SOURCE_DIR}/src/server/shared
    ${CMAKE_SOURCE_DIR}/src/common
)

find_package(CURL REQUIRED)
target_link_libraries(mod_llm_chat
  PRIVATE
    acore-core-interface
    CURL::libcurl
)

install(TARGETS mod_llm_chat
  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin/modules
)