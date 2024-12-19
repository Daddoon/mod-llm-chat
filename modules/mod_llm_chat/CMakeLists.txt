# Instead of failing immediately, you can make Ollama optional
option(USE_OLLAMA "Enable Ollama integration" ON)

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
endfunction() 