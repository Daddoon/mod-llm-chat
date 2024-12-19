# Define minimum CMake version
cmake_minimum_required(VERSION 3.15)

# Define the project
project(mod_llm_chat)

# Make Ollama optional
option(USE_OLLAMA "Enable Ollama integration" ON)

# Find required packages
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Check for locale support
include(CheckSymbolExists)
check_symbol_exists(strtod_l "stdlib.h;locale.h" HAVE_STRTOD_L)
if(NOT HAVE_STRTOD_L)
    message(STATUS "strtod_l not found, using alternative locale handling")
endif()

# Define source files
set(SOURCE_FILES
    src/mod_llm_chat.cpp
    src/ollama_client.cpp
)

# Define our main library target
add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES})

function(setup_ollama)
    find_program(OLLAMA_EXECUTABLE ollama)
    if(NOT OLLAMA_EXECUTABLE)
        if(USE_OLLAMA)
            message(WARNING "Ollama not found. Some features may be disabled.")
        endif()
        return()
    endif()
    
    # Set the model configuration
    set(OLLAMA_MODEL "yi:3b" CACHE STRING "Ollama model to use")
    set(OLLAMA_MODEL_PATH "${CMAKE_CURRENT_BINARY_DIR}/models" CACHE PATH "Path to store Ollama models")
    
    # Add custom target to pull the model during build
    add_custom_target(pull_ollama_model
        COMMAND ${OLLAMA_EXECUTABLE} pull ${OLLAMA_MODEL}
        COMMENT "Pulling Ollama model ${OLLAMA_MODEL}"
    )
    
    # Configure model settings
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/ollama_config.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/ollama_config.json
        @ONLY
    )

    # Add dependency to the main library
    add_dependencies(${PROJECT_NAME} pull_ollama_model)
endfunction()

# Call setup_ollama
setup_ollama()

# Set include directories
target_include_directories(${PROJECT_NAME}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
)

# Set compile definitions
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
    USE_OLLAMA=$<BOOL:${USE_OLLAMA}>
    $<$<BOOL:${HAVE_STRTOD_L}>:HAVE_STRTOD_L>
)

# Installation rules
install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ollama_config.json
    DESTINATION etc/${PROJECT_NAME}
) 