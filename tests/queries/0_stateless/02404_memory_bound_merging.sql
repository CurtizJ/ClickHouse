-- Tags: no-parallel, no-random-merge-tree-settings

drop table if exists pr_t;
drop table if exists dist_t_different_dbs;
drop table if exists shard_1.t_different_dbs;
drop table if exists t_different_dbs;
drop table if exists dist_t;
drop table if exists t;


create table t(a UInt64, b UInt64) engine=MergeTree order by a;
system stop merges t;
insert into t select number, number from numbers_mt(1e6);

set enable_memory_bound_merging_of_aggregation_results = 1;
set max_threads = 4;
set optimize_aggregation_in_order = 1;
set optimize_read_in_order = 1;
set prefer_localhost_replica = 1;

-- slightly different transforms will be generated by reading steps if we let settings randomisation to change this setting value --
set read_in_order_two_level_merge_threshold = 1000;

create table dist_t as t engine = Distributed(test_cluster_two_shards, currentDatabase(), t, a % 2);

-- { echoOn } --
explain pipeline select a from remote(test_cluster_two_shards, currentDatabase(), t) group by a;

select a from remote(test_cluster_two_shards, currentDatabase(), t) group by a order by a limit 5 offset 100500;

explain pipeline select a from remote(test_cluster_two_shards, currentDatabase(), dist_t) group by a;

select a from remote(test_cluster_two_shards, currentDatabase(), dist_t) group by a order by a limit 5 offset 100500;

-- { echoOff } --

set aggregation_in_order_max_block_bytes = '1Mi';
set max_block_size = 500;
-- actual block size might be slightly bigger than the limit --
select max(bs) < 70000 from (select avg(a), max(blockSize()) as bs from remote(test_cluster_two_shards, currentDatabase(), t) group by a);

-- beautiful case when we have different sorting key definitions in tables involved in distributed query => different plans => different sorting properties of local aggregation results --
create database if not exists shard_1;
create table t_different_dbs(a UInt64, b UInt64) engine = MergeTree order by a;
create table shard_1.t_different_dbs(a UInt64, b UInt64) engine = MergeTree order by tuple();

insert into t_different_dbs select number % 1000, number % 1000 from numbers_mt(1e6);
insert into shard_1.t_different_dbs select number % 1000, number % 1000 from numbers_mt(1e6);

create table dist_t_different_dbs as t engine = Distributed(test_cluster_two_shards_different_databases_with_local, '', t_different_dbs);

-- { echoOn } --
explain pipeline select a, count() from dist_t_different_dbs group by a order by a limit 5 offset 500;

select a, count() from dist_t_different_dbs group by a order by a limit 5 offset 500;
select a, count() from dist_t_different_dbs group by a, b order by a limit 5 offset 500;

-- { echoOff } --

drop table if exists pr_t;

create table pr_t(a UInt64, b UInt64) engine=MergeTree order by a settings storage_policy = 's3';
insert into pr_t select number % 1000, number % 1000 from numbers_mt(1e6);

set allow_experimental_parallel_reading_from_replicas = 1;
set parallel_replicas_for_non_replicated_merge_tree = 1;
set max_parallel_replicas = 3;
set use_hedged_requests = 0;
set cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost';
set distributed_aggregation_memory_efficient=1;

select count() from pr_t;

-- { echoOn } --
explain pipeline select a from pr_t group by a order by a limit 5 offset 500;

select a, count() from pr_t group by a order by a limit 5 offset 500;
select a, count() from pr_t group by a, b order by a limit 5 offset 500;

-- { echoOff } --

drop table if exists pr_t;
drop table if exists dist_t_different_dbs;
drop table if exists shard_1.t_different_dbs;
drop table if exists t_different_dbs;
drop table if exists dist_t;
drop table if exists t;
