-- Tags: no-random-merge-tree-settings
-- - no-random-merge-tree-settings -- may change amount of granules

-- Test distributed index analysis with a text index when a token has non-embedded (external)
-- postings. The analyzed state carried back to the coordinator (the `extra_data` column,
-- serialized by `TextIndexAnalyzer::serializeStateBinary`) contains only token infos and
-- embedded postings. Rare (single-block) and long (multi-block) posting lists are not
-- serialized, so the reader (`MergeTreeReaderTextIndex`) must read them from disk on the
-- coordinator.
--
-- A token with cardinality in the range (MAX_CARDINALITY_FOR_EMBEDDED_POSTINGS,
-- MAX_CARDINALITY_FOR_RAW_POSTINGS] gets RawPostings + SingleBlock (not embedded), so it is
-- read by `analyzePostings` on the replica but not embedded into the serialized state. The
-- counts must match with and without distributed index analysis.

DROP TABLE IF EXISTS text_idx_ext_postings;

CREATE TABLE text_idx_ext_postings
(
    key UInt64,
    text String,
    INDEX idx(text) TYPE text(tokenizer = 'splitByNonAlpha') GRANULARITY 1
)
ENGINE = MergeTree()
ORDER BY key
SETTINGS index_granularity = 8192, index_granularity_bytes = '10Mi', min_bytes_for_wide_part = '1G',
         distributed_index_analysis_min_parts_to_activate = 0,
         distributed_index_analysis_min_indexes_bytes_to_activate = 0;

SYSTEM STOP MERGES text_idx_ext_postings;

-- Insert 10 parts x 10000 rows. 'needle' appears every 1000 rows -> 10 matches per part.
-- Cardinality 10 -> non-embedded, raw, single-block external postings.
INSERT INTO text_idx_ext_postings SELECT
    number,
    if(number % 1000 = 0, 'needle in a haystack', 'just some regular text')
FROM numbers(100000)
SETTINGS max_block_size = 10000, min_insert_block_size_rows = 10000, max_insert_threads = 1;

SET cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost';
SET max_parallel_replicas = 3;
SET allow_experimental_parallel_reading_from_replicas = 0;
SET allow_experimental_analyzer = 1;
SET use_query_condition_cache = 0;
SET distributed_index_analysis_for_non_shared_merge_tree = 1;

-- { echo }

-- Without distributed index analysis
SELECT count() FROM text_idx_ext_postings WHERE hasToken(text, 'needle') SETTINGS distributed_index_analysis = 0;

-- With distributed index analysis (the reader reads external postings from disk on the coordinator)
SELECT count() FROM text_idx_ext_postings WHERE hasToken(text, 'needle') SETTINGS distributed_index_analysis = 1;

-- { echoOff }

DROP TABLE text_idx_ext_postings;
