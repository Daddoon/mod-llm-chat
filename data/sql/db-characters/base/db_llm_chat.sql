/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

-- Drop previous tables if they exist
DROP TABLE IF EXISTS `bot_llmchat_personalities`;
CREATE TABLE `bot_llmchat_personalities` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'Character GUID',
  `trait_key` VARCHAR(50) NOT NULL COMMENT 'Personality trait identifier',
  `trait_value` TEXT NOT NULL COMMENT 'Value or description of the trait',
  `last_updated` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last modification timestamp',
  PRIMARY KEY (`guid`, `trait_key`),
  KEY `idx_trait_key` (`trait_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Stores bot personality traits and values';

DROP TABLE IF EXISTS `bot_llmchat_backstories`;
CREATE TABLE `bot_llmchat_backstories` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'Character GUID',
  `backstory` TEXT NOT NULL COMMENT 'Character backstory narrative',
  `last_updated` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last modification timestamp',
  `version` INT UNSIGNED DEFAULT 1 COMMENT 'Backstory version number',
  PRIMARY KEY (`guid`),
  KEY `idx_version` (`version`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Stores bot character backstories';

DROP TABLE IF EXISTS `bot_llmchat_conversations`;
CREATE TABLE `bot_llmchat_conversations` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'Unique conversation ID',
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'Bot character GUID',
  `player_guid` INT UNSIGNED NOT NULL COMMENT 'Player character GUID',
  `conversation` TEXT NOT NULL COMMENT 'Conversation content',
  `timestamp` TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT 'When the conversation occurred',
  `context_tags` VARCHAR(255) DEFAULT NULL COMMENT 'Searchable context tags',
  `location` INT UNSIGNED DEFAULT NULL COMMENT 'Map ID where conversation occurred',
  `emotional_context` VARCHAR(50) DEFAULT NULL COMMENT 'Emotional state during conversation',
  PRIMARY KEY (`id`),
  KEY `idx_participants` (`bot_guid`, `player_guid`),
  KEY `idx_timestamp` (`timestamp`),
  KEY `idx_location` (`location`),
  KEY `idx_context` (`emotional_context`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Stores conversation history between bots and players';

DROP TABLE IF EXISTS `bot_llmchat_emotions`;
CREATE TABLE `bot_llmchat_emotions` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'Character GUID',
  `emotion` VARCHAR(50) NOT NULL COMMENT 'Current emotion',
  `intensity` TINYINT UNSIGNED NOT NULL COMMENT 'Emotion intensity (0-10)',
  `timestamp` TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT 'When the emotion was set',
  `trigger_event` VARCHAR(255) DEFAULT NULL COMMENT 'What triggered this emotion',
  PRIMARY KEY (`guid`),
  KEY `idx_emotion` (`emotion`),
  KEY `idx_intensity` (`intensity`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Tracks current emotional states of bots';

DROP TABLE IF EXISTS `bot_llmchat_relationships`;
CREATE TABLE `bot_llmchat_relationships` (
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'Bot character GUID',
  `target_guid` INT UNSIGNED NOT NULL COMMENT 'Target character GUID',
  `standing` INT NOT NULL DEFAULT 0 COMMENT 'Relationship standing (-100 to 100)',
  `last_interaction` TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT 'Last interaction time',
  `interaction_count` INT UNSIGNED DEFAULT 0 COMMENT 'Number of interactions',
  `relationship_type` VARCHAR(50) DEFAULT 'NEUTRAL' COMMENT 'Type of relationship',
  `notes` TEXT DEFAULT NULL COMMENT 'Additional relationship context',
  PRIMARY KEY (`bot_guid`, `target_guid`),
  KEY `idx_standing` (`standing`),
  KEY `idx_last_interaction` (`last_interaction`),
  KEY `idx_relationship_type` (`relationship_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Tracks relationships between characters';

DROP TABLE IF EXISTS `bot_llmchat_memories`;
CREATE TABLE `bot_llmchat_memories` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'Unique event ID',
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'Bot character GUID',
  `event_type` VARCHAR(50) NOT NULL COMMENT 'Type of memory event',
  `event_description` TEXT NOT NULL COMMENT 'Detailed event description',
  `timestamp` TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT 'When the event occurred',
  `importance` TINYINT UNSIGNED DEFAULT 1 COMMENT 'Event importance (1-10)',
  `related_guids` VARCHAR(255) DEFAULT NULL COMMENT 'Related character GUIDs',
  `location` INT UNSIGNED DEFAULT NULL COMMENT 'Map ID where event occurred',
  `expiry_date` TIMESTAMP NULL DEFAULT NULL COMMENT 'When this memory should fade',
  PRIMARY KEY (`id`),
  KEY `idx_bot_guid` (`bot_guid`),
  KEY `idx_importance` (`importance`),
  KEY `idx_event_type` (`event_type`),
  KEY `idx_location` (`location`),
  KEY `idx_expiry` (`expiry_date`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Stores significant events in bot memory';

DROP TABLE IF EXISTS `bot_llmchat_triggers`;
CREATE TABLE `bot_llmchat_triggers` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'Unique trigger ID',
  `trigger_type` VARCHAR(50) NOT NULL COMMENT 'Type of trigger',
  `trigger_value` VARCHAR(255) NOT NULL COMMENT 'Value that triggers the interaction',
  `response_type` VARCHAR(50) NOT NULL COMMENT 'Type of response',
  `emotion_modifier` TINYINT DEFAULT 0 COMMENT 'How this affects emotions (-10 to 10)',
  `standing_modifier` TINYINT DEFAULT 0 COMMENT 'How this affects standing (-10 to 10)',
  PRIMARY KEY (`id`),
  KEY `idx_trigger_type` (`trigger_type`),
  KEY `idx_trigger_value` (`trigger_value`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Defines triggers for bot interactions'; 