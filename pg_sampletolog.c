/*-------------------------------------------------------------------------
 *
 * pg_sampletolog is a PostgreSQL extension which allows to sample
 * statements and/or transactions to logs.
 *
 * Portions of code inspired by auto_explain contrib.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (c) 2019, Adrien Nayrat adrien.nayrat@gmail.com
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/executor.h"
#include "storage/proc.h"
#include "access/xact.h"

#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#if PG_VERSION_NUM >= 90600
#include "access/parallel.h"
#endif

/* To log statement duration */
#include <utils/timestamp.h>
#include <access/xact.h>

PG_MODULE_MAGIC;

/* GUC variables */
static double pgsl_stmt_sample_rate = 0;
static double pgsl_transaction_sample_rate = 0;
static int	pgsl_stmt_sample_limit = -1;
static int	pgsl_log_level = LOG;
static int	pgsl_log_statement = LOGSTMT_NONE;
static bool pgsl_log_before_execution = false;
static bool pgsl_disable_log_duration = false;


static const struct config_enum_entry loglevel_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"log", LOG, false},
	{NULL, 0, false}
};

static const struct config_enum_entry logstatement_options[] = {
	{"none", LOGSTMT_NONE, false},
	{"ddl", LOGSTMT_DDL, false},
	{"mod", LOGSTMT_MOD, false},
	{"all", LOGSTMT_ALL, false},
	{NULL, 0, false}
};

struct Duration
{
	long		secs;
	int			usecs;
	int			msecs;
};

/* Current nesting depth of ExecutorRun calls */
static int	pgsl_nesting_level = 0;


/* Saved hook values in case of unload */
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Is the current query sampled, per backend */
static bool pgsl_query_issampled = false;

/* Is the current transaction sampled */
static bool pgsl_transaction_issampled = false;

/*
 * Store previous LocalTransactionXid: > src/include/storage/lock.h: >
 * Top-level transactions are identified by VirtualTransactionIDs comprising
 * > the BackendId of the backend running the xact, plus a locally-assigned >
 * LocalTransactionId.
 *
 * It is used to detect a new transaction (standby take VirtualTransaction)
 */
static TransactionId pgsl_previouslxid = 0;


/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static void
			pgsl_ProcessUtility(
#if PG_VERSION_NUM >= 100000
								PlannedStmt *pstmt,
#else
								Node *parsetree,
#endif
								const char *queryString,
								ProcessUtilityContext context,
								ParamListInfo params,
#if PG_VERSION_NUM >= 100000
								QueryEnvironment *queryEnv,
#endif
								DestReceiver *dest,
								char *completionTag);
static void pgsl_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void
			pgsl_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
#if PG_VERSION_NUM >= 90600
							 uint64 count
#else
							 long count
#endif
#if PG_VERSION_NUM >= 100000
							 ,bool execute_once
#endif
);
static void pgsl_ExecutorFinish(QueryDesc *queryDesc);
static void pgsl_ExecutorEnd(QueryDesc *queryDesc);

static void pgsl_log_report(QueryDesc *queryDesc);
static void pgsl_check_transaction_issampled(void);
static bool pgsl_stmt_limit(void);
static struct Duration pgsl_get_duration(void);
static char *pgsl_get_duration_str(void);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables. */
	DefineCustomRealVariable("pg_sampletolog.statement_sample_rate",
							 "Fraction of queries to log.",
							 "Use a value between 0.0 (never log) and 1.0 (always log).",
							 &pgsl_stmt_sample_rate,
							 0.0,
							 0.0,
							 1.0,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("pg_sampletolog.transaction_sample_rate",
							 "Fraction of transactions to log.",
							 "Use a value between 0.0 (never log) and 1.0 (always log).",
							 &pgsl_transaction_sample_rate,
							 0.0,
							 0.0,
							 1.0,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_sampletolog.statement_sample_limit",
							"Always log queries exceeding statement_sample_limit.",
							"Useful to disable sampling for long queriesi.",
							&pgsl_stmt_sample_limit,
							-1,
							-1,
							INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_sampletolog.log_level",
							 "Log level for sampled queries.",
							 NULL,
							 &pgsl_log_level,
							 LOG,
							 loglevel_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("pg_sampletolog.log_statement",
							 "Log all statements of this type.",
							 "Only mod and ddl have effect.",
							 &pgsl_log_statement,
							 LOGSTMT_NONE,
							 logstatement_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_sampletolog.log_before_execution",
							 "Log statement before execution.",
							 NULL,
							 &pgsl_log_before_execution,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_sampletolog.disable_log_duration",
							 "Disable duration in log, used for testing.",
							 NULL,
							 &pgsl_disable_log_duration,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);


	EmitWarningsOnPlaceholders("pg_sampletolog");

	/* Install hooks only on leader. */
#if PG_VERSION_NUM >= 90600
	if (!IsParallelWorker())
	{
#endif
		prev_ProcessUtility = ProcessUtility_hook;
		ProcessUtility_hook = pgsl_ProcessUtility;
		prev_ExecutorStart = ExecutorStart_hook;
		ExecutorStart_hook = pgsl_ExecutorStart;
		prev_ExecutorRun = ExecutorRun_hook;
		ExecutorRun_hook = pgsl_ExecutorRun;
		prev_ExecutorFinish = ExecutorFinish_hook;
		ExecutorFinish_hook = pgsl_ExecutorFinish;
		prev_ExecutorEnd = ExecutorEnd_hook;
		ExecutorEnd_hook = pgsl_ExecutorEnd;
#if PG_VERSION_NUM >= 90600
	}
#endif
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks only on leader. */
#if PG_VERSION_NUM >= 90600
	if (!IsParallelWorker())
	{
#endif
		ProcessUtility_hook = prev_ProcessUtility;
		ExecutorStart_hook = prev_ExecutorStart;
		ExecutorRun_hook = prev_ExecutorRun;
		ExecutorFinish_hook = prev_ExecutorFinish;
		ExecutorEnd_hook = prev_ExecutorEnd;
#if PG_VERSION_NUM >= 90600
	}
#endif
}


static struct Duration
pgsl_get_duration(void)
{
	struct Duration d;

	d.secs = 0;
	d.usecs = 0;
	d.msecs = 0;

	TimestampDifference(GetCurrentStatementStartTimestamp(), GetCurrentTimestamp(), &d.secs, &d.usecs);
	d.msecs = d.usecs / 1000;

	return d;
}

static char *
pgsl_get_duration_str(void)
{
	char	   *duration_str = NULL;
	struct Duration d;

	duration_str = palloc0(sizeof(char) * 32);


	if (!pgsl_disable_log_duration)
	{
		d = pgsl_get_duration();
		snprintf(duration_str, 30, "duration: %ld.%03d ms  ", d.secs * 1000 + d.msecs, d.usecs % 1000);
	}
	return duration_str;

}

void
pgsl_log_report(QueryDesc *queryDesc)
{

	char		message[70];

	/* Log duration and/or queryid if available */
	if (queryDesc->plannedstmt->queryId)
	{
		snprintf(message, 70, "%sstatement: /* queryid = %ld */",
				 pgsl_get_duration_str(),
#if PG_VERSION_NUM >= 110000
				 queryDesc->plannedstmt->queryId);
#else
				 (uint64) queryDesc->plannedstmt->queryId);
#endif
	}
	else
	{
		snprintf(message, 70, "%sstatement:", pgsl_get_duration_str());
	}

	ereport(pgsl_log_level,
			(errmsg("%s %s",
					message, queryDesc->sourceText),
			 errhidestmt(true)));

	/* Ensure we do not log this query again */
	pgsl_query_issampled = false;
}

void
pgsl_check_transaction_issampled(void)
{
	/* Determine if this transaction is a new one */
	if ((pgsl_transaction_sample_rate > 0 || pgsl_transaction_issampled) &&
		pgsl_nesting_level == 0 && pgsl_previouslxid != MyProc->lxid)
	{

		/* It is a new transaction, so determine if it is sampled */
		pgsl_transaction_issampled = pgsl_transaction_sample_rate == 1 ||
			(random() < pgsl_transaction_sample_rate * MAX_RANDOM_VALUE);
		pgsl_previouslxid = MyProc->lxid;
	}
}

/*
 * Always log statements whose duration exceed statement_sample_limit
 */

bool
pgsl_stmt_limit(void)
{
	struct Duration d;

	d = pgsl_get_duration();

	if (pgsl_nesting_level == 0 &&
		(pgsl_transaction_sample_rate > 0 || pgsl_stmt_sample_rate > 0) &&
		(d.secs * 1000 + d.msecs) >= pgsl_stmt_sample_limit && pgsl_stmt_sample_limit != -1)
		return true;
	return false;
}

/*
 * ProcessUtility hook: We check if statement is a DDL
 */
static void
pgsl_ProcessUtility(
#if PG_VERSION_NUM >= 100000
					PlannedStmt *pstmt,
#else
					Node *parsetree,
#endif
					const char *queryString,
					ProcessUtilityContext context,
					ParamListInfo params,
#if PG_VERSION_NUM >= 100000
					QueryEnvironment *queryEnv,
#endif
					DestReceiver *dest,
					char *completionTag)
{
#if PG_VERSION_NUM < 100000
	PlannedStmt *pstmt;
#endif

	/* Log query if this transaction is sampled */
	pgsl_check_transaction_issampled();

#if PG_VERSION_NUM >= 100000
	if (GetCommandLogLevel((Node *) pstmt) <= pgsl_log_statement || pgsl_stmt_sample_rate == 1
#else
	if (GetCommandLogLevel(parsetree) <= pgsl_log_statement || pgsl_stmt_sample_rate == 1
#endif
		|| pgsl_transaction_issampled)
	{
		ereport(pgsl_log_level,
				(errmsg("%sstatement: %s", pgsl_get_duration_str(),
						queryString),
				 errhidestmt(true)));
	}

	if (prev_ProcessUtility)
		(*prev_ProcessUtility) (
#if PG_VERSION_NUM >= 100000
								pstmt,
#else
								parsetree,
#endif
								queryString,
								context,
								params,
#if PG_VERSION_NUM >= 100000
								queryEnv,
#endif
								dest, completionTag);
	else
		standard_ProcessUtility(
#if PG_VERSION_NUM >= 100000
								pstmt,
#else
								parsetree,
#endif
								queryString,
								context,
								params,
#if PG_VERSION_NUM >= 100000
								queryEnv,
#endif
								dest, completionTag);

}

/*
 * ExecutorStart hook: start up log sampling if needed
 */
static void
pgsl_ExecutorStart(QueryDesc *queryDesc, int eflags)
{

	/* Determine if statement of this transaction is sampled */
	if (pgsl_stmt_sample_rate > 0 && pgsl_nesting_level == 0)
		pgsl_query_issampled |= pgsl_query_issampled || (pgsl_stmt_sample_rate == 1 ||
														 (random() < pgsl_stmt_sample_rate * MAX_RANDOM_VALUE));

	pgsl_check_transaction_issampled();

	/* Always log if statement level <= pg_sampletolog.log_statement */
	if (!pgsl_query_issampled && GetCommandLogLevel((Node *) queryDesc->plannedstmt) <= pgsl_log_statement)
		pgsl_query_issampled = true;

	if (pgsl_log_before_execution &&
		(pgsl_query_issampled || pgsl_transaction_issampled))
	{
		pgsl_log_report(queryDesc);
	}


	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}


/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgsl_ExecutorRun(QueryDesc *queryDesc,
				 ScanDirection direction,
#if PG_VERSION_NUM >= 90600
				 uint64 count
#else
				 long count
#endif
#if PG_VERSION_NUM >= 100000
				 ,bool execute_once
#endif
)
{
	pgsl_nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
#if PG_VERSION_NUM >= 100000
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
#else
			prev_ExecutorRun(queryDesc, direction, count);
#endif
		else
#if PG_VERSION_NUM >= 100000
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
#else
			standard_ExecutorRun(queryDesc, direction, count);
#endif
		pgsl_nesting_level--;
	}
	PG_CATCH();
	{
		pgsl_nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgsl_ExecutorFinish(QueryDesc *queryDesc)
{
	pgsl_nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		pgsl_nesting_level--;
	}
	PG_CATCH();
	{
		pgsl_nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}


/*
 * ExecutorEnd hook: log statement if needed
 */
static void
pgsl_ExecutorEnd(QueryDesc *queryDesc)
{
	if (!pgsl_log_before_execution &&
		(pgsl_query_issampled || pgsl_transaction_issampled || pgsl_stmt_limit()))
	{
		pgsl_log_report(queryDesc);
	}
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}
