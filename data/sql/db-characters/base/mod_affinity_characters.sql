-- mod-affinity: the affinity each character discovered (pool_id = affinity_pool.id in the world database).
CREATE TABLE IF NOT EXISTS `character_affinity` (
  `guid` INT UNSIGNED NOT NULL,
  `pool_id` INT UNSIGNED NOT NULL,
  `discovered_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  `rerolls` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'paid rerolls used',
  `offer_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'pool id of the reroll waiting for take/keep, 0 = none',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
