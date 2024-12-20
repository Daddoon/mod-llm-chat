AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/mod_llm_chat.cpp")
AC_ADD_SCRIPT_LOADER("mod_llm_chat" "${CMAKE_CURRENT_LIST_DIR}/src/mod_llm_chat_loader.cpp")

find_package(CURL REQUIRED)
target_link_libraries(worldserver PRIVATE CURL::libcurl)

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod_llm_chat.conf.dist")