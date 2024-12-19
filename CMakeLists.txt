# Basic module information
set(MOD_LLM_CHAT "mod-llm-chat")

# Check for package manager (apt for Ubuntu/Debian)
find_program(APT_GET_EXECUTABLE apt-get)
find_program(SYSTEMCTL_EXECUTABLE systemctl)
find_program(OLLAMA_EXECUTABLE ollama)

# Function to install Ollama and setup the service
function(setup_ollama)
    if(NOT OLLAMA_EXECUTABLE)
        message(STATUS "Installing Ollama...")
        execute_process(
            COMMAND curl https://ollama.ai/install.sh -sSL | sh
            RESULT_VARIABLE OLLAMA_INSTALL_RESULT
        )
        if(NOT OLLAMA_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to install Ollama")
        endif()
    endif()

    if(SYSTEMCTL_EXECUTABLE)
        # Enable and start Ollama service
        execute_process(
            COMMAND sudo systemctl enable ollama
            COMMAND sudo systemctl start ollama
            RESULT_VARIABLE SERVICE_RESULT
        )
        if(NOT SERVICE_RESULT EQUAL 0)
            message(WARNING "Failed to setup Ollama service. You may need to start it manually.")
        endif()

        # Pull the CPU model (1.7B)
        execute_process(
            COMMAND ollama pull llama3.2:1.7b
            RESULT_VARIABLE MODEL_RESULT
        )
        if(NOT MODEL_RESULT EQUAL 0)
            message(WARNING "Failed to pull llama3.2:1.7b model. You may need to pull it manually using: ollama pull llama3.2:1.7b")
        endif()
    else()
        message(WARNING "systemctl not found. Please start Ollama manually and pull the model using: ollama pull llama3.2:1.7b")
    endif()
endfunction()

# Function to install system dependencies
function(install_system_dependencies)
    if(APT_GET_EXECUTABLE)
        message(STATUS "Installing required system dependencies...")
        execute_process(
            COMMAND sudo apt-get update
            COMMAND sudo apt-get install -y libcurl4-openssl-dev nlohmann-json3-dev curl
            RESULT_VARIABLE INSTALL_RESULT
        )
        if(NOT INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to install system dependencies")
        endif()
    else()
        message(WARNING "Automatic dependency installation is only supported on Ubuntu/Debian.\nPlease install libcurl4-openssl-dev and nlohmann-json3-dev manually.")
    endif()
endfunction()

# Check for required packages
find_package(CURL QUIET)
find_package(nlohmann_json 3.2.0 QUIET)

# Install dependencies if not found
if(NOT CURL_FOUND OR NOT TARGET nlohmann_json::nlohmann_json)
    message(STATUS "Missing required dependencies. Attempting to install...")
    install_system_dependencies()
    
    # Recheck for packages
    find_package(CURL REQUIRED)
    find_package(nlohmann_json 3.2.0 REQUIRED)
endif()

# Setup Ollama if not installed
if(NOT OLLAMA_EXECUTABLE)
    message(STATUS "Setting up Ollama...")
    setup_ollama()
endif()

# Verify dependencies are now available
if(NOT CURL_FOUND)
    message(FATAL_ERROR "Required library libcurl not found! Please install it manually.")
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "Required library nlohmann-json not found! Please install it manually.")
endif()

# Add the module
AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/mod_llm_chat.cpp")
AC_ADD_SCRIPT_LOADER("LLMChat" "${CMAKE_CURRENT_LIST_DIR}/src/loader.h")

# Link libraries
target_link_libraries(scripts 
    PRIVATE 
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# Install configuration
install(FILES "${CMAKE_CURRENT_LIST_DIR}/conf/mod_llm_chat.conf.dist" DESTINATION ${CONF_DIR})

# Print setup message
message(STATUS "
LLM Chat Module configuration complete:
- Dependencies installed
- Ollama service configured
- CPU model (llama3.2:1.7b) downloaded
- Configuration file installed

If you encounter any issues:
1. Check Ollama service: systemctl status ollama
2. Verify model: ollama list
3. Manual model install if needed: ollama pull llama3.2:1.7b
") 