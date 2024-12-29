-- Create character_rp_profiles table
CREATE TABLE IF NOT EXISTS `character_rp_profiles` (
    `name` VARCHAR(12) NOT NULL,
    `profile` TEXT NOT NULL,
    PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Character RP Profiles for LLM Chat'; 