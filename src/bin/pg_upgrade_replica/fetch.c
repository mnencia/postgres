/*-------------------------------------------------------------------------
 *
 * fetch.c
 *		Everything that talks to the new primary over a plain libpq/SQL
 *		connection.
 *
 * Bulk file transfer is no longer this tool's own protocol -- it's done by
 * the pg_basebackup/pg_combinebackup subprocesses reuse.c drives, so this
 * file only needs to cover the small, one-off reads that happen before
 * either of those runs: the pg_upgrade_manifest file itself, and a couple
 * of catalog/GUC values (see reuse.c).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/fetch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "common/logging.h"

#include "fetch.h"

static void run_command(PGconn *conn, const char *sql);
static void check_server_build_matches(PGconn *conn);

/*
 * Connect to the new primary and put the connection in the state this
 * tool needs: read-only, no timeouts that could fire mid-sync.
 *
 * This connection is only ever used for a handful of small SQL queries
 * (the manifest file, a couple of catalog lookups); the actual bulk data
 * transfer goes through pg_basebackup's own replication-protocol
 * connection, made separately by that subprocess. This tool has no
 * full_page_writes concern of its own: nothing here does a
 * pg_read_binary_file() against a live, concurrently-written file (which
 * would need one), and pg_basebackup's BASE_BACKUP command already
 * forces full_page_writes on for its own duration server-side (see
 * forcePageWrites in xlog.c) regardless.
 */
RemoteConn *
remote_connect(const ConnParams *cparams, const char *progname)
{
	RemoteConn *rconn;
	PGconn	   *conn;

	/*
	 * connectDatabase() already installs a secure, empty search_path
	 * before returning, so every query below (including
	 * check_server_build_matches()'s unqualified pg_control_system()
	 * call) is already safe from a same-named function on the connecting
	 * role's default search_path intercepting it.
	 */
	conn = connectDatabase(cparams, progname, false, false, true);

	run_command(conn, "SET statement_timeout = 0");
	run_command(conn, "SET lock_timeout = 0");
	run_command(conn, "SET idle_in_transaction_session_timeout = 0");
	run_command(conn, "SET transaction_timeout = 0");
	run_command(conn, "SET default_transaction_read_only = on");

	check_server_build_matches(conn);

	rconn = pg_malloc0(sizeof(RemoteConn));
	rconn->conn = conn;
	rconn->data_directory = run_scalar_query(conn, "SHOW data_directory");

	return rconn;
}

/*
 * find_sibling_exec() already confirms pg_basebackup/pg_combinebackup match
 * this tool's own build; nothing separately confirmed this build itself
 * matches the new primary it just connected to. Two call sites in reuse.c
 * assume that anyway: tablespace_version_dir() combines the connection's
 * own server_version_num with this build's own compiled-in
 * CATALOG_VERSION_NO, and write_old_replica_view_metadata() casts the new
 * primary's own raw pg_control bytes directly to this build's own
 * ControlFileData. Both are silently wrong, not just unsupported, against a
 * mismatched build (a stale binary left on a container image after a minor
 * bump is a realistic way to hit this), so check the one thing that
 * actually governs both: pg_control_system() reports the connected
 * server's real pg_control_version and catalog_version_no, compared here
 * against this build's own compiled-in constants.
 */
static void
check_server_build_matches(PGconn *conn)
{
	PGresult   *res;
	uint32		remote_control_version;
	uint32		remote_catalog_version;

	res = PQexec(conn,
				 "SELECT pg_control_version, catalog_version_no "
				 "FROM pg_control_system()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not query pg_control_system(): %s",
				 PQresultErrorMessage(res));
	if (PQntuples(res) != 1)
		pg_fatal("unexpected result from pg_control_system()");

	remote_control_version = strtoul(PQgetvalue(res, 0, 0), NULL, 10);
	remote_catalog_version = strtoul(PQgetvalue(res, 0, 1), NULL, 10);
	PQclear(res);

	if (remote_control_version != PG_CONTROL_VERSION ||
		remote_catalog_version != CATALOG_VERSION_NO)
		pg_fatal("this build of pg_upgrade_replica (pg_control version %u, "
				 "catalog version %u) does not match the new primary's own "
				 "(pg_control version %u, catalog version %u) -- "
				 "pg_upgrade_replica must be run from the very same build "
				 "as the new primary",
				 PG_CONTROL_VERSION, CATALOG_VERSION_NO,
				 remote_control_version, remote_catalog_version);
}

void
remote_disconnect(RemoteConn *rconn)
{
	disconnectDatabase(rconn->conn);
	pg_free(rconn->data_directory);
	pg_free(rconn);
}

char *
run_scalar_query(PGconn *conn, const char *sql)
{
	PGresult   *res;
	char	   *result;

	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("query failed: %s: %s", sql, PQresultErrorMessage(res));
	if (PQntuples(res) != 1 || PQnfields(res) != 1)
		pg_fatal("unexpected result from query: %s", sql);
	result = pg_strdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	return result;
}

static void
run_command(PGconn *conn, const char *sql)
{
	PGresult   *res;

	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("command failed: %s: %s", sql, PQresultErrorMessage(res));
	PQclear(res);
}

/*
 * One-off read of a small file (just the manifest, in practice): a plain
 * PQexecParams requesting a binary result, so the bytea comes back as raw
 * bytes with no text/hex encoding overhead.
 */
char *
remote_read_whole_file(RemoteConn *rconn, const char *abspath, size_t *len_p)
{
	const char *params[1];
	PGresult   *res;
	int			len;
	char	   *result;

	params[0] = abspath;
	res = PQexecParams(rconn->conn, "SELECT pg_read_binary_file($1)",
					   1, NULL, params, NULL, NULL, 1);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not read file \"%s\": %s", abspath,
				 PQresultErrorMessage(res));
	if (PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_fatal("file \"%s\" is missing on the new primary", abspath);

	len = PQgetlength(res, 0, 0);
	result = pg_malloc(len);
	memcpy(result, PQgetvalue(res, 0, 0), len);
	PQclear(res);

	*len_p = len;
	return result;
}
