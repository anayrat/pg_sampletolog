# pg_sampletolog

pg_sampletolog is a PostgreSQL extension which allows to sample statements and/or transactions to logs. It adds in PostgreSQL (from 9.4 to 11) same kind of statement sampling added in PostgreSQL 12 (currently not released).

pg_sampletolog allows to:

  * Log a sample of statements
  * Log a sample of transactions
  * Log before or after execution (in order to be compatible with pgreplay)
  * Log all `DDL` or `MOD` statements, same as [log_statement](https://www.postgresql.org/docs/current/runtime-config-logging.html#GUC-LOG-STATEMENT)
  * Log statement's queryid if [pg_stat_statements](https://www.postgresql.org/docs/current/pgstatstatements.html) is installed



## Installation

### Compiling

The module can be built using the standard PGXS infrastructure. For this to work, the `pg_config` program must be available in your $PATH. Instruction to install follows:

```
git clone https://github.com/anayrat/pg_sampletolog.git
cd pg_sampletolog
make
make install
```

### Postgres setup

Extension must me loaded either:

  * In local session with `LOAD 'pg_sampletolog';` order.
  * In `session_preload_libraries`. To be loaded at connection start.

New settings should be visible in `pg_settings` view:

```
select name from pg_settings where name like 'pg_sampletolog%';
                 name                 
----------------------------------------
 pg_sampletolog.disable_log_duration
 pg_sampletolog.log_before_execution
 pg_sampletolog.log_level
 pg_sampletolog.log_statement
 pg_sampletolog.statement_sample_rate
 pg_sampletolog.transaction_sample_rate
(6 rows)
```


## Usage

pg_sampletolog is controlled by specifing GUC settings. So you can apply them in your `postgresql.conf` or change them inside the session with `SET` order.

By default, pg_sampletolog is disabled, you have to change theses settings (default values in parenthesis):

  * *pg_sampletolog.log_level* (`LOG`): Control log severity level. Refer to PostgreSQL documentation for their meaning : [Message Severity Levels](https://www.postgresql.org/docs/current/runtime-config-logging.html#RUNTIME-CONFIG-SEVERITY-LEVELS)
  * *pg_sampletolog.statement_sample_rate* (`0`): Control fraction of statements to report in log.
  * *pg_sampletolog.transaction_sample_rate* (`0`): Control fraction of transactions to report in log.
  * *pg_sampletolog.log_statement* `none`): Allow to log *all* DDL and/or MOD statements.
  * *pg_sampletolog.log_before_execution* (`false`): Control to log statements before or after execution.
  * *pg_sampletolog.disable_log_duration* (`false`): Disable duration reporting (mainly used for tests).


**Examples:**

  * Log only 10% of statements: `pg_sampletolog.statement_sample_rate = 0.1`

pg_sampelog will log arround 10% of statements. It will do computation for each statement by calling `random()` function whose cost is negligible. Message output will look like (depending of your `log_line_prefix`) :
```
2019-01-27 12:50:39.361 CET [27047] LOG:  duration: 0.014 ms  statement: SELECT 1;
```

  * Log only 10% of transactions: `pg_sampletolog.transaction_sample_rate = 0.1`

Same as previous, with the difference that, when a transaction starts, pg_sampletolog will choose if the transaction will be logged. When a transaction is logged, **all** statements of the transaction are logged.

Message output will look like (depending of your `log_line_prefix`) :
```
Transaction : BEGIN; SELECT 1; SELECT 1; COMMIT;

2019-01-27 12:51:40.562 CET [27069] LOG:  duration: 0.008 ms  statement: SELECT 1;
2019-01-27 12:51:40.562 CET [27069] LOG:  duration: 0.005 ms  statement: SELECT 1;
```

  * Log all DDL statements: `pg_sampletolog.log_statement = 'ddl'`:

pg_sampletolog will log **all** DDL statements, it is similar to `log_statement = ddl` parameter in Postgres. It could be useful if you want a sample of read queries but catch all ddl changes.

```
2019-01-27 12:53:47.564 CET [27103] LOG:  statement: CREATE TABLE t1(c1 int);
```

  * Log all data-modifying statements: `pg_sampletolog.log_statement = 'mod'`:

pg_sampletolog will log **all** data-modifying statements (including ddl), it is similar to `log_statement = mod` parameter in Postgres. It could be useful if you want a sample of read queries but catch all write changes.

```
2019-01-27 12:59:54.043 CET [27160] LOG:  duration: 0.246 ms  statement: INSERT INTO t1 VALUES(1);
2019-01-27 13:00:16.468 CET [27160] LOG:  duration: 126.851 ms  statement: CREATE INDEX ON t1(c1);
```

  * Log before execution: `pg_sampletolog.log_before_execution = on`

By default, pg_sampletolog, will log after query is executed. It could be useful if you want to use results with pgreplay.


pg_sampletolog works on primary, but also on standby. If you call an SQL function containing several queries, pg_sampletolog will only log function call.


With a simple test with pgbench and `log_min_duration = 0`:
```
pgbench -c8 -j2 -t 30000 -P5 bench
starting vacuum...end.
progress: 5.0 s, 10618.2 tps, lat 0.750 ms stddev 0.170
progress: 10.0 s, 10690.9 tps, lat 0.746 ms stddev 0.168
progress: 15.0 s, 10596.4 tps, lat 0.752 ms stddev 0.149
progress: 20.0 s, 9547.6 tps, lat 0.836 ms stddev 0.176
transaction type: <builtin: TPC-B (sort of)>
scaling factor: 100
query mode: simple
number of clients: 8
number of threads: 2
number of transactions per client: 30000
number of transactions actually processed: 240000/240000
latency average = 0.774 ms
latency stddev = 0.175 ms
tps = 9836.617690 (including connections establishing)
tps = 9837.334350 (excluding connections establishing)
pgbench -c8 -j2 -t 30000 -P5 bench  6.96s user 12.32s system 78% cpu 24.448 total


wc -l logfile
1680009
```

Same test with `pg_sampletolog.statement_sample_rate = 0.1`
```
wc -l logfile
119859
```

We got less than 10% lines of first test because pg_sampletolog don't log `BEGIN` or `COMMIT` orders.


Bonus if you have `pg_stat_statements` installed, pg_sampletolog will report queryid:
```
LOG:  duration: 0.032 ms  statement: /* queryid = 5892081150081336634 */ UPDATE pgbench_tellers SET tbalance = tbalance + -3337 WHERE tid = 326;
```

It is useful to get query's parameters from a query you identified in pg_stat_statements' view.

## Limitations

*No supported*:

  * PREPARE statements are not supported.

## Testing

Tests are in `sql/pg_sampletolog.sql` and results in `expected/pg_sampletolog.out`.
Use `make installcheck` to run them, you should see:

```
/tmp/pgbuild/lib/postgresql/pgxs/src/makefiles/../../src/test/regress/pg_regress --inputdir=./ --bindir='/tmp/pgbuild/bin'    --dbname=contrib_regression pg_sampletolog
(using postmaster on Unix socket, port 5432)
============== dropping database "contrib_regression" ==============
DROP DATABASE
============== creating database "contrib_regression" ==============
CREATE DATABASE
ALTER DATABASE
============== running regression test queries        ==============
test pg_sampletolog               ... ok

=====================
 All 1 tests passed. 
=====================
```

## TODO

  * Log statements by their queryid

## Author

Adrien NAYRAT

## Licence

pg_sampletolog is free software distributed under the PostgreSQL license.

Copyright (c) 2019, Adrien NAYRAT
