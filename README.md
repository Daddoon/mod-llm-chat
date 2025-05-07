# LLM Chat Module for AzerothCore
## Notice: This is a fork of Jake Aquilina [mod-llm-chat](https://gitlab.realsoftgames.com/krazor/mod_llm_chat)
### - This fork allow mod-llm-chat to compile with the AzerothCore version of mod-playerbots only
### - The purpose of this fork is purely for experimentation and is not intented to be maintained

This module enables AI chat interactions in World of Warcraft using either Ollama or LM Studio as the LLM provider.

## Pre-built Binaries

- See in Release section !
- This is just given for convenience and only for Windows (x64).
- Check the **mod-llm-chat.conf** file in accordance to your **ollama configuration**
- AC Data files of your WOTLK client must be placed in a **data** dir at the root of your server binary (for this release)
	- English: [AC Data (en-US)](https://github.com/wowgaming/client-data/releases)
	- French:  [AC Data (fr-FR)](https://github.com/Daddoon/ac-client-data-fr/releases)
	- Other localizations: You should extract theses data by yourself. Read AzerothCore manual
 - Obviously, MySQL requirements and else should also be met before starting your server, like in the AzerothCore installation manual

## Requirements

- Current code has been tested against liyunfan1223 AzerothCore Server from this [commit](https://github.com/liyunfan1223/azerothcore-wotlk/tree/ce9343d9167acb27fbfc8ef1203f0077034d07de)

### System Requirements

- AzerothCore v5.0.0+
- AzerothCore [build requirements](https://www.azerothcore.org/wiki/requirements)
- nlohmann-json library (version 3.2.0 or higher)
- Ollama or LM Studio installed locally
- CMake 3.5+ (included with AzerothCore)

### LLM Requirements

Choose one:

- Ollama with llama3.2 model (1B for CPU, 8B for GPU)
- LM Studio with compatible model

## Installation

### 1. Installation (AzerothCore + mod-llm-chat) - Ubuntu/Debian/Windows

1.1 See AzerothCore [Requirement](https://www.azerothcore.org/wiki/requirements) manual
1.2 See AzerothCore [Installation](https://www.azerothcore.org/wiki/core-installation) manual but **INSTEAD** clone from liyunfan1223 repository for Playerbots support, has **mod-llm-chat** is based on Playerbots.

```bash
git clone https://github.com/liyunfan1223/azerothcore-wotlk.git --branch=Playerbot
```

**(OPTIONAL)** If you are having issue afterward, on the last AzerothCore version from Playerbot, you may test on this commit, from your base project directory:

```bash
git reset --hard ce9343d
```

Then:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/liyunfan1223/mod-playerbots.git --branch=master
git clone https://github.com/Daddoon/mod-llm-chat.git --branch=main
```

- 1.3 Install nlohmann-json library (version 3.2.0 or higher)
- 1.3.1 On Ubuntu/Debian

  ```bash
      sudo apt install -y nlohmann-json3-dev
   ```
- 1.3.2 On Windows
  1. Go through the steps from **AzerothCore - Windows Core** installation manual but stop after the first step of the **Compiling the Source** section
  2. Open your Visual Studio **AzerothCore.sln** project you should have generated from **CMake** from **AzerothCore Installation manual*
  3. Right click on the **modules** project in your **Solution Explorer**
  4. Click Manage NuGet Package
  5. Search for **nlohmann.json**
  6. Click Install

1.4 Continue installation all AzerothCore installation steps needed from the official manual


## Install the tables for the character database

The tables are required for storing long term conversation memory with playerbots and characters as well as setting up initial character traits and responses.

(Unsure if automatic or not)
Navigate to the sql directory:

```bash
cd azerothcore-wotlk/modules/mod_llm_chat/data/sql
```

Install the tables with command, or copy past with your favorite SQL tool in your DB:

```sql
source db_llm_chat.sql
```

Navigate to the sql directory:

```bash
db-characters/base
```

(TODO: Need to test if this is required on the llmdb OR acore database, as USE is not specified)
Install the tables with command, or copy past with your favorite SQL tool in your DB:

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

- Do not expect deep support from me as I have serious chronical disease. This is just for fun and some support.
- But feel free to propose PR here or on the original repository of Jake Aquilina [mod-llm-chat](https://gitlab.realsoftgames.com/krazor/mod_llm_chat) based on GitLab.

## License

This module is released under the same license as AzerothCore.
