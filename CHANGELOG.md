Changelog
=========

2019-06-11 v2.0.0:

The major change in this release is a modification of output to be compatible
with standard PostgreSQL logs format. This can break compatibility.

  - Fix statement duration that was only the time passed in the executor (Gilles Darold)
  - Make output fully compatible with standard PostgreSQL logs format (Gilles Darold)
  - Move queryId as a comment before the statement to make logs compatible
    with pgBadger (Gilles Darold)
  - Force write of all ProcessUtility statement (include DDL, `SET`...) when
    `pg_sampletolog.statement_sample_rate` is set to 1.0 as this mean that
    every statement should be written just like
    `pg_sampletolog.log_statement = 'all'` (Gilles Darold)

2019-04-21 v1.0.0:

  - No change, this extension can be considered as stable

2019-02-10 v0.0.2:

  - Change formating
  - Fix missing utility statements
  - Add debian packaging

2019-01-27 v0.0.1:

  - First release
