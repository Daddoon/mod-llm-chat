-- Create the LLMDB database if it doesn't exist
CREATE DATABASE IF NOT EXISTS `LLMDB`;

USE `LLMDB`;

-- Create tables for LLM Chat module
CREATE TABLE IF NOT EXISTS `llm_chat_personalities` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `name` VARCHAR(255) NOT NULL,
    `description` TEXT,
    `traits` TEXT,
    `emotions` TEXT,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `llm_chat_conversations` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `sender_guid` BIGINT UNSIGNED NOT NULL,
    `responder_guid` BIGINT UNSIGNED NOT NULL,
    `message` TEXT NOT NULL,
    `response` TEXT NOT NULL,
    `chat_type` TINYINT UNSIGNED NOT NULL,
    `timestamp` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `sender_guid` (`sender_guid`),
    KEY `responder_guid` (`responder_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `llm_chat_memories` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `sender_guid` BIGINT UNSIGNED NOT NULL,
    `responder_guid` BIGINT UNSIGNED NOT NULL,
    `memory_type` VARCHAR(50) NOT NULL,
    `content` TEXT NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    `expires_at` TIMESTAMP NULL DEFAULT NULL,
    PRIMARY KEY (`id`),
    KEY `sender_guid` (`sender_guid`),
    KEY `responder_guid` (`responder_guid`),
    KEY `memory_type` (`memory_type`),
    KEY `expires_at` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `llm_chat_relationships` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `character_guid` BIGINT UNSIGNED NOT NULL,
    `related_guid` BIGINT UNSIGNED NOT NULL,
    `relationship_type` VARCHAR(50) NOT NULL,
    `strength` TINYINT NOT NULL DEFAULT 0,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `unique_relationship` (`character_guid`, `related_guid`),
    KEY `character_guid` (`character_guid`),
    KEY `related_guid` (`related_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `llm_chat_emotions` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `character_guid` BIGINT UNSIGNED NOT NULL,
    `emotion` VARCHAR(50) NOT NULL,
    `intensity` TINYINT NOT NULL DEFAULT 0,
    `trigger` VARCHAR(255),
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `character_guid` (`character_guid`),
    KEY `emotion` (`emotion`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Grant necessary permissions
GRANT SELECT, INSERT, UPDATE, DELETE ON `LLMDB`.* TO 'acore'@'localhost';
FLUSH PRIVILEGES; 