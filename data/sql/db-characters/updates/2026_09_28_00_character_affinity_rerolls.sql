-- mod-affinity: paid rerolls. Only for a table created before this file; a fresh install gets the
-- columns from the base file, which the updater may apply after this one, hence the guard on the
-- table existing.
SET @has_table := (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_affinity');
SET @has_column := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_affinity' AND COLUMN_NAME = 'rerolls');
SET @sql := IF(@has_table = 1 AND @has_column = 0,
    'ALTER TABLE `character_affinity` ADD COLUMN `rerolls` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''paid rerolls used'' AFTER `discovered_at`, ADD COLUMN `offer_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''pool id of the reroll waiting for take/keep, 0 = none'' AFTER `rerolls`',
    'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
