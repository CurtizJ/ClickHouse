set send_logs_level = 'none';

drop table if exists test.ttl;

create table test.ttl (d Date, a Int) engine = MergeTree order by a partition by toDayOfMonth(d);
alter table test.ttl modify ttl d + interval 1 day;
show create table test.ttl;
insert into test.ttl values (toDateTime('2000-10-10 00:00:00'), 2);
insert into test.ttl values (toDateTime('2100-10-10 00:00:00'), 3);
optimize table test.ttl final;
select * from test.ttl order by d;

alter table test.ttl modify ttl a; -- { serverError 446}

drop table if exists test.ttl;

create table test.ttl (d Date, a Int) engine = MergeTree order by tuple() partition by toDayOfMonth(d);
alter table test.ttl modify column a Int ttl d + interval 1 day;
desc table test.ttl;
alter table test.ttl modify column d Int ttl d + interval 1 day; -- { serverError 44}

