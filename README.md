# LLM Chat Module for AzerothCore

This module enables AI chat interactions in World of Warcraft using either Ollama or LM Studio as the LLM provider.

## Requirements

### System Requirements

-   AzerothCore v5.0.0+
-   libcurl development libraries
-   nlohmann-json library (version 3.2.0 or higher)
-   Ollama or LM Studio installed locally
-   CMake 3.5+ (included with AzerothCore)

### LLM Requirements

Choose one:

-   Ollama with llama3.2 model (3B for CPU, 8B for GPU)
-   LM Studio with compatible model

## Installation

### 1. Install System Dependencies (Ubuntu/Debian)

Update package list
sudo apt update
Install required development libraries
sudo apt install -y \
libcurl4-openssl-dev \
nlohmann-json3-dev \
build-essential \
cmake \
git

### 2. Install LLM Provider

#### Option A: Ollama (Recommended)

Install Ollama
curl https://ollama.ai/install.sh | sh
Start Ollama service
sudo systemctl start ollama
Pull the Llama2 model
ollama pull llama3.2:3b
Verify installation
ollama list

To switch between models, update your `mod_llm_chat.conf` file:

-   For CPU servers: `LLM.Ollama.Model = "llama3.2:3b"`
-   For GPU servers: `LLM.Ollama.Model = "llama3.2:8b"`

#### Option B: LM Studio

1. Download LM Studio from https://lmstudio.ai/
2. Install and configure with your preferred model
3. Enable API access in settings

4. Log into the game
5. Type any message in chat - the AI should respond automatically

## Troubleshooting

### Common Issues

1. "Error communicating with LLM service"

    - Verify Ollama/LM Studio is running

    ```bash
    sudo systemctl status ollama
    sudo systemctl restart ollama  # if needed
    ```

    - Check endpoints in configuration
    - Verify model is properly installed

2. "Required library not found"

    ```bash
    # Reinstall dependencies
    sudo apt install -y libcurl4-openssl-dev nlohmann-json3-dev
    ```

3. No AI Response
    - Check module is enabled in configuration
    - Verify chat range setting
    - Check server logs for errors

### Logs

Check the following for error messages:

-   WorldServer console output
-   Server logs in `/var/log/azerothcore/` or your configured log directory

## Support

-   Report issues on GitHub
-   Join AzerothCore Discord for help

## License

This module is released under the same license as AzerothCore.
