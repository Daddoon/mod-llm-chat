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
    # Rest of your Ollama setup...
endfunction() 