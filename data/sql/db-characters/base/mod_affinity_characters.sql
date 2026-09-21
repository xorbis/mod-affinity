-- mod-affinity: the affinity each character discovered (pool_id = affinity_pool.id in the world database).
CREATE TABLE IF NOT EXISTS `character_affinity` (
  `guid` INT UNSIGNED NOT NULL,
  `pool_id` INT UNSIGNED NOT NULL,
  `discovered_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
