# LLM Chat Module for AzerothCore
## Notice: This is a fork of Jake Aquilina [mod-llm-chat](https://gitlab.realsoftgames.com/krazor/mod_llm_chat)
### This fork allow mod-llm-chat to compile with the AzerothCore version of mod-playerbots only


This module enables AI chat interactions in World of Warcraft using either Ollama or LM Studio as the LLM provider.

## Requirements

### System Requirements

- AzerothCore v5.0.0+
- Boost development libraries (libboost-all-dev)
- nlohmann-json library (version 3.2.0 or higher)
- Ollama or LM Studio installed locally
- CMake 3.5+ (included with AzerothCore)

### LLM Requirements

Choose one:

- Ollama with llama3.2 model (1B for CPU, 8B for GPU)
- LM Studio with compatible model

## Installation

### 1. Install System Dependencies (Ubuntu/Debian)

Update package list:

```bash
sudo apt update
```

Install required development libraries:

```bash
sudo apt install -y \
    libboost-all-dev \
    nlohmann-json3-dev \
    build-essential \
    cmake \
    git
```

## Install the tables for the character database

The tables are required for storing long term conversation memory with playerbots and characters as well as setting up initial character traits and responses.

Navigate to the sql directory:

```bash
cd azerothcore-wotlk/modules/mod_llm_chat/data/sql/db-characters/base
```

Install the tables with command:

```sql
source character_rp_profiles.sql
```

## Configure the LLM Provider

### Option A: Ollama (Recommended)

1. Install Ollama from https://ollama.ai/
2. Start the Ollama service
3. Pull the recommended model:

```bash
ollama pull socialnetwooky/llama3.2-abliterated:1b_q8
```

To switch between models, update your `mod-llm-chat.conf` file:

- For CPU servers: `LLM.Ollama.Model = "llama3.2:1b"`
- For GPU servers: `LLM.Ollama.Model = "llama3.2:8b"`

### Option B: LM Studio

1. Download LM Studio from https://lmstudio.ai/
2. Install and configure with your preferred model
3. Enable API access in settings

## Usage

1. Log into the game
2. Type any message in chat - the AI should respond automatically

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
   sudo apt install -y libboost-all-dev nlohmann-json3-dev
   ```

3. No AI Response
   - Check module is enabled in configuration
   - Verify chat range setting
   - Check server logs for errors

## Support

- Report issues on GitHub
- Join AzerothCore Discord for help

## License

This module is released under the same license as AzerothCore.
