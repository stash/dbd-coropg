/* vim: noet sts=0 ts=8 sw=8 */
/*

  $Id: dbdimp.c 13162 2009-08-04 18:10:35Z turnstep $

  Copyright (c) 2002-2009 Greg Sabino Mullane and others: see the Changes file
  Portions Copyright (c) 2002 Jeffrey W. Baker
  Portions Copyright (c) 1997-2000 Edmund Mergl
  Portions Copyright (c) 1994-1997 Tim Bunce
   
  You may distribute under the terms of either the GNU General Public
  License or the Artistic License, as specified in the Perl README file.

*/


#include "Pg.h"

#if defined (_WIN32) && !defined (atoll)
#  define atoll(X) _atoi64(X)
#endif

#ifndef powf
#define powf (float)pow
#endif

#define sword signed int
#define sb2 signed short
#define ub2 unsigned short

#if PGLIBVERSION < 80000

/* Should not be called, throw errors: */
PGresult *PQprepare(PGconn *a, const char *b, const char *c, int d, const Oid *e);
PGresult *PQprepare(PGconn *a, const char *b, const char *c, int d, const Oid *e) {
	if (a||b||c||d||e) d=0;
	croak ("Called wrong PQprepare");
}

int PQserverVersion(const PGconn *a);
int PQserverVersion(const PGconn *a) { if (!a) return 0; croak ("Called wrong PQserverVersion"); }

typedef struct pg_cancel PGcancel;
int	PQcancel(PGcancel *cancel, char *errbuf, int errbufsize);
int	PQcancel(PGcancel *cancel, char *errbuf, int errbufsize) {
	croak ("Called wrong PQcancel");
}
PGcancel *PQgetCancel(PGconn *conn);
PGcancel *PQgetCancel(PGconn *conn) {
	croak ("Called wrong PQgetCancel");
}
void PQfreeCancel(PGcancel *cancel);
void PQfreeCancel(PGcancel *cancel) {
	croak ("Called wrong PQfreeCancel");
}


#endif

#ifndef PGErrorVerbosity
typedef enum
	{
		PGERROR_TERSE,				/* single-line error messages */
		PGERROR_DEFAULT,			/* recommended style */
		PGERROR_VERBOSE				/* all the facts, ma'am */
	} PGErrorVerbosity;
#endif

#define IS_DBI_HANDLE(h)										\
	(SvROK(h) && SvTYPE(SvRV(h)) == SVt_PVHV &&					\
	 SvRMAGICAL(SvRV(h)) && (SvMAGIC(SvRV(h)))->mg_type == 'P')

#define IS_PG_OK_STATUS(status_) ( \
	status_ == PGRES_EMPTY_QUERY || \
	status_ == PGRES_COMMAND_OK ||  \
	status_ == PGRES_TUPLES_OK || \
	status_ == PGRES_COPY_OUT || \
	status_ == PGRES_COPY_IN \
)

	

static void pg_error(pTHX_ SV *h, int error_num, const char *error_msg);
static void _pg_warn (pTHX_ imp_dbh_t *imp_dbh, const char * message, ...);
static void pg_warn (void * arg, const char * message);
static ExecStatusType _result(pTHX_ imp_dbh_t *imp_dbh, const char *sql);
static ExecStatusType _sqlstate(pTHX_ imp_dbh_t *imp_dbh, PGresult *result);
static int pg_db_rollback_commit (pTHX_ SV *dbh, imp_dbh_t *imp_dbh, int action);
static void pg_st_split_statement (pTHX_ imp_sth_t *imp_sth, int version, char *statement);
static int pg_st_prepare_statement (pTHX_ SV *sth, imp_sth_t *imp_sth);
static int is_high_bit_set(pTHX_ const unsigned char *val, STRLEN size);
static int pg_st_deallocate_statement(pTHX_ SV *sth, imp_sth_t *imp_sth);
static PGTransactionStatusType pg_db_txn_status (pTHX_ imp_dbh_t *imp_dbh);
static int pg_db_start_txn (pTHX_ SV *dbh, imp_dbh_t *imp_dbh);
static int handle_old_async(pTHX_ SV * handle, imp_dbh_t * imp_dbh, const int asyncflag);

static const char const *_error_message(const imp_dbh_t *imp_dbh);
static int coro_wait_for (pTHX_ imp_dbh_t *imp_dbh, const char *func);
static PGconn * coro_PQconnectdb(pTHX_ imp_dbh_t *imp_dbh, const char *conn_str);
static PGresult *coro_read_result(pTHX_ imp_dbh_t *imp_dbh);
static int coro_flush(pTHX_ imp_dbh_t *imp_dbh);
static void coro_cleanup (pTHX_ imp_dbh_t *imp_dbh);
static SV *coro_init (pTHX_ imp_dbh_t *imp_dbh, PGconn *conn);
static PGresult *coro_PQexec (pTHX_ imp_dbh_t *imp_dbh, const char *sql);
static PGresult *coro_PQexecParams (pTHX_ imp_sth_t *imp_sth, const char *statement);
static PGresult *coro_PQprepare (pTHX_ imp_sth_t *imp_sth, const char *statement, int params);
static PGresult *coro_PQexecPrepared (pTHX_ imp_sth_t *imp_sth);

static const char *READABLE = "readable";
static const char *WRITABLE = "writable";
#define CORO_WAIT_FOR_READABLE coro_wait_for(aTHX_ imp_dbh, READABLE)
#define CORO_WAIT_FOR_WRITABLE coro_wait_for(aTHX_ imp_dbh, WRITABLE)


/* ================================================================== */
void dbd_init (dbistate_t *dbistate)
{
	dTHX;
	DBIS = dbistate;
}


/* ================================================================== */
int dbd_db_login (SV * dbh, imp_dbh_t * imp_dbh, char * dbname, char * uid, char * pwd)
{

	dTHX;
	char *         conn_str;
	char *         dest;
	bool           inquote = DBDPG_FALSE;
	STRLEN         connect_string_size;
	ConnStatusType connstatus;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_login\n", THEADER);

	/* DBD::Pg syntax: 'dbname=dbname;host=host;port=port', 'User', 'Pass' */
	/* libpq syntax: 'dbname=dbname host=host port=port user=uid password=pwd' */

	/* Figure out how large our connection string is going to be */
	connect_string_size = strlen(dbname);
	if (*uid)
		connect_string_size += strlen("user='' ") + 2*strlen(uid);
	if (*pwd)
		connect_string_size += strlen("password='' ") + 2*strlen(pwd);
	New(0, conn_str, connect_string_size+1, char); /* freed below */

	/* Change all semi-colons in dbname to a space, unless single-quoted */
	dest = conn_str;
	while (*dbname != '\0') {
		if (';' == *dbname && !inquote)
			*dest++ = ' ';
		else {
			if ('\'' == *dbname)
				inquote = !inquote;
			*dest++ = *dbname;
		}
		dbname++;
	}
	*dest = '\0';

	/* Add in the user and/or password if they exist, escaping single quotes and backslashes */
	if (*uid) {
		strcat(conn_str, " user='");
		dest = conn_str;
		while(*dest != '\0')
			dest++;
		while(*uid != '\0') {
			if ('\''==*uid || '\\'==*uid)
				*(dest++)='\\';
			*(dest++)=*(uid++);
		}
		*dest = '\0';
		strcat(conn_str, "'");
	}
	if (*pwd) {
		strcat(conn_str, " password='");
		dest = conn_str;
		while(*dest != '\0')
			dest++;
		while(*pwd != '\0') {
			if ('\''==*pwd || '\\'==*pwd)
				*(dest++)='\\';
			*(dest++)=*(pwd++);
		}
		*dest = '\0';
		strcat(conn_str, "'");
	}

	/* Remove any stored savepoint information */
	if (imp_dbh->savepoints) {
		av_undef(imp_dbh->savepoints);
		sv_free((SV *)imp_dbh->savepoints);
	}
	imp_dbh->savepoints = newAV(); /* freed in dbd_db_destroy */

	/* Close any old connection and free memory, just in case */
	if (imp_dbh->conn) {
		TRACE_PQFINISH;
		PQfinish(imp_dbh->conn);
	}
	
	/* Attempt the connection to the database */
	if (TLOGIN) TRC(DBILOGFP, "%sLogin connection string: (%s)\n", THEADER, conn_str);
	imp_dbh->conn = coro_PQconnectdb(aTHX_ imp_dbh, conn_str);
	if (TLOGIN) TRC(DBILOGFP, "%sConnection complete\n", THEADER);
	Safefree(conn_str);

	/* Set the initial sqlstate */
	Renew(imp_dbh->sqlstate, 6, char); /* freed in dbd_db_destroy */
	strncpy(imp_dbh->sqlstate, "25P01", 6); /* "NO ACTIVE SQL TRANSACTION" */

	/* Check to see that the backend connection was successfully made */
	TRACE_PQSTATUS;
	connstatus = PQstatus(imp_dbh->conn);
	if (CONNECTION_OK != connstatus) {
		pg_error(aTHX_ dbh, connstatus, _error_message(imp_dbh));
		strncpy(imp_dbh->sqlstate, "08006", 6); /* "CONNECTION FAILURE" */
		TRACE_PQFINISH;
		PQfinish(imp_dbh->conn);
		sv_free((SV *)imp_dbh->savepoints);
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_login (error)\n", THEADER);
		return 0;
	}

	/* Call the pg_warn function anytime this connection raises a notice */
	TRACE_PQSETNOTICEPROCESSOR;
	(void)PQsetNoticeProcessor(imp_dbh->conn, pg_warn, (void *)SvRV(dbh));
	
	/* Figure out what protocol this server is using (most likely 3) */
	TRACE_PQPROTOCOLVERSION;
	imp_dbh->pg_protocol = PQprotocolVersion(imp_dbh->conn);

	/* Figure out this particular backend's version */
	imp_dbh->pg_server_version = -1;
#if PGLIBVERSION >= 80000
	TRACE_PQSERVERVERSION;
	imp_dbh->pg_server_version = PQserverVersion(imp_dbh->conn);
#endif

	if (imp_dbh->pg_server_version <= 0) {
		int	cnt, vmaj, vmin, vrev;
		cnt = sscanf(PQparameterStatus(imp_dbh->conn, "server_version"), "%d.%d.%d",
					 &vmaj, &vmin, &vrev);
		if (cnt >= 2) {
			if (cnt == 2) /* Account for devel version e.g. 8.3beta1 */
				vrev = 0;
			imp_dbh->pg_server_version = (100 * vmaj + vmin) * 100 + vrev;
		}
	}

	imp_dbh->pg_bool_tf      = DBDPG_FALSE;
	imp_dbh->pg_enable_utf8  = DBDPG_FALSE;
	imp_dbh->prepare_now     = DBDPG_FALSE;
	imp_dbh->done_begin      = DBDPG_FALSE;
	imp_dbh->dollaronly      = DBDPG_FALSE;
	imp_dbh->expand_array    = DBDPG_TRUE;
	imp_dbh->txn_read_only   = DBDPG_FALSE;
	imp_dbh->pid_number      = getpid();
	imp_dbh->prepare_number  = 1;
	imp_dbh->copystate       = 0;
	imp_dbh->pg_errorlevel   = 1; /* Default */
	imp_dbh->async_status    = 0;
	imp_dbh->async_sth       = NULL;

	/* If using server version 7.4, switch to "smart" */
	imp_dbh->server_prepare = PGLIBVERSION >= 80000 ? 1 : 2;

	/* Tell DBI that we should call destroy when the handle dies */
	DBIc_IMPSET_on(imp_dbh);

	/* Tell DBI that we should call disconnect when the handle dies */
	DBIc_ACTIVE_on(imp_dbh);

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_login\n", THEADER);
	return 1;

} /* end of dbd_db_login */


/* ================================================================== */

/* Database specific error handling. */
static void pg_error (pTHX_ SV * h, int error_num, const char * error_msg)
{
	D_imp_xxh(h);
	size_t error_len;
	imp_dbh_t * imp_dbh = (imp_dbh_t *)(DBIc_TYPE(imp_xxh) == DBIt_ST ? DBIc_PARENT_COM(imp_xxh) : imp_xxh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_error (message: %s number: %d)\n",
					THEADER, error_msg, error_num);

	error_len = strlen(error_msg);

	/* Strip final newline so line number appears for warn/die */
	if (error_len > 0 && error_msg[error_len-1] == 10)
		error_len--;

	sv_setiv(DBIc_ERR(imp_xxh), (IV)error_num);
	sv_setpvn(DBIc_ERRSTR(imp_xxh), error_msg, error_len);
	sv_setpv(DBIc_STATE(imp_xxh), (char*)imp_dbh->sqlstate);

	if (TEND) TRC(DBILOGFP, "%sEnd pg_error\n", THEADER);

} /* end of pg_error */


/* ================================================================== */
/* Turn database notices into perl warnings for proper handling. */
static void _pg_warn (pTHX_ imp_dbh_t *imp_dbh, const char *message, ...)
{
	va_list ap;
	if (TSTART) TRC(DBILOGFP, "%sBegin pg_warn (message: %s DBIc_WARN: %d PrintWarn: %d)\n",
					THEADER,
					message, DBIc_WARN(imp_dbh) ? 1 : 0,
					DBIc_is(imp_dbh, DBIcf_PrintWarn) ? 1 : 0);

	if (DBIc_WARN(imp_dbh) && DBIc_is(imp_dbh, DBIcf_PrintWarn)) {
		va_start(ap,message);
		vwarn(message, &ap);
		va_end(ap);
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_warn\n", THEADER);
}

static void pg_warn (void * arg, const char * message)
{
	dTHX;
	SV *tmp;

	tmp = sv_2mortal(newRV((SV *)arg));

	/* This fun little bit is to prevent a core dump when the following occurs:
	   client_min_messages is set to DEBUG3 or greater, and we exit without a disconnect.
	   DBI issues a 'rollback' in this case, which causes some debugging messages 
	   to be emitted from the server (such as "StartTransactionCommand"). However, we can't do 
	   the D_imp_dbh call anymore, because the underlying dbh has lost some of its magic.
	   Unfortunately, DBI then coredumps in dbh_getcom2. Hence, we make sure that the 
	   object passed in is still 'valid', in that a certain level has a ROK flag.
	   If it's not, we just return without issuing any warning, as we can't check things 
	   like DBIc_WARN. There may be a better way of handling all this, and we may want to 
	   default to always warn() - input welcome.
	*/
	if (!SvROK(SvMAGIC(SvRV(tmp))->mg_obj)) {
		return;
	}
	else {
		D_imp_dbh(tmp);
		_pg_warn(aTHX_ imp_dbh, message);
	}

} /* end of pg_warn */


/* ================================================================== */
/* Quick command executor used throughout this file */
static ExecStatusType _result(pTHX_ imp_dbh_t * imp_dbh, const char * sql)
{
	PGresult *     result;
	ExecStatusType status;

	if (TSTART) TRC(DBILOGFP, "%sBegin _result (sql: %s)\n", THEADER, sql);

	if (TSQL) TRC(DBILOGFP, "%s;\n\n", sql);

	result = coro_PQexec(aTHX_ imp_dbh, sql);
	status = _sqlstate(aTHX_ imp_dbh, result);

	TRACE_PQCLEAR;
	PQclear(result);

	if (TEND) TRC(DBILOGFP, "%sEnd _result\n", THEADER);
	return status;

} /* end of _result */


/* ================================================================== */
/* Set the SQLSTATE based on a result, returns the status */
static ExecStatusType _sqlstate(pTHX_ imp_dbh_t * imp_dbh, PGresult * result)
{
	ExecStatusType status   = PGRES_FATAL_ERROR; /* until proven otherwise */
	bool           stateset = DBDPG_FALSE;
        const char *diag_sqlstate;

	if (TSTART) TRC(DBILOGFP, "%sBegin _sqlstate\n", THEADER);

	switch (imp_dbh->coro_error) {
	case COERR_OK:
                break;
        case COERR_PGRESULT:
                imp_dbh->coro_error = COERR_OK; /* clear once read */
		break;
	case COERR_INTERRUPTED:
		strncpy(imp_dbh->sqlstate, "58030", 6); /* IO ERROR */
		status = PGRES_FATAL_ERROR;
		goto finish_sqlstate;
	default:
		strncpy(imp_dbh->sqlstate, "08000", 6); /* CONNECTION EXCEPTION */
		status = PGRES_FATAL_ERROR;
		goto finish_sqlstate;
	}

	/*
	  Because PQresultErrorField may not work completely when an error occurs, and 
	  we are connecting over TCP/IP, only set it here if non-null, and fall through 
	  to a better default value below.
	*/
	if (result) {
                status = imp_dbh->last_result_status;
		TRACE_PQRESULTERRORFIELD;
                diag_sqlstate = PQresultErrorField(result,PG_DIAG_SQLSTATE);
		if (diag_sqlstate) {
			strncpy(imp_dbh->sqlstate, diag_sqlstate, 5);
			imp_dbh->sqlstate[5] = '\0';
			stateset = DBDPG_TRUE;
		}
	}
	if (!stateset) {
		/* Do our best to map the status result to a sqlstate code */
		switch (status) {
		case PGRES_EMPTY_QUERY:
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
			strncpy(imp_dbh->sqlstate, "00000", 6); /* SUCCESSFUL COMPLETION */
			break;
		case PGRES_BAD_RESPONSE:
		case PGRES_NONFATAL_ERROR:
			strncpy(imp_dbh->sqlstate, "01000", 6); /* WARNING */
			break;
		case PGRES_FATAL_ERROR:
			if (!result) { /* libpq returned null - some sort of connection problem */
				strncpy(imp_dbh->sqlstate, "08000", 6); /* CONNECTION EXCEPTION */
				break;
			}
			/*@fallthrough@*/
		default:
			strncpy(imp_dbh->sqlstate, "22000", 6); /* DATA EXCEPTION */
			break;
		}
	}
finish_sqlstate:

	if (TEND) TRC(DBILOGFP, "%sEnd _sqlstate (imp_dbh->sqlstate: %s)\n",
				  THEADER, imp_dbh->sqlstate);

	if (TRACE7) TRC(DBILOGFP, "%s_sqlstate txn_status is %d\n",
					THEADER, pg_db_txn_status(aTHX_ imp_dbh));

        imp_dbh->last_result_err = NULL;
        imp_dbh->last_result_status = 0;

	if (TEND) TRC(DBILOGFP, "%sEnd _sqlstate (status: %d)\n", THEADER, status);
	return status;

} /* end of _sqlstate */


/* ================================================================== */
int dbd_db_ping (SV * dbh)
{
	dTHX;
	D_imp_dbh(dbh);
	PGTransactionStatusType tstatus;
	ExecStatusType          status;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_ping\n", THEADER);

	if (NULL == imp_dbh->conn) {
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_ping (error: no connection)\n", THEADER);
		return -1;
	}

	if (imp_dbh->coro_error == COERR_PGFATAL) {
		if (TEND || TCORO) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: -2 coro)\n", THEADER);
		return -2;
	}
        else if (imp_dbh->coro_error != COERR_OK) {
		if (TEND || TCORO) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: -4 coro)\n", THEADER);
		return -4;
        }

	if (imp_dbh->copystate) {
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: 2 copy in progress)\n", THEADER);
		return 2;
	}

	tstatus = pg_db_txn_status(aTHX_ imp_dbh);

	if (TRACE5) TRC(DBILOGFP, "%sdbd_db_ping txn_status is %d\n", THEADER, tstatus);

	if (tstatus >= 4) { /* Unknown, so we err on the side of "bad" */
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: -2 unknown/bad)\n", THEADER);
		return -2;
	}

	if (tstatus != 0) {/* 2=active, 3=intrans, 4=inerror */
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: %d)\n", THEADER, 1+tstatus);
		return 1+tstatus;
	}

	/* Even though it may be reported as normal, we have to make sure by issuing a command */

	status = _result(aTHX_ imp_dbh, "SELECT 'DBD::Pg ping test'");

	if (PGRES_TUPLES_OK == status) {
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: 1 PGRES_TUPLES_OK)\n", THEADER);
		return 1;
	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_pg_ping (result: -3)\n", THEADER);
	return -3;

} /* end of dbd_db_ping */


/* ================================================================== */
static PGTransactionStatusType pg_db_txn_status (pTHX_ imp_dbh_t * imp_dbh)
{

	if (TSTART) TRC(DBILOGFP, "%sBegin PGTransactionStatusType\n", THEADER);
	TRACE_PQTRANSACTIONSTATUS;
	return PQtransactionStatus(imp_dbh->conn);

} /* end of pg_db_txn_status */


/* rollback and commit share so much code they get one function: */

/* ================================================================== */
static int pg_db_rollback_commit (pTHX_ SV * dbh, imp_dbh_t * imp_dbh, int action)
{
	PGTransactionStatusType tstatus;
	ExecStatusType          status;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_rollback_commit (action: %s AutoCommit: %d BegunWork: %d, copystate: %d)\n",
					THEADER,
					action ? "commit" : "rollback",
					DBIc_is(imp_dbh, DBIcf_AutoCommit) ? 1 : 0,
					DBIc_is(imp_dbh, DBIcf_BegunWork) ? 1 : 0,
					imp_dbh->copystate);
	
	/* No action if AutoCommit = on or the connection is invalid */
	if ((NULL == imp_dbh->conn) || (DBIc_has(imp_dbh, DBIcf_AutoCommit))) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_commit (result: 0)\n", THEADER);
		return 0;
	}

	/* We only perform these actions if we need to. For newer servers, we 
	   ask it for the status directly and double-check things */

	tstatus = pg_db_txn_status(aTHX_ imp_dbh);
	if (TRACE4) TRC(DBILOGFP, "%sdbd_db_%s txn_status is %d\n", THEADER, action ? "commit" : "rollback", tstatus);

	if (PQTRANS_IDLE == tstatus) { /* Not in a transaction */
		if (imp_dbh->done_begin) {
			/* We think we ARE in a transaction but we really are not */
			if (TRACEWARN)
				TRC(DBILOGFP, "%sWarning: invalid done_begin turned off\n", THEADER);
			imp_dbh->done_begin = DBDPG_FALSE;
		}
	}
	else if (PQTRANS_ACTIVE == tstatus) { /* Still active - probably in a COPY */
		if (TRACEWARN)
			TRC(DBILOGFP,"%sCommand in progress, so no done_begin checking!\n", THEADER);
	}
	else if (PQTRANS_INTRANS == tstatus || PQTRANS_INERROR == tstatus) { /* In a (possibly failed) transaction */
		if (!imp_dbh->done_begin) {
			/* We think we are NOT in a transaction but we really are */
			if (TRACEWARN)
				TRC(DBILOGFP, "%sWarning: invalid done_begin turned on\n", THEADER);
			imp_dbh->done_begin = DBDPG_TRUE;
		}
	}
	else { /* Something is wrong: transaction status unknown */
		if (TRACEWARN)
			TRC(DBILOGFP, "%sWarning: cannot determine transaction status\n", THEADER);
	}

	/* If begin_work has been called, turn AutoCommit back on and BegunWork off */
	if (DBIc_has(imp_dbh, DBIcf_BegunWork)!=0) {
		DBIc_set(imp_dbh, DBIcf_AutoCommit, 1);
		DBIc_set(imp_dbh, DBIcf_BegunWork, 0);
	}

	if (!imp_dbh->done_begin) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_commit (result: 1)\n", THEADER);
		return 1;
	}

	if (imp_dbh->copystate) {
		if (pg_db_endcopy(aTHX_ dbh)!=-1)
			_pg_warn(aTHX_ imp_dbh, "Warning: endcopy failed during %s", action ? "commit" : "rollback");
	}
	status = _result(aTHX_ imp_dbh, action ? "commit" : "rollback");
		
	/* Set this early, for scripts that continue despite the error below */
	imp_dbh->done_begin = DBDPG_FALSE;

	if (PGRES_COMMAND_OK != status) {
		pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_commit (error: status not OK)\n", THEADER);
		return 0;
	}

	/* We just did a rollback or a commit, so savepoints are not relevant, and we cannot be in a PGRES_COPY state */
	av_undef(imp_dbh->savepoints);
	imp_dbh->copystate=0;

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_commit (result: 1)\n", THEADER);
	return 1;

} /* end of pg_db_rollback_commit */

/* ================================================================== */
int dbd_db_commit (SV * dbh, imp_dbh_t * imp_dbh)
{
	dTHX;
	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_commit\n", THEADER);
	return pg_db_rollback_commit(aTHX_ dbh, imp_dbh, 1);
}

/* ================================================================== */
int dbd_db_rollback (SV * dbh, imp_dbh_t * imp_dbh)
{
	dTHX;
	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_rollback\n", THEADER);
	return pg_db_rollback_commit(aTHX_ dbh, imp_dbh, 0);
}


/* ================================================================== */
int dbd_db_disconnect (SV * dbh, imp_dbh_t * imp_dbh)
{
	dTHX;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_disconnect\n", THEADER);

	/* We assume that disconnect will always work	
	   since most errors imply already disconnected. */

	DBIc_ACTIVE_off(imp_dbh);
	
	if (NULL != imp_dbh->conn) {
		/* Attempt a rollback */
		if (0 != dbd_db_rollback(dbh, imp_dbh) && TRACE5)
			TRC(DBILOGFP, "%sdbd_db_disconnect: AutoCommit=off -> rollback\n", THEADER);
		
		TRACE_PQFINISH;
		PQfinish(imp_dbh->conn);
		imp_dbh->conn = NULL;
	}

	/* We don't free imp_dbh since a reference still exists	*/
	/* The DESTROY method is the only one to 'free' memory.	*/
	/* Note that statement objects may still exists for this dbh! */

	if (TLOGIN) TRC(DBILOGFP, "%sDisconnection complete\n", THEADER);

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_disconnect\n", THEADER);
	return 1;

} /* end of dbd_db_disconnect */


/* ================================================================== */
void dbd_db_destroy (SV * dbh, imp_dbh_t * imp_dbh)
{
	dTHX;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_destroy\n", THEADER);

	if (DBIc_ACTIVE(imp_dbh))
		(void)dbd_db_disconnect(dbh, imp_dbh);

	if (imp_dbh->async_sth) { /* Just in case */
		if (imp_dbh->async_sth->result) {
			TRACE_PQCLEAR;
			PQclear(imp_dbh->async_sth->result);
		}
		imp_dbh->async_sth = NULL;
	}

	av_undef(imp_dbh->savepoints);
	sv_free((SV *)imp_dbh->savepoints);
	Safefree(imp_dbh->sqlstate);

	coro_cleanup(aTHX_ imp_dbh);

	DBIc_IMPSET_off(imp_dbh);

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_destroy\n", THEADER);

} /* end of dbd_db_destroy */


/* ================================================================== */
SV * dbd_db_FETCH_attrib (SV * dbh, imp_dbh_t * imp_dbh, SV * keysv)
{
	dTHX;
	STRLEN kl;
	char * key = SvPV(keysv,kl);
	SV *   retsv = Nullsv;
	
	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_FETCH (key: %s)\n", THEADER, dbh ? key : key);

	switch (kl) {

	case 5: /* pg_db */

		if (strEQ("pg_db", key)) {
			TRACE_PQDB;
			retsv = newSVpv(PQdb(imp_dbh->conn),0);
		}
		break;

	case 6: /* pg_pid */

		if (strEQ("pg_pid", key)) {
			TRACE_PQBACKENDPID;
			retsv = newSViv((IV)PQbackendPID(imp_dbh->conn));
		}
		break;

	case 7: /* pg_user  pg_pass  pg_port  pg_host */

		if (strEQ("pg_user", key)) {
			TRACE_PQUSER;
			retsv = newSVpv(PQuser(imp_dbh->conn),0);
		}
		else if (strEQ("pg_pass", key)) {
			TRACE_PQPASS;
			retsv = newSVpv(PQpass(imp_dbh->conn),0);
		}
		else if (strEQ("pg_port", key)) {
			TRACE_PQPORT;
			retsv = newSVpv(PQport(imp_dbh->conn),0);
		}
		else if (strEQ("pg_host", key)) {
			TRACE_PQHOST;
			retsv = PQhost(imp_dbh->conn) ? newSVpv(PQhost(imp_dbh->conn),0) : Nullsv;
		}
		break;

	case 9: /* pg_socket */

		if (strEQ("pg_socket", key)) {
			TRACE_PQSOCKET;
			retsv = newSViv(imp_dbh->socket_fd);
		}
		break;

	case 10: /* AutoCommit  pg_bool_tf  pg_pid_number  pg_options */

		if (strEQ("AutoCommit", key))
			retsv = boolSV(DBIc_has(imp_dbh, DBIcf_AutoCommit));
		else if (strEQ("pg_bool_tf", key))
			retsv = newSViv((IV)imp_dbh->pg_bool_tf);
		else if (strEQ("pg_pid_number", key)) /* Undocumented on purpose */
			retsv = newSViv((IV)imp_dbh->pid_number);
		else if (strEQ("pg_options", key)) {
			TRACE_PQOPTIONS;
			retsv = newSVpv(PQoptions(imp_dbh->conn),0);
		}
		break;

	case 11: /* pg_INV_READ  pg_protocol */

		if (strEQ("pg_INV_READ", key))
			retsv = newSViv((IV)INV_READ);
		else if (strEQ("pg_protocol", key))
			retsv = newSViv((IV)imp_dbh->pg_protocol);
		break;

	case 12: /* pg_INV_WRITE */

		if (strEQ("pg_INV_WRITE", key))
			retsv = newSViv((IV) INV_WRITE );
		break;

	case 13: /* pg_errorlevel */

		if (strEQ("pg_errorlevel", key))
			retsv = newSViv((IV)imp_dbh->pg_errorlevel);
		break;

	case 14: /* pg_lib_version  pg_prepare_now  pg_enable_utf8 */

		if (strEQ("pg_lib_version", key))
			retsv = newSViv((IV) PGLIBVERSION );
		else if (strEQ("pg_prepare_now", key))
			retsv = newSViv((IV)imp_dbh->prepare_now);
#ifdef is_utf8_string
		else if (strEQ("pg_enable_utf8", key))
			retsv = newSViv((IV)imp_dbh->pg_enable_utf8);
#endif
		break;

	case 15: /* pg_default_port pg_async_status pg_expand_array */

		if (strEQ("pg_default_port", key))
			retsv = newSViv((IV) PGDEFPORT );
		else if (strEQ("pg_async_status", key))
			retsv = newSViv((IV)imp_dbh->async_status);
		else if (strEQ("pg_expand_array", key))
			retsv = newSViv((IV)imp_dbh->expand_array);
		break;

	case 17: /* pg_server_prepare  pg_server_version */

		if (strEQ("pg_server_prepare", key))
			retsv = newSViv((IV)imp_dbh->server_prepare);
		else if (strEQ("pg_server_version", key))
			retsv = newSViv((IV)imp_dbh->pg_server_version);
		break;

	case 25: /* pg_placeholder_dollaronly */

		if (strEQ("pg_placeholder_dollaronly", key))
			retsv = newSViv((IV)imp_dbh->dollaronly);
		break;

	case 30: /* pg_standard_conforming_strings */

		if (strEQ("pg_standard_conforming_strings", key)) {
			if (NULL != PQparameterStatus(imp_dbh->conn, "standard_conforming_strings")) {
				retsv = newSVpv(PQparameterStatus(imp_dbh->conn,"standard_conforming_strings"),0);
			}
		}
		break;

	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_FETCH_attrib\n", THEADER);

	if (!retsv)
		return Nullsv;
	
	if (retsv == &sv_yes || retsv == &sv_no) {
		return retsv; /* no need to mortalize yes or no */
	}
	return sv_2mortal(retsv);

} /* end of dbd_db_FETCH_attrib */


/* ================================================================== */
int dbd_db_STORE_attrib (SV * dbh, imp_dbh_t * imp_dbh, SV * keysv, SV * valuesv)
{
	dTHX;
	STRLEN       kl;
	char *       key = SvPV(keysv,kl);
	unsigned int newval = SvTRUE(valuesv);
	int          retval = 0;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_db_STORE (key: %s newval: %d kl:%d)\n", THEADER, key, newval, kl);
	
	switch (kl) {

	case 8: /* ReadOnly */

		if (strEQ("ReadOnly", key)) {
			if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
				warn("Setting ReadOnly in AutoCommit mode has no effect");
			}
			imp_dbh->txn_read_only = newval ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}
		break;

	case 10: /* AutoCommit  pg_bool_tf */

		if (strEQ("AutoCommit", key)) {
			if (newval != DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
				if (newval!=0) { /* It was off but is now on, so do a final commit */
					if (0!=dbd_db_commit(dbh, imp_dbh) && TRACE4)
						TRC(DBILOGFP, "%sSetting AutoCommit to 'on' forced a commit\n", THEADER);
				}
				DBIc_set(imp_dbh, DBIcf_AutoCommit, newval);
			}
			retval = 1;
		}
		else if (strEQ("pg_bool_tf", key)) {
			imp_dbh->pg_bool_tf = newval!=0 ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}
		break;

	case 13: /* pg_errorlevel */

		if (strEQ("pg_errorlevel", key)) {
			if (SvOK(valuesv)) {
				newval = (unsigned)SvIV(valuesv);
			}
			/* Default to "1" if an invalid value is passed in */
			imp_dbh->pg_errorlevel = 0==newval ? 0 : 2==newval ? 2 : 1;
			TRACE_PQSETERRORVERBOSITY;
			(void)PQsetErrorVerbosity(imp_dbh->conn, (PGVerbosity)imp_dbh->pg_errorlevel);
			if (TRACE5)
				TRC(DBILOGFP, "%sReset error verbosity to %d\n", THEADER, imp_dbh->pg_errorlevel);
			retval = 1;
		}
		break;

	case 14: /* pg_prepare_now  pg_enable_utf8 */

		if (strEQ("pg_prepare_now", key)) {
			imp_dbh->prepare_now = newval ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}

#ifdef is_utf8_string
		else if (strEQ("pg_enable_utf8", key)) {
			imp_dbh->pg_enable_utf8 = newval!=0 ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}
#endif
		break;

	case 15: /* pg_expand_array */

		if (strEQ("pg_expand_array", key)) {
			imp_dbh->expand_array = newval ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}
		break;

	case 17: /* pg_server_prepare */

		if (strEQ("pg_server_prepare", key)) {
			if (SvOK(valuesv)) {
				newval = (unsigned)SvIV(valuesv);
			}
			/* Default to "2" if an invalid value is passed in */
			imp_dbh->server_prepare = 0==newval ? 0 : 1==newval ? 1 : 2;
			retval = 1;
		}
		break;

	case 25: /* pg_placeholder_dollaronly */

		if (strEQ("pg_placeholder_dollaronly", key)) {
			imp_dbh->dollaronly = newval ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}
		break;
	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_db_STORE_attrib\n", THEADER);
	return retval;

} /* end of dbd_db_STORE_attrib */


/* ================================================================== */
SV * dbd_st_FETCH_attrib (SV * sth, imp_sth_t * imp_sth, SV * keysv)
{
	dTHX;
	STRLEN            kl;
	char *            key = SvPV(keysv,kl);
	SV *              retsv = Nullsv;
	int               fields, x;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_FETCH (key: %s)\n", THEADER, key);
	
	/* Some can be done before we have a result: */
	switch (kl) {

	case 8: /* pg_bound */

		if (strEQ("pg_bound", key)) {
			HV *pvhv = newHV();
			ph_t *currph;
			int i;
			for (i=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,i++) {
				(void)hv_store_ent 
					(pvhv,
					 (3==imp_sth->placeholder_type ? newSVpv(currph->fooname,0) : newSViv(i+1)),
					 newSViv(NULL == currph->bind_type ? 0 : 1), 0);
			}
			retsv = newRV_noinc((SV*)pvhv);
		}
		break;

	case 9: /* pg_direct */

		if (strEQ("pg_direct", key))
			retsv = newSViv((IV)imp_sth->direct);
		break;

	case 10: /* ParamTypes */

		if (strEQ("ParamTypes", key)) {
			HV *pvhv = newHV();
			ph_t *currph;
			int i;
			for (i=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,i++) {
				if (NULL == currph->bind_type) {
					(void)hv_store_ent
						(pvhv, (3==imp_sth->placeholder_type ? newSVpv(currph->fooname,0) : newSViv(i+1)),
						 newSV(0), 0);
				}
				else {
					HV *pvhv2 = newHV();
					if (currph->bind_type->type.sql) {
						(void)hv_store(pvhv2, "TYPE", 4, newSViv(currph->bind_type->type.sql), 0);
					}
					else {
						(void)hv_store(pvhv2, "pg_type", 7, newSViv(currph->bind_type->type_id), 0);
					}
					(void)hv_store_ent
						(pvhv, (3==imp_sth->placeholder_type ? newSVpv(currph->fooname,0) : newSViv(i+1)),
						 newRV_noinc((SV*)pvhv2), 0);
				}
			}
			retsv = newRV_noinc((SV*)pvhv);
		}
		break;

	case 11: /* ParamValues pg_segments pg_numbound */

		if (strEQ("ParamValues", key)) {
			HV *pvhv = newHV();
			ph_t *currph;
			int i;
			for (i=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,i++) {
				if (NULL == currph->value) {
					(void)hv_store_ent 
						(pvhv,
						 (3==imp_sth->placeholder_type ? newSVpv(currph->fooname,0) : newSViv(i+1)),
						 newSV(0), 0);
				}
				else {
					(void)hv_store_ent
						(pvhv,
						 (3==imp_sth->placeholder_type ? newSVpv(currph->fooname,0) : newSViv(i+1)),
						 newSVpv(currph->value,0),0);
				}
			}
			retsv = newRV_noinc((SV*)pvhv);
		}
		else if (strEQ("pg_segments", key)) {
			AV *arr = newAV();
			seg_t *currseg;
			int i;
			for (i=0,currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg,i++) {
				av_push(arr, newSVpv(currseg->segment ? currseg->segment : "NULL",0));
			}
			retsv = newRV_noinc((SV*)arr);
		}
		else if (strEQ("pg_numbound", key)) {
			ph_t *currph;
			int i = 0;
			for (currph=imp_sth->ph; NULL != currph; currph=currph->nextph) {
				i += NULL == currph->bind_type ? 0 : 1;
			}
			retsv = newSViv(i);
		}
		break;

	case 14: /* pg_prepare_now pg_current_row */

		if (strEQ("pg_prepare_now", key))
			retsv = newSViv((IV)imp_sth->prepare_now);
		else if (strEQ("pg_current_row", key))
			retsv = newSViv(imp_sth->cur_tuple);
		break;

	case 15: /* pg_prepare_name */

		if (strEQ("pg_prepare_name", key))
			retsv = newSVpv((char *)imp_sth->prepare_name, 0);
		break;

	case 17: /* pg_server_prepare */

		if (strEQ("pg_server_prepare", key))
			retsv = newSViv((IV)imp_sth->server_prepare);
		break;

	case 25: /* pg_placeholder_dollaronly */

		if (strEQ("pg_placeholder_dollaronly", key))
			retsv = newSViv((IV)imp_sth->dollaronly);
		break;

	}

	if (retsv != Nullsv) {
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_FETCH_attrib\n", THEADER);
		return retsv;
	}

	if (! imp_sth->result) {
		if (TRACEWARN)
			TRC(DBILOGFP, "%sCannot fetch value of %s pre-execute\n", THEADER, key);
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_FETCH_attrib\n", THEADER);
		return Nullsv;
	}

	fields = DBIc_NUM_FIELDS(imp_sth);
	
	switch (kl) {

	case 4: /* NAME  TYPE */

		if (strEQ("NAME", key)) {
			AV *av = newAV();
			char *fieldname;
			SV * sv_fieldname;
			retsv = newRV(sv_2mortal((SV*)av));
			while(--fields >= 0) {
				TRACE_PQFNAME;
				fieldname = PQfname(imp_sth->result, fields);
				sv_fieldname = newSVpv(fieldname,0);
#ifdef is_utf8_string
				if (is_high_bit_set(aTHX_ (unsigned char *)fieldname, strlen(fieldname)) && is_utf8_string((unsigned char *)fieldname, strlen(fieldname)))
					SvUTF8_on(sv_fieldname);
#endif
				(void)av_store(av, fields, sv_fieldname);
			}
		}
		else if (strEQ("TYPE", key)) {
			/* Need to convert the Pg type to ANSI/SQL type. */
			sql_type_info_t * type_info;
			AV *av = newAV();
			retsv = newRV(sv_2mortal((SV*)av));
			while(--fields >= 0) {
				TRACE_PQFTYPE;
				type_info = pg_type_data((int)PQftype(imp_sth->result, fields));
				(void)av_store(av, fields, newSViv( type_info ? type_info->type.sql : 0 ) );
			}
		}
		break;

	case 5: /* SCALE */

		if (strEQ("SCALE", key)) {
			AV *av = newAV();
			Oid o;
			retsv = newRV(sv_2mortal((SV*)av));
			while(--fields >= 0) {
				TRACE_PQFTYPE;
				o = PQftype(imp_sth->result, fields);
				if (PG_NUMERIC == o) {
					TRACE_PQFMOD;
					o = PQfmod(imp_sth->result, fields)-4;
					(void)av_store(av, fields, newSViv(o % (o>>16)));
				}
				else {
					(void)av_store(av, fields, &sv_undef);
				}
			}
		}
		break;

	case 7: /* pg_size  pg_type */

		if (strEQ("pg_size", key)) {
			AV *av = newAV();
			retsv = newRV(sv_2mortal((SV*)av));
			while(--fields >= 0) {
				TRACE_PQFSIZE;
				(void)av_store(av, fields, newSViv(PQfsize(imp_sth->result, fields)));
			}
		}
		else if (strEQ("pg_type", key)) {
			sql_type_info_t * type_info;
			AV *av = newAV();
			retsv = newRV(sv_2mortal((SV*)av));
			while(--fields >= 0) {			
				TRACE_PQFTYPE;
				type_info = pg_type_data((int)PQftype(imp_sth->result,fields));
				(void)av_store(av, fields, newSVpv(type_info ? type_info->type_name : "unknown", 0));
			}
		}
		break;

	case 8: /* pg_async  NULLABLE */

		if (strEQ("pg_async", key))
			retsv = newSViv((IV)imp_sth->async_flag);
		else if (strEQ("NULLABLE", key)) {
			AV *av = newAV();
			PGresult *result;
			int status = -1;
			D_imp_dbh_from_sth;
			int nullable; /* 0 = not nullable, 1 = nullable 2 = unknown */
			int y;
			retsv = newRV(sv_2mortal((SV*)av));

			while(--fields >= 0) {
				nullable=2;
				TRACE_PQFTABLE;
				x = PQftable(imp_sth->result, fields);
				TRACE_PQFTABLECOL;
				y = PQftablecol(imp_sth->result, fields);
				if (InvalidOid != x && y > 0) { /* We know what table and column this came from */
					/* XXX: should this be turned into a placeholder-style statement? */
					char statement[128];
					snprintf(statement, sizeof(statement),
							"SELECT attnotnull FROM pg_catalog.pg_attribute WHERE attrelid=%d AND attnum=%d", x, y);
					result = coro_PQexec(aTHX_ imp_dbh, statement);
					status = _sqlstate(aTHX_ imp_dbh, result);
					if (PGRES_TUPLES_OK == status) {
						TRACE_PQNTUPLES;
						if (PQntuples(result)!=0) {
							TRACE_PQGETVALUE;
							switch (PQgetvalue(result,0,0)[0]) {
							case 't':
								nullable = 0;
								break;
							case 'f':
							default:
								nullable = 1;
								break;
							}
						}
					}
					if (result) {
						TRACE_PQCLEAR;
						PQclear(result);
					}
					if (PGRES_FATAL_ERROR == status) break; // while fields
				}
				(void)av_store(av, fields, newSViv(nullable));
			}
		}
		break;

	case 9: /* PRECISION */

		if (strEQ("PRECISION", key)) {
			AV *av = newAV();
			int sz = 0;
			Oid o;
			retsv = newRV(sv_2mortal((SV*)av));
			while(--fields >= 0) {
				TRACE_PQFTYPE;
				o = PQftype(imp_sth->result, fields);
				switch (o) {
				case PG_BPCHAR:
				case PG_VARCHAR:
					TRACE_PQFMOD;
					sz = PQfmod(imp_sth->result, fields);
					break;
				case PG_NUMERIC:
					TRACE_PQFMOD;
					sz = PQfmod(imp_sth->result, fields)-4;
					if (sz > 0)
						sz = sz >> 16;
					break;
				default:
					TRACE_PQFSIZE;
					sz = PQfsize(imp_sth->result, fields);
					break;
				}
				(void)av_store(av, fields, sz > 0 ? newSViv(sz) : &sv_undef);
			}
		}
		break;

	case 10: /* CursorName */

		if (strEQ("CursorName", key))
			retsv = &sv_undef;
		break;

	case 11: /* RowsInCache */

		if (strEQ("RowsInCache", key))
			retsv = &sv_undef;
		break;

	case 13: /* pg_oid_status  pg_cmd_status */
		if (strEQ("pg_oid_status", key)) {
			TRACE_PQOIDVALUE;
			retsv = newSVuv((unsigned int)PQoidValue(imp_sth->result));
		}
		else if (strEQ("pg_cmd_status", key)) {
			TRACE_PQCMDSTATUS;
			retsv = newSVpv((char *)PQcmdStatus(imp_sth->result), 0);
		}
		break;

	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_FETCH_attrib\n", THEADER);

	if (retsv == Nullsv)
		return Nullsv;

	return sv_2mortal(retsv);

} /* end of dbd_st_FETCH_attrib */


/* ================================================================== */
int dbd_st_STORE_attrib (SV * sth, imp_sth_t * imp_sth, SV * keysv, SV * valuesv)
{
	dTHX;
	STRLEN kl;
	char * key = SvPV(keysv,kl);
	STRLEN vl;
	char * value = SvPV(valuesv,vl);
	int    retval = 0;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_STORE (key: %s value: %s)\n",
					THEADER, key, value);
	
	switch (kl) {

	case 8: /* pg_async */

		if (strEQ("pg_async", key)) {
			imp_sth->async_flag = (int)SvIV(valuesv);
			retval = 1;
		}
		break;

	case 14: /* pg_prepare_now */

		if (strEQ("pg_prepare_now", key)) {
			imp_sth->prepare_now = strEQ(value,"0") ? DBDPG_FALSE : DBDPG_TRUE;
			retval = 1;
		}
		break;

	case 15: /* pg_prepare_name */

		if (strEQ("pg_prepare_name", key)) {
			Safefree(imp_sth->prepare_name);
			New(0, imp_sth->prepare_name, vl+1, char); /* freed in dbd_st_destroy */
			Copy(value, imp_sth->prepare_name, vl, char);
			imp_sth->prepare_name[vl] = '\0';
			retval = 1;
		}
		break;

	case 17: /* pg_server_prepare*/

		if (strEQ("pg_server_prepare", key)) {
			imp_sth->server_prepare = strEQ(value,"0") ? DBDPG_FALSE : DBDPG_TRUE;
			retval = 1;
		}
		break;

	case 25: /* pg_placeholder_dollaronly */

		if (strEQ("pg_placeholder_dollaronly", key)) {
			imp_sth->dollaronly = SvTRUE(valuesv) ? DBDPG_TRUE : DBDPG_FALSE;
			retval = 1;
		}
		break;
	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_STORE_attrib\n", THEADER);
	return retval;

} /* end of dbd_st_STORE_attrib */


/* ================================================================== */
int dbd_discon_all (SV * drh, imp_drh_t * imp_drh)
{
	dTHX;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_discon_all\n", THEADER);

	/* The disconnect_all concept is flawed and needs more work */
	if (!PL_dirty && !SvTRUE(perl_get_sv("DBI::PERL_ENDING",0))) {
		sv_setiv(DBIc_ERR(imp_drh), (IV)1);
		sv_setpv(DBIc_ERRSTR(imp_drh), "disconnect_all not implemented");
	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_discon_all\n", THEADER);
	return 0;

} /* end of dbd_discon_all */


/* Deprecated in favor of $dbh->{pg_socket} */
/* ================================================================== */
int pg_db_getfd (imp_dbh_t * imp_dbh)
{
	dTHX;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_getfd\n", THEADER);

	TRACE_PQSOCKET;
	return PQsocket(imp_dbh->conn);

} /* end of pg_db_getfd */


/* ================================================================== */
SV * pg_db_pg_notifies (SV * dbh, imp_dbh_t * imp_dbh)
{
	dTHX;
	int        status;
	PGnotify * notify;
	AV *       ret;
	SV *       retsv;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_pg_notifies\n", THEADER);

	TRACE_PQCONSUMEINPUT;
	status = PQconsumeInput(imp_dbh->conn);
	if (0 == status) { 
		pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_pg_notifies (error)\n", THEADER);
		return &sv_undef;
	}

	TRACE_PQNOTIFIES;
	notify = PQnotifies(imp_dbh->conn);

	if (!notify) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_pg_notifies (undef)\n", THEADER);
		return &sv_undef; 
	}

	ret=newAV();
	av_push(ret, newSVpv(notify->relname,0) );
	av_push(ret, newSViv(notify->be_pid) );
	av_push(ret, newSVpv(notify->extra,0) );
	/* Think about utf-8 in payloads someday... */
	
	TRACE_PQFREEMEM;
	PQfreemem(notify);

	retsv = newRV(sv_2mortal((SV*)ret));

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_pg_notifies\n", THEADER);
	return sv_2mortal(retsv);

} /* end of pg_db_pg_notifies */


/* ================================================================== */
int dbd_st_prepare (SV * sth, imp_sth_t * imp_sth, char * statement, SV * attribs)
{
	dTHX;
	D_imp_dbh_from_sth;
	STRLEN mypos=0, wordstart, newsize; /* Used to find and set firstword */
	SV **svp; /* To help parse the arguments */

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_prepare (statement: %s)\n", THEADER, statement);

	if ('\0' == *statement)
		croak ("Cannot prepare empty statement");

	/* Set default values for this statement handle */
	imp_sth->placeholder_type = 0;
	imp_sth->numsegs          = 0;
	imp_sth->numphs           = 0;
	imp_sth->numbound         = 0;
	imp_sth->cur_tuple        = 0;
	imp_sth->rows             = -1; /* per DBI spec */
	imp_sth->totalsize        = 0;
	imp_sth->async_flag       = 0;
	imp_sth->async_status     = 0;
	imp_sth->prepare_name     = NULL;
	imp_sth->firstword        = NULL;
	imp_sth->result           = NULL;
	imp_sth->type_info        = NULL;
	imp_sth->seg              = NULL;
	imp_sth->ph               = NULL;
	imp_sth->PQvals           = NULL;
	imp_sth->PQlens           = NULL;
	imp_sth->PQfmts           = NULL;
	imp_sth->PQoids           = NULL;
	imp_sth->prepared_by_us   = DBDPG_FALSE; /* Set to 1 when actually done preparing */
	imp_sth->onetime          = DBDPG_FALSE; /* Allow internal shortcut */
	imp_sth->direct           = DBDPG_FALSE;
	imp_sth->is_dml           = DBDPG_FALSE; /* Not preparable DML until proved otherwise */
	imp_sth->has_binary       = DBDPG_FALSE; /* Are any of the params binary? */
	imp_sth->has_default      = DBDPG_FALSE; /* Are any of the params DEFAULT? */
	imp_sth->has_current      = DBDPG_FALSE; /* Are any of the params DEFAULT? */
	imp_sth->use_inout        = DBDPG_FALSE; /* Are any of the placeholders using inout? */
	imp_sth->all_bound        = DBDPG_FALSE; /* Have all placeholders been bound? */

	/* We inherit some preferences from the database handle */
	imp_sth->server_prepare   = imp_dbh->server_prepare;
	imp_sth->prepare_now      = imp_dbh->prepare_now;
	imp_sth->dollaronly       = imp_dbh->dollaronly;

	/* Parse and set any attributes passed in */
	if (attribs) {
		if ((svp = hv_fetch((HV*)SvRV(attribs),"pg_server_prepare", 17, 0)) != NULL) {
			int newval = (int)SvIV(*svp);
			/* Default to "2" if an invalid value is passed in */
			imp_sth->server_prepare = 0==newval ? 0 : 1==newval ? 1 : 2;
		}
		if ((svp = hv_fetch((HV*)SvRV(attribs),"pg_direct", 9, 0)) != NULL)
			imp_sth->direct = 0==SvIV(*svp) ? DBDPG_FALSE : DBDPG_TRUE;
		else if ((svp = hv_fetch((HV*)SvRV(attribs),"pg_prepare_now", 14, 0)) != NULL) {
			imp_sth->prepare_now = 0==SvIV(*svp) ? DBDPG_FALSE : DBDPG_TRUE;
		}
		if ((svp = hv_fetch((HV*)SvRV(attribs),"pg_placeholder_dollaronly", 25, 0)) != NULL) {
			imp_sth->dollaronly = SvTRUE(*svp) ? DBDPG_TRUE : DBDPG_FALSE;
		}
		if ((svp = hv_fetch((HV*)SvRV(attribs),"pg_async", 8, 0)) != NULL) {
			imp_sth->async_flag = (int)SvIV(*svp);
		}
	}

	/* Figure out the first word in the statement */
	while (*statement && isSPACE(*statement)) {
		mypos++;
		statement++;
	}
	if (isALPHA(*statement)) {
		wordstart = mypos;
		while (isALPHA(*statement)) {
			mypos++;
			statement++;
		}
		newsize = mypos-wordstart;
		New(0, imp_sth->firstword, newsize+1, char); /* freed in dbd_st_destroy */
		Copy(statement-newsize, imp_sth->firstword, newsize, char);
		imp_sth->firstword[newsize] = '\0';

		/* Note whether this is preparable DML */
		if (0 == strcasecmp(imp_sth->firstword, "SELECT") ||
			0 == strcasecmp(imp_sth->firstword, "INSERT") ||
			0 == strcasecmp(imp_sth->firstword, "UPDATE") ||
			0 == strcasecmp(imp_sth->firstword, "DELETE")
			) {
			imp_sth->is_dml = DBDPG_TRUE;
		}
	}
	statement -= mypos; /* Rewind statement */

	/* Break the statement into segments by placeholder */
	pg_st_split_statement(aTHX_ imp_sth, imp_dbh->pg_server_version, statement);

	/*
	  We prepare it right away if:
	  1. The statement is DML
	  2. The attribute "direct" is false
	  3. The attribute "pg_server_prepare" is not 0
	  4. The attribute "pg_prepare_now" is true
	  5. We are compiled on a 8 or greater server
	*/
	if (TRACE4)	TRC(DBILOGFP,
					"%sImmediate prepare decision: dml=%d direct=%d server_prepare=%d prepare_now=%d PGLIBVERSION=%d\n",
					THEADER,
					imp_sth->is_dml,
					imp_sth->direct,
					imp_sth->server_prepare,
					imp_sth->prepare_now,
					PGLIBVERSION);

	if (imp_sth->is_dml
		&& !imp_sth->direct
		&& 0 != imp_sth->server_prepare
		&& imp_sth->prepare_now
		&& PGLIBVERSION >= 80000
		) {
		if (TRACE5) TRC(DBILOGFP, "%sRunning an immediate prepare\n", THEADER);

		if (pg_st_prepare_statement(aTHX_ sth, imp_sth)!=0) {
			croak (_error_message(imp_dbh));
		}
	}

	/* Tell DBI to call destroy when this handle ends */
	DBIc_IMPSET_on(imp_sth);

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_prepare\n", THEADER);
	return 1;

} /* end of dbd_st_prepare */


/* ================================================================== */
static void pg_st_split_statement (pTHX_ imp_sth_t * imp_sth, int version, char * statement)
{

	/* Builds the "segment" and "placeholder" structures for a statement handle */

	D_imp_dbh_from_sth;

	STRLEN currpos; /* Where we currently are in the statement string */

	STRLEN sectionstart, sectionstop; /* Borders of current section */

	STRLEN sectionsize; /* Size of an allocated segment */

	STRLEN backslashes; /* Counts backslashes, only used in quote section */

	STRLEN dollarsize; /* Size of dollarstring */

	int topdollar; /* Used to enforce sequential $1 arguments */

	int placeholder_type; /* Which type we are in: one of 0,1,2,3 (none,?,$,:) */

	unsigned char ch; /* The current character being checked */

	char quote; /* Current quote or comment character: used only in those two blocks */

	bool found; /* Simple boolean */

	bool inside_dollar; /* Inside a dollar quoted value */

	char * dollarstring = NULL; /* Dynamic string between $$ in dollar quoting */

	char standard_conforming_strings = 1; /* Status 0=on 1=unknown -1=off */

	STRLEN xlen; /* Because "x" is too hard to search for */

	int xint;

	seg_t *newseg, *currseg = NULL; /* Segment structures to help build linked lists */

	ph_t *newph, *thisph, *currph = NULL; /* Placeholder structures to help build ll */

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_st_split_statement\n", THEADER);
	if (TRACE6) TRC(DBILOGFP, "%spg_st_split_statement: (%s)\n", THEADER, statement);
	
	/*
	  If the pg_direct flag is set (or the string has no length), we do not split at all,
	  but simply put everything verbatim into a single segment and return.
	*/
	if (imp_sth->direct || '\0' == *statement) {
		if (TRACE4) {
			TRC(DBILOGFP, "%snot splitting due to %s\n",
				THEADER, imp_sth->direct ? "pg_direct" : "empty string");
		}
		imp_sth->numsegs   = 1;
		imp_sth->numphs    = 0;
		imp_sth->totalsize = strlen(statement);

		New(0, imp_sth->seg, 1, seg_t); /* freed in dbd_st_destroy */
		imp_sth->seg->placeholder = 0;
		imp_sth->seg->nextseg     = NULL;
		imp_sth->seg->ph          = NULL;

		if (imp_sth->totalsize > 0) {
			New(0, imp_sth->seg->segment, imp_sth->totalsize+1, char); /* freed in dbd_st_destroy */
			Copy(statement, imp_sth->seg->segment, imp_sth->totalsize+1, char);
		}
		else {
			imp_sth->seg->segment = NULL;
		}
		if (TRACE6) TRC(DBILOGFP, "%sdirect split = (%s) length=(%d)\n",
						THEADER, imp_sth->seg->segment, (int)imp_sth->totalsize);
		if (TEND) TRC(DBILOGFP, "%sEnd pg_st_split_statement (direct)\n", THEADER);
		return;
	}

	/* Start everyone at the start of the string */
	currpos = sectionstart = 0;

	ch = 1;

	while (1) {

		/* Are we done processing this string? */
		if (ch < 1) {
			break;
		}

		/* Put the current letter into ch, and advance statement to the next character */
		ch = *statement++;

		/* Remember: currpos matches *statement, not ch */
		currpos++;

		/* Quick short-circuit for uninteresting characters */
		if (
			(ch < 34 && ch != 0) || (ch > 63 && ch != 91) ||
			(ch!=34 && ch!=39 &&    /* simple quoting */
			 ch!=45 && ch!=47 &&    /* comment */
			 ch!=36 &&              /* dollar quoting or placeholder */
			 ch!=58 && ch!=63 &&    /* placeholder */
			 ch!=91 &&              /* array slice */
			 ch!=0                  /* end of the string (create segment) */
			 )
			) {
			continue;
		}

		/* 1: A traditionally quoted section */
		if ('\'' == ch || '"' == ch) {
			quote = ch;
			backslashes = 0;
			if ('\'' == ch && 1 == standard_conforming_strings) {
				const char * scs = PQparameterStatus(imp_dbh->conn,"standard_conforming_strings");
				standard_conforming_strings = (NULL==scs ? 1 : strncmp(scs,"on",2));
			}

			/* Go until ending quote character (unescaped) or end of string */
			while (quote && ++currpos && (ch = *statement++)) {
				/* 1.1 : single quotes have no meaning in double-quoted sections and vice-versa */
				/* 1.2 : backslashed quotes do not end the section */
				/* 1.2.1 : backslashes have no meaning in double quoted sections */
				/* 1.2.2 : if standard_confirming_strings is set, ignore backslashes in single quotes */
				if (ch == quote && (quote == '"' || 0==(backslashes&1))) {
					quote = 0;
				}
				else if ('\\' == ch) {
					if (quote == '"' || standard_conforming_strings)
						backslashes++;
				}
				else
					backslashes = 0;
			}
			/* 1.3 Quote ended normally, not the end of the string */
			if (ch != 0)
				continue;
			/* 1.4 String ended, but the quote did not */
			if (0 != quote) {
				/* Let the backend handle this */
			}

			/* 1.5: End quote was the last character in the string */
		} /* end quote section */

		/* 2: A comment block: */
		if (('-' == ch && '-' == *statement) ||
			('/' == ch && '*' == *statement)
			) {
			quote = *statement;
			/* Go until end of comment (may be newline) or end of the string */
			while (quote && ++currpos && (ch = *statement++)) {
				/* 2.1: dashdash only terminates at newline */
				if ('-' == quote && '\n' == ch) {
					quote=0;
				}
				/* 2.2: slashstar ends with a matching starslash */
				else if ('*' == quote && '*' == ch && '/' == *statement) {
					/* Slurp up the slash */
					ch = *statement++;
					currpos++;
					quote=0;
				}
			}

			/* 2.3 Comment ended normally, not the end of the string */
			if (ch != 0)
				continue;

			/* 2.4 String ended, but the comment did not - do nothing special */
			/* 2.5: End quote was the last character in the string */
		} /* end comment section */

		/* 3: advanced dollar quoting - only if the backend is version 8 or higher */
		if (version >= 80000 && '$' == ch && 
			(*statement == '$' 
			 || *statement == '_'
			 || (*statement >= 'A' && *statement <= 'Z') 
			 || (*statement >= 'a' && *statement <= 'z'))) {
			/* "SQL identifiers must begin with a letter (a-z, but also letters with diacritical marks and non-Latin letters) 
			   or an underscore (_). Subsequent characters in an identifier or key word can be letters, underscores, 
			   digits (0-9), or dollar signs ($)
			*/
			/* Postgres technically allows \200-\377 as well, but we don't */
			sectionsize = 0; /* How far from the first dollar sign are we? */
			found = 0; /* Have we found the end of the dollarquote? */

			/* Scan forward until we hit the matching dollarsign */
			while ((ch = *statement++)) {

				sectionsize++;
				if ('$' == ch) {
					found = DBDPG_TRUE;
					break;
				}

				/* If we hit an invalid character, bail out */
				if (ch <= 47 
					|| (ch >= 58 && ch <= 64)
					|| (ch >= 91 && ch <= 94)
					|| ch == 96
					|| (ch >= 123)) {
					break;
				}
			} /* end first scan */

			/* Not found? Move to the next letter after the dollarsign and move on */
			if (!found) {
				statement -= sectionsize;
				if (!ch) {
					ch = 1; /* So the top loop still works */
					statement--;
				}
				continue;
			}

			/* We only need to create a dollarstring if something was between the two dollar signs */
			if (sectionsize >= 1) {
				New(0, dollarstring, sectionsize, char); /* note: a true array, not a null-terminated string */
				strncpy(dollarstring, statement-sectionsize, sectionsize);
			}

			/* Move on and see if the quote is ever closed */

			inside_dollar=0; /* Are we evaluating the dollar sign for the end? */
			dollarsize = sectionsize;
			xlen=0; /* The current character we are tracing */
			found=0;
			while ((ch = *statement++)) {
				sectionsize++;
				if (inside_dollar) {
					/* Special case of $$ */
					if (dollarsize < 1) {
						found = DBDPG_TRUE;
						break;
					}
					if (ch == dollarstring[xlen++]) {
						/* Got a total match? */
						if (xlen >= dollarsize) {
							found = DBDPG_TRUE;
							statement++;
							sectionsize--;
							break;
						}
					}
					else { /* False dollar string: reset */
						inside_dollar=0;
						xlen=0;
						/* Fall through in case this is a dollar sign */
					}
				}
				if ('$' == ch) {
					inside_dollar = DBDPG_TRUE;
				}
			}

			/* Once here, we are either rewinding, or are done parsing the string */

			/* If end of string, rewind one character */
			if (0==ch) {
				sectionsize--;
			}

			if (dollarstring)
				Safefree(dollarstring);

			/* Advance our cursor to the current position */
			currpos += sectionsize+1;

			statement--; /* Rewind statement by one */

			/* If not found, might be end of string, so set ch */
			if (!found) {
				ch = 1;
			}

			/* Regardless if found or not, we send it back */
			continue;

		} /* end dollar quoting */
		
		/* All we care about at this point is placeholder characters and end of string */
		if ('?' != ch && '$' != ch && ':' != ch && 0!=ch) {
			continue;
		}

		/* We might slurp in a placeholder, so mark the character before the current one */
		/* In other words, inside of "ABC?", set sectionstop to point to "C" */
		sectionstop=currpos-1;

		/* Figure out if we have a placeholder */
		placeholder_type = 0;

		/* Dollar sign placeholder style */
		if ('$' == ch && isDIGIT(*statement)) {
			if ('0' == *statement)
				croak("Invalid placeholder value");
			while(isDIGIT(*statement)) {
				++statement;
				++currpos;
			}
			placeholder_type = 2;
		}
		else if (! imp_sth->dollaronly) {
			/* Question mark style */
			if ('?' == ch) {
				placeholder_type = 1;
			}
			/* Colon style, but skip two colons in a row (e.g. myval::float) */
			else if (':' == ch) {
				if (':' == *statement) {
					/* Might as well skip _all_ consecutive colons */
					while(':' == *statement) {
						++statement;
						++currpos;
					}
					continue;
				}
				if (isALNUM(*statement)) {
					while(isALNUM(*statement)) {
						++statement;
						++currpos;
					}
					placeholder_type = 3;
				}
			}
		}

		/* Check for conflicting placeholder types */
		if (placeholder_type!=0) {
			if (imp_sth->placeholder_type && placeholder_type != imp_sth->placeholder_type)
				croak("Cannot mix placeholder styles \"%s\" and \"%s\"",
					  1==imp_sth->placeholder_type ? "?" : 2==imp_sth->placeholder_type ? "$1" : ":foo",
					  1==placeholder_type ? "?" : 2==placeholder_type ? "$1" : ":foo");
		}
		
		/* Move on to the next letter unless we found a placeholder, or we are at the end of the string */
		if (0==placeholder_type && ch)
			continue;

		/* If we got here, we have a segment that needs to be saved */
		New(0, newseg, 1, seg_t); /* freed in dbd_st_destroy */
		newseg->nextseg = NULL;
		newseg->placeholder = 0;
		newseg->ph = NULL;

		if (1==placeholder_type) {
			newseg->placeholder = ++imp_sth->numphs;
		}
		else if (2==placeholder_type) {
			newseg->placeholder = atoi(statement-(currpos-sectionstop-1));
		}
		else if (3==placeholder_type) {
			sectionsize = currpos-sectionstop;
			/* Have we seen this placeholder yet? */
			for (xint=1,thisph=imp_sth->ph; NULL != thisph; thisph=thisph->nextph,xint++) {
				if (0==strncmp(thisph->fooname, statement-sectionsize, sectionsize)) {
					newseg->placeholder = xint;
					newseg->ph = thisph;
					break;
				}
			}
			if (0==newseg->placeholder) {
				imp_sth->numphs++;
				newseg->placeholder = imp_sth->numphs;
				New(0, newph, 1, ph_t); /* freed in dbd_st_destroy */
				newseg->ph        = newph;
				newph->nextph     = NULL;
				newph->bind_type  = NULL;
				newph->value      = NULL;
				newph->quoted     = NULL;
				newph->referenced = DBDPG_FALSE;
				newph->defaultval = DBDPG_TRUE;
				newph->isdefault  = DBDPG_FALSE;
				newph->iscurrent  = DBDPG_FALSE;
				newph->isinout    = DBDPG_FALSE;
				New(0, newph->fooname, sectionsize+1, char); /* freed in dbd_st_destroy */
				Copy(statement-sectionsize, newph->fooname, sectionsize, char);
				newph->fooname[sectionsize] = '\0';
				if (NULL==currph) {
					imp_sth->ph = newph;
				}
				else {
					currph->nextph = newph;
				}
				currph = newph;
			}
		} /* end if placeholder_type */

		sectionsize = sectionstop-sectionstart; /* 4-0 for "ABCD" */
		if (sectionsize>0) {
			New(0, newseg->segment, sectionsize+1, char); /* freed in dbd_st_destroy */
			Copy(statement-(currpos-sectionstart), newseg->segment, sectionsize, char);
			newseg->segment[sectionsize] = '\0';
			imp_sth->totalsize += sectionsize;
		}
		else {
			newseg->segment = NULL;
		}
		if (TRACE6)
			TRC(DBILOGFP, "%sCreated segment (%s)\n", THEADER, newseg->segment);
		
		/* Tie it in to the previous one */
		if (NULL==currseg) {
			imp_sth->seg = newseg;
		}
		else {
			currseg->nextseg = newseg;
		}
		currseg = newseg;
		sectionstart = currpos;
		imp_sth->numsegs++;

		if (placeholder_type > 0)
			imp_sth->placeholder_type = placeholder_type;

		/* If this segment also, ended the string, set ch so we bail out early */
		if ('\0' == *statement)
			break;

	} /* end large while(1) loop: statement parsing */

	/* For dollar sign placeholders, ensure that the rules are followed */
	if (2==imp_sth->placeholder_type) {
		/* 
		   We follow the Pg rules: must start with $1, repeats are allowed, 
		   numbers must be sequential. We change numphs if repeats found
		*/
		topdollar=0;
		for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
			if (currseg->placeholder > topdollar)
				topdollar = currseg->placeholder;
		}
		/* Make sure every placeholder from 1 to topdollar is used at least once */
		for (xint=1; xint <= topdollar; xint++) {
			for (found=0, currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
				if (currseg->placeholder==xint) {
					found = DBDPG_TRUE;
					break;
				}
			}
			if (!found)
				croak("Invalid placeholders: must start at $1 and increment one at a time (expected: $%d)\n", xint);
		}
		if (TRACE6)	TRC(DBILOGFP, "%sSet number of placeholders to %d\n", THEADER, topdollar);
		imp_sth->numphs = topdollar;
	}

	/* Create sequential placeholders */
	if (3 != imp_sth->placeholder_type) {
		for (xint=1; xint <= imp_sth->numphs; xint++) {
			New(0, newph, 1, ph_t); /* freed in dbd_st_destroy */
			newph->nextph     = NULL;
			newph->bind_type  = NULL;
			newph->value      = NULL;
			newph->quoted     = NULL;
			newph->fooname    = NULL;
			newph->referenced = DBDPG_FALSE;
			newph->defaultval = DBDPG_TRUE;
			newph->isdefault  = DBDPG_FALSE;
			newph->iscurrent  = DBDPG_FALSE;
			newph->isinout    = DBDPG_FALSE;
			/* Let the correct segment(s) point to it */
			for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
				if (currseg->placeholder==xint) {
					currseg->ph = newph;
				}
			}
			if (NULL==currph) {
				imp_sth->ph = newph;
			}
			else {
				currph->nextph = newph;
			}
			currph = newph;
		}
	}

	if (TRACE7) {
		TRC(DBILOGFP, "%sPlaceholder type: %d numsegs: %d numphs: %d\n",
			THEADER, imp_sth->placeholder_type, imp_sth->numsegs, imp_sth->numphs);
		TRC(DBILOGFP, "%sPlaceholder numbers and segments:\n",
			THEADER);
		for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
			TRC(DBILOGFP, "%sPH: (%d) SEG: (%s)\n",
				THEADER, currseg->placeholder, currseg->segment);
		}
		if (imp_sth->numphs) {
			TRC(DBILOGFP, "%sPlaceholder number, fooname, id:\n", THEADER);
			for (xlen=1,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,xlen++) {
				TRC(DBILOGFP, "%s#%d FOONAME: (%s)\n",
					THEADER, (int)xlen, currph->fooname);
			}
		}
	}

	DBIc_NUM_PARAMS(imp_sth) = imp_sth->numphs;

	if (TEND) TRC(DBILOGFP, "%sEnd pg_st_split_statement\n", THEADER);
	return;

} /* end pg_st_split_statement */



/* ================================================================== */
static int pg_st_prepare_statement (pTHX_ SV * sth, imp_sth_t * imp_sth)
{
	D_imp_dbh_from_sth;
	char *       statement;
	unsigned int x;
	STRLEN       execsize;
	PGresult *   result;
	int          status = -1;
	int	     params = 0;
	seg_t *      currseg;
	ph_t *       currph;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_st_prepare_statement\n", THEADER);

	Renew(imp_sth->prepare_name, 256, char); /* freed in dbd_st_destroy */

	/* Name is "dbdpg_xPID_#_xFD", where x is 'p'ositive or 'n'egative */
	snprintf(imp_sth->prepare_name, 256, "dbdpg_%c%d_%d_%c%d",
			(imp_dbh->pid_number < 0 ? 'n' : 'p'),
			abs(imp_dbh->pid_number),
			imp_dbh->prepare_number,
			(imp_dbh->socket_fd < 0 ? 'n' : 'p'),
			abs(imp_dbh->socket_fd));
	imp_sth->prepare_name[255] = 0; // unnecessary paranoia?

	if (TRACE5)
		TRC(DBILOGFP, "%sNew statement name (%s)\n", THEADER, imp_sth->prepare_name);

	execsize = imp_sth->totalsize;

	if (imp_sth->numphs!=0) {
		for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
			if (0==currseg->placeholder)
				continue;
			/* The parameter itself: dollar sign plus digit(s) */
			for (x=1; x<7; x++) {
				if (currseg->placeholder < powf((float)10,(float)x))
					break;
			}
			if (x>=7)
				croak("Too many placeholders!");
			execsize += x+1;
		}
	}

	New(0, statement, execsize+1, char); /* freed below */
	statement[0] = '\0';

	/* Construct the statement, with proper placeholders */
	for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
		if (currseg->segment != NULL)
			strcat(statement, currseg->segment);
		if (currseg->placeholder) {
			sprintf(strchr(statement, '\0'), "$%d", currseg->placeholder);
		}
	}

	statement[execsize] = '\0';

	if (TRACE6)
		TRC(DBILOGFP, "%sPrepared statement (%s)\n", THEADER, statement);

	if (imp_sth->numbound!=0) {
		params = imp_sth->numphs;
		if (NULL == imp_sth->PQoids) {
			Newz(0, imp_sth->PQoids, (unsigned int)imp_sth->numphs, Oid);
		}
		for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph) {
			imp_sth->PQoids[x++] = (currph->defaultval) ? 0 : (Oid)currph->bind_type->type_id;
		}
	}
	if (TSQL)
		TRC(DBILOGFP, "PREPARE %s AS %s;\n\n", imp_sth->prepare_name, statement);

	result = coro_PQprepare(aTHX_ imp_sth, statement, params);
	status = _sqlstate(aTHX_ imp_dbh, result);
	if (result) {
		TRACE_PQCLEAR;
		PQclear(result);
	}
	if (TRACE6)
		TRC(DBILOGFP, "%sUsing PQprepare: %s\n", THEADER, statement);

	Safefree(statement);
	if (PGRES_COMMAND_OK != status) {
		Safefree(imp_sth->prepare_name);
		imp_sth->prepare_name = NULL;
		pg_error(aTHX_ sth, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_st_prepare_statement (error)\n", THEADER);
		return -2;
	}

	imp_sth->prepared_by_us = DBDPG_TRUE; /* Done here so deallocate is not called spuriously */
	imp_dbh->prepare_number++; /* We do this at the end so we don't increment if we fail above */

	if (TEND) TRC(DBILOGFP, "%sEnd pg_st_prepare_statement\n", THEADER);
	return 0;
	
} /* end of pg_st_prepare_statement */



/* ================================================================== */
int dbd_bind_ph (SV * sth, imp_sth_t * imp_sth, SV * ph_name, SV * newvalue, IV sql_type, SV * attribs, int is_inout, IV maxlen)
{
	dTHX;
	D_imp_dbh_from_sth;
	char * name = Nullch;
	STRLEN name_len;
	ph_t * currph = NULL;
	int    x, phnum;
	SV **  svp;
	bool   reprepare = DBDPG_FALSE;
	int    pg_type = 0;
	char * value_string = NULL;
	bool   is_array = DBDPG_FALSE;

	maxlen = 0; /* not used, this makes the compiler happy */

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_bind_ph (ph_name: %s)\n",
					THEADER,
					neatsvpv(ph_name,0));

	if (0==imp_sth->numphs)
		croak("Statement has no placeholders to bind");

	/* Check the placeholder name and transform to a standard form */
	if (SvGMAGICAL(ph_name)) {
		(void)mg_get(ph_name);
	}
	name = SvPV(ph_name, name_len);
	if (3==imp_sth->placeholder_type) {
		if (':' != *name) {
			croak("Placeholders must begin with ':' when using the \":foo\" style");
		}
	}
	else {
		for (x=0; *(name+x); x++) {
			if (!isDIGIT(*(name+x)) && (x!=0 || '$'!=*(name+x))) {
				croak("Placeholder should be in the format \"$1\"\n");
			}
		}
	}

	/* Find the placeholder in question */

	if (3==imp_sth->placeholder_type) {
		for (x=0,currph=imp_sth->ph; NULL != currph; currph = currph->nextph) {
			if (0==strcmp(currph->fooname, name)) {
				x=1;
				break;
			}
		}
		if (0==x)
			croak("Cannot bind unknown placeholder '%s'", name);
	}
	else { /* We have a number */	
		if ('$' == *name)
			name++;
		phnum = atoi(name);
		if (phnum < 1 || phnum > imp_sth->numphs)
			croak("Cannot bind unknown placeholder %d (%s)", phnum, neatsvpv(ph_name,0));
		for (x=1,currph=imp_sth->ph; NULL != currph; currph = currph->nextph,x++) {
			if (x==phnum)
				break;
		}
	}

	/* Check the value */
	if (SvTYPE(newvalue) > SVt_PVLV) { /* hook for later array logic	*/
		croak("Cannot bind a non-scalar value (%s)", neatsvpv(newvalue,0));
	}
	/* dbi handle allowed for cursor variables */
	if ((SvROK(newvalue) &&!IS_DBI_HANDLE(newvalue) &&!SvAMAGIC(newvalue))) {
		if (strnEQ("DBD::Pg::DefaultValue", neatsvpv(newvalue,0), 21)
			|| strnEQ("DBI::DefaultValue", neatsvpv(newvalue,0), 17)) {
			/* This is a special type */
			Safefree(currph->value);
			currph->value = NULL;
			currph->valuelen = 0;
			currph->isdefault = DBDPG_TRUE;
			imp_sth->has_default = DBDPG_TRUE;
		}
		else if (strnEQ("DBD::Pg::Current", neatsvpv(newvalue,0), 16)) {
			/* This is a special type */
			Safefree(currph->value);
			currph->value = NULL;
			currph->valuelen = 0;
			currph->iscurrent = DBDPG_TRUE;
			imp_sth->has_current = DBDPG_TRUE;
		}
		else if (SvTYPE(SvRV(newvalue)) == SVt_PVAV) {
			SV * quotedval;
			quotedval = pg_stringify_array(newvalue,",",imp_dbh->pg_server_version, 0);
			currph->valuelen = sv_len(quotedval);
			Renew(currph->value, currph->valuelen+1, char); /* freed in dbd_st_destroy */
			currph->value = SvUTF8(quotedval) ? SvPVutf8_nolen(quotedval) : SvPV_nolen(quotedval);
			currph->bind_type = pg_type_data(PG_CSTRINGARRAY);
			is_array = DBDPG_TRUE;
		}
		else {
			croak("Cannot bind a reference\n");
		}
	}
	if (TRACE5) {
		TRC(DBILOGFP, "%sBind (%s) (type=%ld)\n", THEADER, name, (long)sql_type);
		if (attribs) {
			TRC(DBILOGFP, "%sBind attribs (%s)", THEADER, neatsvpv(attribs,0));
		}
	}

	if (is_inout) {
		currph->isinout = DBDPG_TRUE;
		imp_sth->use_inout = DBDPG_TRUE;
		currph->inout = newvalue; /* Reference to a scalar */
	}

	/* We ignore attribs for these special cases */
	if (currph->isdefault || currph->iscurrent || is_array) {
		if (NULL == currph->bind_type) {
			imp_sth->numbound++;
			currph->bind_type = pg_type_data(PG_UNKNOWN);
		}
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_bind_ph (special)\n", THEADER);
		return 1;
	}

	/* Check for a pg_type argument (sql_type already handled) */
	if (attribs) {
		if((svp = hv_fetch((HV*)SvRV(attribs),"pg_type", 7, 0)) != NULL)
			pg_type = (int)SvIV(*svp);
	}
	
	if (sql_type && pg_type)
		croak ("Cannot specify both sql_type and pg_type");

	if (NULL == currph->bind_type && (sql_type || pg_type))
		imp_sth->numbound++;
	
	if (pg_type) {
		if ((currph->bind_type = pg_type_data(pg_type))) {
			if (!currph->bind_type->bind_ok) { /* Re-evaluate with new prepare */
				croak("Cannot bind %s, pg_type %s not supported by DBD::Pg",
					  name, currph->bind_type->type_name);
			}
		}
		else {
			croak("Cannot bind %s unknown pg_type %d", name, pg_type);
		}
	}
	else if (sql_type) {
		/* always bind as pg_type, because we know we are 
		   inserting into a pg database... It would make no 
		   sense to quote something to sql semantics and break
		   the insert.
		*/
		if (!(currph->bind_type = sql_type_data((int)sql_type))) {
			croak("Cannot bind param %s: unknown sql_type %ld", name, (long)sql_type);
		}
		if (!(currph->bind_type = pg_type_data(currph->bind_type->type.pg))) {
			croak("Cannot find a pg_type for %ld", (long)sql_type);
		}
	}
	else if (NULL == currph->bind_type) { /* "sticky" data type */
		/* This is the default type, but we will honor defaultval if we can */
		currph->bind_type = pg_type_data(PG_UNKNOWN);
		if (!currph->bind_type)
			croak("Default type is bad!!!!???");
	}

	if (pg_type || sql_type) {
		currph->defaultval = DBDPG_FALSE;
		/* Possible re-prepare, depending on whether the type name also changes */
		if (imp_sth->prepared_by_us && NULL != imp_sth->prepare_name)
			reprepare = DBDPG_TRUE;
		/* Mark this statement as having binary if the type is bytea */
		if (PG_BYTEA==currph->bind_type->type_id)
			imp_sth->has_binary = DBDPG_TRUE;
	}

	/* convert to a string ASAP */
	if (!SvPOK(newvalue) && SvOK(newvalue)) {
		(void)sv_2pv(newvalue, &na);
	}

	/* upgrade to at least string */
	(void)SvUPGRADE(newvalue, SVt_PV);

	if (SvOK(newvalue)) {
		value_string = SvPV(newvalue, currph->valuelen);
		Renew(currph->value, currph->valuelen+1, char); /* freed in dbd_st_destroy */
		Copy(value_string, currph->value, currph->valuelen, char);
		currph->value[currph->valuelen] = '\0';
	}
	else {
		Safefree(currph->value);
		currph->value = NULL;
		currph->valuelen = 0;
	}

	if (reprepare) {
		if (TRACE5)
			TRC(DBILOGFP, "%sBinding has forced a re-prepare\n", THEADER);
		/* Deallocate sets the prepare_name to NULL */
		if (pg_st_deallocate_statement(aTHX_ sth, imp_sth)!=0) {
			/* Deallocation failed. Let's mark it and move on */
			Safefree(imp_sth->prepare_name);
			imp_sth->prepare_name = NULL;
			if (TRACEWARN)
				TRC(DBILOGFP, "%sFailed to deallocate!\n", THEADER);
		}
	}

	if (TRACE7)
		TRC	(DBILOGFP,
			 "%sPlaceholder (%s) bound as type (%s) (type_id=%d), length %d, value of (%s)\n",
			 THEADER, name, currph->bind_type->type_name,
			 currph->bind_type->type_id, (int)currph->valuelen,
			 PG_BYTEA==currph->bind_type->type_id ? "(binary, not shown)" : value_string);

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_bind_ph\n", THEADER);
	return 1;

} /* end of dbd_bind_ph */


/* ================================================================== */
SV * pg_stringify_array(SV *input, const char * array_delim, int server_version, int extraquotes) {

	dTHX;
	AV * toparr;
	AV * currarr;
	AV * lastarr;
	int done;
	int array_depth = 0;
	int array_items;
	int inner_arrays = 0;
	int xy, yz;
	SV * svitem;
	char * string;
	STRLEN svlen;
	SV * value;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_stringify_array\n", THEADER);

	toparr = (AV *) SvRV(input);
	value = extraquotes ? newSVpv("'{", 2) : newSVpv("{", 1);

	/* Empty arrays are easy */
	if (av_len(toparr) < 0) {
		av_clear(toparr);
		sv_catpv(value, "}");
		if (extraquotes)
			sv_catpv(value, "'");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_stringify_array (empty)\n", THEADER);
		return value;
	}

	done = 0;
	currarr = lastarr = toparr;
	while (!done) {
		/* Grab the first item of the current array */
		svitem = *av_fetch(currarr, 0, 0);

		/* If a ref, die if not an array, else keep descending */
		if (SvROK(svitem)) {
			if (SvTYPE(SvRV(svitem)) != SVt_PVAV)
				croak("Arrays must contain only scalars and other arrays");
			array_depth++;

			/* Squirrel away this level before we leave it */
			lastarr = currarr;

			/* Set the current array to this item */
			currarr = (AV *)SvRV(svitem);

			/* If this is an empty array, stop here */
			if (av_len(currarr) < 0)
				done = 1;
		}
		else
			done = 1;
	}

	inner_arrays = array_depth ? 1+(int)av_len(lastarr) : 0;

	/* How many items are in each inner array? */
	array_items = array_depth ? (1+(int)av_len((AV*)SvRV(*av_fetch(lastarr,0,0)))) : 1+(int)av_len(lastarr);

	for (xy=1; xy < array_depth; xy++) {
		sv_catpv(value, "{");
	}

	for (xy=0; xy < inner_arrays || !array_depth; xy++) {
		if (array_depth) {
			svitem = *av_fetch(lastarr, xy, 0);
			if (!SvROK(svitem))
				croak ("Not a valid array!");
			currarr = (AV*)SvRV(svitem);
			if (SvTYPE(currarr) != SVt_PVAV)
				croak("Arrays must contain only scalars and other arrays!");
			if (1+av_len(currarr) != array_items)
				croak("Invalid array - all arrays must be of equal size");
			sv_catpv(value, "{");
		}
		for (yz=0; yz < array_items; yz++) {
			svitem = *av_fetch(currarr, yz, 0);

			if (SvROK(svitem))
				croak("Arrays must contain only scalars and other arrays");

			if (!SvOK(svitem)) { /* Insert NULL if we can */
				/* Only version 8.2 and up can handle NULLs in arrays */
				if (server_version < 80200)
					croak("Cannot use NULLs in arrays until version 8.2");
				sv_catpv(value, "NULL"); /* Beware of array_nulls config param! */
			}
			else {
				sv_catpv(value, "\"");
				if (SvUTF8(svitem))
					SvUTF8_on(value);
				string = SvPV(svitem, svlen);
				while (svlen--) {

					/* If an embedded quote, throw a backslash before it */
					if ('\"' == *string)
						sv_catpvn(value, "\\", 1);
					/* If a backslash, double it up */
					if ('\\' == *string) {
						sv_catpvn(value, "\\\\\\", 3);
					}
					sv_catpvn(value, string, 1);
					string++;
				}
				sv_catpv(value, "\"");
			}

			if (yz < array_items-1)
				sv_catpv(value, array_delim);
		}

		if (!array_items) {
			sv_catpv(value, "\"\"");
		}

		sv_catpv(value, "}");
		if (xy < inner_arrays-1)
			sv_catpv(value, array_delim);
		if (!array_depth)
			break;
	}

	for (xy=0; xy<array_depth; xy++) {
		sv_catpv(value, "}");
	}
	if (extraquotes)
		sv_catpv(value, "'");

	if (TEND) TRC(DBILOGFP, "%sEnd pg_stringify_array (string: %s)\n", THEADER, neatsvpv(value,0));
	return value;

} /* end of pg_stringify_array */

/* ================================================================== */
static SV * pg_destringify_array(pTHX_ imp_dbh_t *imp_dbh, unsigned char * input, sql_type_info_t * coltype)
{

	AV*    av;              /* The main array we are returning a reference to */
	AV*    currentav;       /* The current array level */
	AV*    topav;           /* Where each item starts at */
	char*  string;
	STRLEN section_size = 0;
	bool   in_quote = 0;
	bool   seen_quotes = 0;
	int    opening_braces = 0;
	int    closing_braces = 0;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_destringify_array (string: %s quotechar: %c)\n",
					THEADER, input, coltype->array_delimeter);

	/*
	  Note: we don't do careful balance checking here, as this is coming straight from 
	  the Postgres backend, and we rely on it to give us a sane and balanced structure
	*/

	/* The array may start with a non 1-based beginning. If so, we'll just eat the range */
	if ('[' == *input) {
		while (*input != '\0') {
			if ('=' == *input++)
				break;
		}
	}

	/* Eat the opening brace and perform a sanity check */
	if ('{' != *(input++))
		croak("Tried to destringify a non-array!: %s", input);

	/* Count how deep this array goes */
	while ('{' == *input) {
		opening_braces++;
		input++;
	}
	input -= opening_braces;

	New(0, string, strlen((char *)input), char); /* Freed at end of this function */
	string[0] = '\0';

	av = currentav = topav = newAV();

	while (*input != '\0') {
		if (in_quote) {
			if ('"' == *input) {
				in_quote = 0;
				/* String will be stored by following delim or brace */
				input++;
				continue;
			}
			if ('\\' == *input) { /* Eat backslashes */
				input++;
			}
			string[section_size++] = *input++;
			continue;
		}
		else if ('{' == *input) {
			AV * const newav = newAV();
			av_push(currentav, newRV_noinc((SV*)newav));
			currentav = newav;
		}
		else if (coltype->array_delimeter == *input) {
		}
		else if ('}' == *input) {
		}
		else if ('"' == *input) {
			in_quote = seen_quotes = (bool)1;
		}
		else {
			string[section_size++] = *input;
		}

		if ('}' == *input || (coltype->array_delimeter == *input && '}' != *(input-1))) {
			string[section_size] = '\0';
			if (0 == section_size && !seen_quotes) {
				/* Just an empty array */
			}
			else if (4 == section_size && 0 == strncmp(string, "NULL", 4) && '"' != *(input-1)) {
				av_push(currentav, &PL_sv_undef);
			}
			else {
				if (1 == coltype->svtype)
					av_push(currentav, newSViv(SvIV(sv_2mortal(newSVpvn(string,section_size)))));
				else if (2 == coltype->svtype)
					av_push(currentav, newSVnv(SvNV(sv_2mortal(newSVpvn(string,section_size)))));
				else if (3 == coltype->svtype)
					av_push(currentav, newSViv('t' == *string ? 1 : 0));
				else {
					SV *sv = newSVpvn(string, section_size);
#ifdef is_utf8_string
					if (imp_dbh->pg_enable_utf8) {
						SvUTF8_off(sv);
						if (is_high_bit_set(aTHX_ (unsigned char *)string, section_size) && is_utf8_string((unsigned char*)string, section_size)) {
							SvUTF8_on(sv);
						}
					}
#endif
					av_push(currentav, sv);

				}
			}
			section_size = 0;
		}

		/* Handle all touching closing braces */
		if ('}' == *input) {
			if (closing_braces) {
				while ('}' == *input) {
					input++;
				}
			}
			else {
				while ('}' == *input) {
					closing_braces++;
					input++;
				}
				/* Set the new topav if required */
				if ('\0' != *input && opening_braces > closing_braces) {
					closing_braces = opening_braces - closing_braces;
					while (closing_braces--) {
						topav = (AV*)SvRV(AvARRAY(topav)[0]);
					}
				}
			}
			currentav = topav;
		}
		else {
			input++;
		}
	}
	Safefree(string);

	if (TEND) TRC(DBILOGFP, "%sEnd pg_destringify_array\n", THEADER);
	return newRV_noinc((SV*)av);

} /* end of pg_destringify_array */


/* ================================================================== */
int pg_quickexec (SV * dbh, const char * sql, const int asyncflag)
{
	dTHX;
	D_imp_dbh(dbh);
	PGresult *              result;
	ExecStatusType          status = PGRES_FATAL_ERROR; /* Assume the worst */
	PGTransactionStatusType txn_status;
	char *                  cmdStatus = NULL;
	int                     rows = 0;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_quickexec (query: %s async: %d async_status: %d)\n",
			THEADER, sql, asyncflag, imp_dbh->async_status);

	if (NULL == imp_dbh->conn)
		croak("execute on disconnected handle");

	/* Abort if we are in the middle of a copy */
	if (imp_dbh->copystate != 0) {
		if (PGRES_COPY_IN == imp_dbh->copystate) {
			croak("Must call pg_putcopyend before issuing more commands");
		}
		else {
			croak("Must call pg_getcopydata until no more rows before issuing more commands");
		}
	}			

	/* If we are still waiting on an async, handle it */
	if (imp_dbh->async_status) {
		if (TRACE5) TRC(DBILOGFP, "%shandling old async\n", THEADER);
		rows = handle_old_async(aTHX_ dbh, imp_dbh, asyncflag);
		if (rows) {
			if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (async rows: %d)\n", THEADER, rows);
			return rows;
		}
	}

	/* If not autocommit, start a new transaction */
	if (!imp_dbh->done_begin && !DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		status = _result(aTHX_ imp_dbh, "begin");
		if (PGRES_COMMAND_OK != status) {
			pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
			if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (error: begin failed)\n", THEADER);
			return -2;
		}
		imp_dbh->done_begin = DBDPG_TRUE;
		/* If read-only mode, make it so */
		if (imp_dbh->txn_read_only) {
			status = _result(aTHX_ imp_dbh, "set transaction read only");
			if (PGRES_COMMAND_OK != status) {
				pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
				if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (error: set transaction read only failed)\n", THEADER);
				return -2;
			}
		}
	}

	/*
	  We want txn mode if AutoCommit
	 */


	/* Asynchronous commands get kicked off and return undef */
	if (asyncflag & PG_ASYNC) {
		if (TRACE4) TRC(DBILOGFP, "%sGoing asychronous with do()\n", THEADER);
		TRACE_PQSENDQUERY;
		if (! PQsendQuery(imp_dbh->conn, sql)) {
			if (TRACE4) TRC(DBILOGFP, "%sPQsendQuery failed\n", THEADER);
			pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
			if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (error: async do failed)\n", THEADER);
			return -2;
		}
		imp_dbh->async_status = 1;
		imp_dbh->async_sth = NULL; /* Needed? */

		if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (async)\n", THEADER);
		return 0;
	}

	if (TSQL) TRC(DBILOGFP, "%s;\n\n", sql);

	result = coro_PQexec(aTHX_ imp_dbh, sql);
	status = _sqlstate(aTHX_ imp_dbh, result);

	imp_dbh->copystate = 0; /* Assume not in copy mode until told otherwise */

	if (TRACE4) TRC(DBILOGFP, "%sGot a status of %d\n", THEADER, status);
	switch (status) {
	case PGRES_TUPLES_OK:
		TRACE_PQNTUPLES;
		rows = PQntuples(result);
		break;
	case PGRES_COMMAND_OK:
		/* non-select statement */
		TRACE_PQCMDSTATUS;
		cmdStatus = PQcmdStatus(result);
		/* If the statement indicates a number of rows, we want to return that */
		/* Note: COPY and FETCH do not currently reach here, although they return numbers */
		if (0 == strncmp(cmdStatus, "INSERT", 6)) {
			/* INSERT(space)oid(space)numrows */
			for (rows=8; cmdStatus[rows-1] != ' '; rows++) {
			}
			rows = atoi(cmdStatus + rows);
		}
		else if (0 == strncmp(cmdStatus, "MOVE", 4)) {
			rows = atoi(cmdStatus + 5);
		}
		else if (0 == strncmp(cmdStatus, "DELETE", 6)
			 || 0 == strncmp(cmdStatus, "UPDATE", 6)) {
			rows = atoi(cmdStatus + 7);
		}
		break;
	case PGRES_COPY_OUT:
	case PGRES_COPY_IN:
		/* Copy Out/In data transfer in progress */
		imp_dbh->copystate = status;
		rows = -1;
		break;
	case PGRES_EMPTY_QUERY:
	case PGRES_BAD_RESPONSE:
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
	default:
		rows = -2;
		pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
		break;
	}

	if (result) {
		TRACE_PQCLEAR;
		PQclear(result);
	}
	else {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (no result)\n", THEADER);
		return -2;
	}

	TRACE_PQTRANSACTIONSTATUS;
	txn_status = PQtransactionStatus(imp_dbh->conn);

	if (PQTRANS_IDLE == txn_status) {
		imp_dbh->done_begin = DBDPG_FALSE;
		imp_dbh->copystate=0;
		/* If begin_work has been called, turn AutoCommit back on and BegunWork off */
		if (DBIc_has(imp_dbh, DBIcf_BegunWork)!=0) {
			DBIc_set(imp_dbh, DBIcf_AutoCommit, 1);
			DBIc_set(imp_dbh, DBIcf_BegunWork, 0);
		}
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (rows: %d, txn_status: %d)\n",
				  THEADER, rows, txn_status);
	return rows;

} /* end of pg_quickexec */


/* ================================================================== */
/* Return value <= -2:error, >=0:ok row count, (-1=unknown count) */
int dbd_st_execute (SV * sth, imp_sth_t * imp_sth)
{
	dTHX;
	D_imp_dbh_from_sth;
	ph_t *        currph;
	int           status = -1;
	STRLEN        execsize, x;
	seg_t *       currseg;
	char *        statement = NULL;
	int           num_fields;
	int           ret = -2;
	
	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_execute\n", THEADER);
	
	if (NULL == imp_dbh->conn)
		croak("execute on disconnected handle");

	/* Abort if we are in the middle of a copy */
	if (imp_dbh->copystate!=0)
		croak("Must call pg_endcopy before issuing more commands");

	/* Ensure that all the placeholders have been bound */
	if (!imp_sth->all_bound && imp_sth->numphs!=0) {
		for (currph=imp_sth->ph; NULL != currph; currph=currph->nextph) {
			if (NULL == currph->bind_type) {
				pg_error(aTHX_ sth, PGRES_FATAL_ERROR, "execute called with an unbound placeholder");
				if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (error: unbound placeholder)\n", THEADER);
				return -2;
			}
			if (currph->isinout) {
				currph->valuelen = sv_len(currph->inout);
				Renew(currph->value, currph->valuelen+1, char);
				Copy(SvPV_nolen(currph->inout), currph->value, currph->valuelen, char);
				currph->value[currph->valuelen] = '\0';
			}
		}
		imp_sth->all_bound = DBDPG_TRUE;
	}

	/* Check for old async transactions */
	if (imp_dbh->async_status) {
		if (TRACE7) TRC(DBILOGFP, "%sAttempting to handle existing async transaction\n", THEADER);
		ret = handle_old_async(aTHX_ sth, imp_dbh, imp_sth->async_flag);
		if (ret) {
			if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (async ret: %d)\n", THEADER, ret);
			return ret;
		}
	}

	/* If not autocommit, start a new transaction */
	if (!imp_dbh->done_begin && !DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		status = _result(aTHX_ imp_dbh, "begin");
		if (PGRES_COMMAND_OK != status) {
			pg_error(aTHX_ sth, status, _error_message(imp_dbh));
			if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (error: begin failed)\n", THEADER);
			return -2;
		}
		imp_dbh->done_begin = DBDPG_TRUE;
		/* If read-only mode, make it so */
		if (imp_dbh->txn_read_only) {
			status = _result(aTHX_ imp_dbh, "set transaction read only");
			if (PGRES_COMMAND_OK != status) {
				pg_error(aTHX_ sth, status, _error_message(imp_dbh));
				if (TEND) TRC(DBILOGFP, "%sEnd pg_quickexec (error: set transaction read only failed)\n", THEADER);
				return -2;
			}
		}
	}

	/* clear old result (if any) */
	if (imp_sth->result) {
		TRACE_PQCLEAR;
		PQclear(imp_sth->result);
		imp_sth->result = NULL;
	}

	/*
	  Now, we need to build the statement to send to the backend
	  We are using one of PQexec, PQexecPrepared, or PQexecParams
	  First, we figure out the size of the statement...
	*/

	execsize = imp_sth->totalsize; /* Total of all segments */

	/* If using plain old PQexec, we need to quote each value ourselves */
	if (!imp_sth->is_dml
		|| imp_sth->has_default
		|| imp_sth->has_current
		|| (1 != imp_sth->server_prepare
			&& imp_sth->numbound != imp_sth->numphs)
		) {
		for (currph=imp_sth->ph; NULL != currph; currph=currph->nextph) {
			if (currph->isdefault) {
				Renew(currph->quoted, 8, char); /* freed in dbd_st_destroy */
				strncpy(currph->quoted, "DEFAULT", 8);
				currph->quotedlen = 7;
			}
			else if (currph->iscurrent) {
				Renew(currph->quoted, 18, char); /* freed in dbd_st_destroy */
				strncpy(currph->quoted, "CURRENT_TIMESTAMP", 18);
				currph->quotedlen = 17;
			}
			else if (NULL == currph->value) {
				Renew(currph->quoted, 5, char); /* freed in dbd_st_destroy */
				strncpy(currph->quoted, "NULL", 5);
				currph->quotedlen = 4;
			}
			else {
				if (currph->quoted)
					Safefree(currph->quoted);
				currph->quoted = currph->bind_type->quote(
					currph->value,
					currph->valuelen,
					&currph->quotedlen,
					imp_dbh->pg_server_version >= 80100 ? 1 : 0
														  ); /* freed in dbd_st_destroy */
			}
		}
		/* Set the size of each actual in-place placeholder */
		for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
			if (currseg->placeholder!=0)
				execsize += currseg->ph->quotedlen;
		}
	}
	else { /* We are using a server that can handle PQexecParams/PQexecPrepared */
		/* Put all values into an array to pass to PQexecPrepared */
		if (NULL == imp_sth->PQvals) {
			Newz(0, imp_sth->PQvals, (unsigned int)imp_sth->numphs, const char *); /* freed in dbd_st_destroy */
		}
		for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph) {
			imp_sth->PQvals[x++] = currph->value;
		}

		/* Binary or regular? */

		if (imp_sth->has_binary) {
			if (NULL == imp_sth->PQlens) {
				Newz(0, imp_sth->PQlens, (unsigned int)imp_sth->numphs, int); /* freed in dbd_st_destroy */
				Newz(0, imp_sth->PQfmts, (unsigned int)imp_sth->numphs, int); /* freed below */
			}
			for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,x++) {
				if (PG_BYTEA==currph->bind_type->type_id) {
					imp_sth->PQlens[x] = (int)currph->valuelen;
					imp_sth->PQfmts[x] = 1;
				}
				else {
					imp_sth->PQlens[x] = 0;
					imp_sth->PQfmts[x] = 0;
				}
			}
		}
	}
	
	/* We use the new server_side prepare style if:
	   1. The statement is DML (DDL is not preparable)
	   2. The attribute "pg_direct" is false
	   3. The attribute "pg_server_prepare" is not 0
	   4. The "onetime" attribute has not been set
	   5. There are no DEFAULT or CURRENT values
	   6a. The attribute "pg_server_prepare" is 1
	   OR
	   6b. All placeholders are bound (and "pg_server_prepare" is 2)
	*/
	if (TRACE4) TRC(DBILOGFP,
					"%sPQexec* decision: dml=%d direct=%d server_prepare=%d numbound=%d numphs=%d default=%d\n",
					THEADER, 
					imp_sth->is_dml,
					imp_sth->direct,
					imp_sth->server_prepare,
					imp_sth->numbound,
					imp_sth->numphs,
					imp_sth->has_default);

	if (imp_sth->is_dml
		&& !imp_sth->direct
		&& 0 != imp_sth->server_prepare
		&& !imp_sth->has_default
		&& !imp_sth->has_current
		&& !imp_sth->onetime
		&& (1 == imp_sth->server_prepare
			|| (imp_sth->numbound == imp_sth->numphs))
		){
	
		if (TRACE4) TRC(DBILOGFP, "%sPQexecPrepared\n", THEADER);

		/* Prepare if it has not already been prepared (or it needs repreparing) */
		if (NULL == imp_sth->prepare_name) {
			if (imp_sth->prepared_by_us) {
				if (TRACE5) TRC(DBILOGFP, "%sRe-preparing statement\n", THEADER);
			}
			if (pg_st_prepare_statement(aTHX_ sth, imp_sth)!=0) {
				if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (error)\n", THEADER);
				return -2;
			}
		}
		else {
			if (TRACE5) TRC(DBILOGFP, "%sUsing previously prepared statement (%s)\n",
							THEADER, imp_sth->prepare_name);
		}
		
		if (TRACE7) {
			for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,x++) {
				TRC(DBILOGFP, "%sPQexecPrepared item #%d\n", THEADER, (int)x);
				TRC(DBILOGFP, "%s-> Value: (%s)\n",
					THEADER, (imp_sth->PQfmts && imp_sth->PQfmts[x]==1) ? "(binary, not shown)" 
									: imp_sth->PQvals[x]);
				TRC(DBILOGFP, "%s-> Length: (%d)\n", THEADER, imp_sth->PQlens ? imp_sth->PQlens[x] : 0);
				TRC(DBILOGFP, "%s-> Format: (%d)\n", THEADER, imp_sth->PQfmts ? imp_sth->PQfmts[x] : 0);
			}
		}
		
		if (TRACE5) TRC(DBILOGFP, "%sRunning PQexecPrepared with (%s) (%s)\n",
						THEADER, imp_sth->prepare_name, (char *)imp_sth->PQvals);
		if (TSQL) {
			TRC(DBILOGFP, "EXECUTE %s (\n", imp_sth->prepare_name);
			for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,x++) {
				TRC(DBILOGFP, "$%d: %s\n", (int)x+1, imp_sth->PQvals[x]);
			}
			TRC(DBILOGFP, ");\n\n");
		}

		if (imp_sth->async_flag & PG_ASYNC) {
			TRACE_PQSENDQUERYPREPARED;
			ret = PQsendQueryPrepared
				(imp_dbh->conn, imp_sth->prepare_name, imp_sth->numphs, imp_sth->PQvals, imp_sth->PQlens, imp_sth->PQfmts, 0);
		}
		else {
			imp_sth->result = coro_PQexecPrepared(aTHX_ imp_sth);
		}
	} /* end new-style prepare */
	else {
		
		/* prepare via PQexec or PQexecParams */


		/* PQexecParams */

		if (imp_sth->is_dml
			&& imp_sth->numphs
			&& !imp_sth->has_default
			&& !imp_sth->has_current
			&& (1 == imp_sth->server_prepare || imp_sth->numbound == imp_sth->numphs)
			) {

			if (TRACE5) TRC(DBILOGFP, "%sPQexecParams\n", THEADER);

			/* Figure out how big the statement plus placeholders will be */
			for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
				if (0==currseg->placeholder)
					continue;
				/* The parameter itself: dollar sign plus digit(s) */
				for (x=1; x<7; x++) {
					if (currseg->placeholder < powf((float)10,(float)x))
						break;
				}
				if (x>=7)
					croak("Too many placeholders!");
				execsize += x+1;
			}

			/* Create the statement */
			New(0, statement, execsize+1, char); /* freed below */
			statement[0] = '\0';
			for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
				if (currseg->segment != NULL)
					strcat(statement, currseg->segment);
				if (currseg->placeholder!=0)
					sprintf(strchr(statement, '\0'), "$%d", currseg->placeholder);
			}
			statement[execsize] = '\0';
			
			/* Populate PQoids */
			if (NULL == imp_sth->PQoids) {
				Newz(0, imp_sth->PQoids, (unsigned int)imp_sth->numphs, Oid);
			}
			for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph) {
				imp_sth->PQoids[x++] = (currph->defaultval) ? 0 : (Oid)currph->bind_type->type_id;
			}
		
			if (TRACE7) {
				for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,x++) {
					TRC(DBILOGFP, "%sPQexecParams item #%d\n", THEADER, (int)x);
					TRC(DBILOGFP, "%s-> Type: (%d)\n", THEADER, imp_sth->PQoids[x]);
					TRC(DBILOGFP, "%s-> Value: (%s)\n", THEADER, imp_sth->PQvals[x]);
					TRC(DBILOGFP, "%s-> Length: (%d)\n", THEADER, imp_sth->PQlens ? imp_sth->PQlens[x] : 0);
					TRC(DBILOGFP, "%s-> Format: (%d)\n", THEADER, imp_sth->PQfmts ? imp_sth->PQfmts[x] : 0);
				}
			}

			if (TSQL) {
				TRC(DBILOGFP, "EXECUTE %s (\n", statement);
				for (x=0,currph=imp_sth->ph; NULL != currph; currph=currph->nextph,x++) {
					TRC(DBILOGFP, "$%d: %s\n", (int)x+1, imp_sth->PQvals[x]);
				}
				TRC(DBILOGFP, ");\n\n");
			}

			if (TRACE5) TRC(DBILOGFP, "%sRunning PQexecParams with (%s)\n", THEADER, statement);
			if (imp_sth->async_flag & PG_ASYNC) {
				TRACE_PQSENDQUERYPARAMS;
				ret = PQsendQueryParams
					(imp_dbh->conn, statement, imp_sth->numphs, imp_sth->PQoids, imp_sth->PQvals, imp_sth->PQlens, imp_sth->PQfmts, 0);
			}
			else {
				imp_sth->result = coro_PQexecParams(aTHX_ imp_sth, statement);
			}
		}
		
		/* PQexec */

		else {

			if (TRACE5) TRC(DBILOGFP, "%sPQexec\n", THEADER);

			/* Go through and quote each value, then turn into a giant statement */
			for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
				if (currseg->placeholder!=0)
					execsize += currseg->ph->quotedlen;
			}

			New(0, statement, execsize+1, char); /* freed below */
			statement[0] = '\0';
			for (currseg=imp_sth->seg; NULL != currseg; currseg=currseg->nextseg) {
				if (currseg->segment != NULL)
					strcat(statement, currseg->segment);
				if (currseg->placeholder!=0)
					strcat(statement, currseg->ph->quoted);
			}
			statement[execsize] = '\0';

			if (TRACE5) TRC(DBILOGFP, "%sRunning %s with (%s)\n", 
							THEADER, imp_sth->async_flag & 1 ? "PQsendQuery" : "PQexec", statement);
			
			if (TSQL)
				TRC(DBILOGFP, "%s;\n\n", statement);

			if (imp_sth->async_flag & PG_ASYNC) {
				TRACE_PQSENDQUERY;
				ret = PQsendQuery(imp_dbh->conn, statement);
			}
			else {
				imp_sth->result = coro_PQexec(aTHX_ imp_dbh, statement);
			}

		} /* end PQexec */

		Safefree(statement);

	} /* end non-prepared exec */

	/* Some form of PQexec/PQsendQuery has been run at this point */

	/* If running asynchronously, we don't stick around for the result */
	if (imp_sth->async_flag & PG_ASYNC) {
		if (TRACEWARN) TRC(DBILOGFP, "%sEarly return for async query", THEADER);
		imp_dbh->async_status = 1;
		imp_sth->async_status = 1;
		imp_dbh->async_sth = imp_sth;
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (async)\n", THEADER);
		return 0;
	}

	status = _sqlstate(aTHX_ imp_dbh, imp_sth->result);

	imp_dbh->copystate = 0; /* Assume not in copy mode until told otherwise */
	if (PGRES_TUPLES_OK == status) {
		TRACE_PQNFIELDS;
		num_fields = PQnfields(imp_sth->result);
		imp_sth->cur_tuple = 0;
		DBIc_NUM_FIELDS(imp_sth) = num_fields;
		DBIc_ACTIVE_on(imp_sth);
		TRACE_PQNTUPLES;
		ret = PQntuples(imp_sth->result);
		if (TRACE5) TRC(DBILOGFP,
						"%sStatus was PGRES_TUPLES_OK, fields=%d, tuples=%d\n",
						THEADER, num_fields, ret);
	}
	else if (PGRES_COMMAND_OK == status) {
		/* non-select statement */
		char *cmdStatus = NULL;
		bool gotrows = DBDPG_FALSE;

		if (TRACE5)
			TRC(DBILOGFP, "%sStatus was PGRES_COMMAND_OK\n", THEADER);

		if (imp_sth->result) {
			TRACE_PQCMDSTATUS;
			cmdStatus = PQcmdStatus(imp_sth->result);
			if (0 == strncmp(cmdStatus, "INSERT", 6)) {
				/* INSERT(space)oid(space)numrows */
				for (ret=8; cmdStatus[ret-1] != ' '; ret++) {
				}
				ret = atoi(cmdStatus + ret);
				gotrows = DBDPG_TRUE;
			}
			else if (0 == strncmp(cmdStatus, "MOVE", 4)) {
				ret = atoi(cmdStatus + 5);
				gotrows = DBDPG_TRUE;
			}
			else if (0 == strncmp(cmdStatus, "DELETE", 6)
					 || 0 == strncmp(cmdStatus, "UPDATE", 6)) {
				ret = atoi(cmdStatus + 7);
				gotrows = DBDPG_TRUE;
			}
		}
		if (!gotrows) {
			/* No rows affected, but check for change of state */
			TRACE_PQTRANSACTIONSTATUS;
			if (PQTRANS_IDLE == PQtransactionStatus(imp_dbh->conn)) {
				imp_dbh->done_begin = DBDPG_FALSE;
				/* If begin_work has been called, turn AutoCommit back on and BegunWork off */
				if (DBIc_has(imp_dbh, DBIcf_BegunWork)!=0) {
					DBIc_set(imp_dbh, DBIcf_AutoCommit, 1);
					DBIc_set(imp_dbh, DBIcf_BegunWork, 0);
				}
			}
			if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (OK, no rows)\n", THEADER);
			return 0;
		}
	}
	else if (PGRES_COPY_OUT == status || PGRES_COPY_IN == status) {
		if (TRACE5)
			TRC(DBILOGFP, "%sStatus was PGRES_COPY_%s\n",
				THEADER, PGRES_COPY_OUT == status ? "OUT" : "IN");
		/* Copy Out/In data transfer in progress */
		imp_dbh->copystate = status;
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (COPY)\n", THEADER);
		return -1;
	}
	else {
		if (TRACE5) TRC(DBILOGFP, "%sInvalid status returned (%d)\n", THEADER, status);
		pg_error(aTHX_ sth, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (error: bad status)\n", THEADER);
		return -2;
	}
	
	/* store the number of affected rows */
	
	imp_sth->rows = ret;

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_execute (rows: %d)\n", THEADER, ret);
	return ret;

} /* end of dbd_st_execute */


/* ================================================================== */
static int is_high_bit_set(pTHX_ const unsigned char * val, STRLEN size)
{
	if (TSTART) TRC(DBILOGFP, "%sBegin is_high_bit_set\n", THEADER);
	while (*val && size--)
		if (*val++ & 0x80) return 1;
	return 0;
}


/* ================================================================== */
AV * dbd_st_fetch (SV * sth, imp_sth_t * imp_sth)
{
	dTHX;
	D_imp_dbh_from_sth;
	sql_type_info_t * type_info;
	int               num_fields;
	unsigned char *   value;
	char *            p;
	int               i;
	int               chopblanks;
	STRLEN            value_len = 0;
	STRLEN            len;
	AV *              av;
	
	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_fetch\n", THEADER);

	/* Check that execute() was executed successfully */
	if ( !DBIc_ACTIVE(imp_sth) ) {
		pg_error(aTHX_ sth, PGRES_NONFATAL_ERROR, "no statement executing\n");	
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_fetch (error: no statement)\n", THEADER);
		return Nullav;
	}
	
	TRACE_PQNTUPLES;
	if (imp_sth->cur_tuple == imp_sth->rows) {
		if (TRACE5)
			TRC(DBILOGFP, "%sFetched the last tuple (%d)\n", THEADER, imp_sth->cur_tuple);
		imp_sth->cur_tuple = 0;
		DBIc_ACTIVE_off(imp_sth);
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_fetch (last tuple)\n", THEADER);
		return Nullav; /* we reached the last tuple */
	}

	av = DBIS->get_fbav(imp_sth);
	num_fields = AvFILL(av)+1;
	
	chopblanks = (int)DBIc_has(imp_sth, DBIcf_ChopBlanks);

	/* Set up the type_info array if we have not seen it yet */
	if (NULL == imp_sth->type_info) {
		Newz(0, imp_sth->type_info, (unsigned int)num_fields, sql_type_info_t*); /* freed in dbd_st_destroy */
		for (i = 0; i < num_fields; ++i) {
			TRACE_PQFTYPE;
			imp_sth->type_info[i] = pg_type_data((int)PQftype(imp_sth->result, i));
			if (imp_sth->type_info[i] == NULL) {
				if (TRACEWARN) {
					TRACE_PQFTYPE;
					TRC(DBILOGFP, "%sUnknown type returned by Postgres: %d. Setting to UNKNOWN\n",
						THEADER, PQftype(imp_sth->result, i));
				}
				imp_sth->type_info[i] = pg_type_data(PG_UNKNOWN);
			}
		}
	}
	
	for (i = 0; i < num_fields; ++i) {
		SV *sv;

		if (TRACE5)
			TRC(DBILOGFP, "%sFetching field #%d\n", THEADER, i);

		sv = AvARRAY(av)[i];

		TRACE_PQGETISNULL;
		if (PQgetisnull(imp_sth->result, imp_sth->cur_tuple, i)!=0) {
			SvROK(sv) ? (void)sv_unref(sv) : (void)SvOK_off(sv);
		}
		else {
			TRACE_PQGETVALUE;
			value = (unsigned char*)PQgetvalue(imp_sth->result, imp_sth->cur_tuple, i); 

			type_info = imp_sth->type_info[i];

			if (type_info
				&& 0 == strncmp(type_info->arrayout, "array", 5)
				&& imp_dbh->expand_array) {
				sv_replace(sv, pg_destringify_array(aTHX_ imp_dbh, value, type_info));
			}
			else {
				if (type_info) {
					type_info->dequote(value, &value_len); /* dequote in place */
					/* For certain types, we can cast to non-string Perlish values */
					switch (type_info->type_id) {
					case PG_BOOL:
						if (imp_dbh->pg_bool_tf) {
							*value = ('1' == *value) ? 't' : 'f';
							sv_setpvn(sv, (char *)value, value_len);
						}
						else
							sv_setiv(sv, '1' == *value ? 1 : 0);
						break;
					case PG_OID:
					case PG_INT4:
					case PG_INT2:
						sv_setiv(sv, atol((char *)value));
						break;
					default:
						sv_setpvn(sv, (char *)value, value_len);
					}
				}
				else {
					value_len = strlen((char *)value);
					sv_setpvn(sv, (char *)value, value_len);
				}
			
				if (type_info && (PG_BPCHAR == type_info->type_id) && chopblanks) {
					p = SvEND(sv);
					len = SvCUR(sv);
					while(len && ' ' == *--p)
						--len;
					if (len != SvCUR(sv)) {
						SvCUR_set(sv, len);
						*SvEND(sv) = '\0';
					}
				}
			}
#ifdef is_utf8_string
			if (imp_dbh->pg_enable_utf8 && type_info) {
				SvUTF8_off(sv);
				switch (type_info->type_id) {
				case PG_CHAR:
				case PG_TEXT:
				case PG_BPCHAR:
				case PG_VARCHAR:
					if (is_high_bit_set(aTHX_ value, value_len) && is_utf8_string((unsigned char*)value, value_len)) {
						SvUTF8_on(sv);
					}
					break;
				default:
					break;
				}
			}
#endif
		}
	}
	
	imp_sth->cur_tuple += 1;

	/* Experimental inout support */
	if (imp_sth->use_inout) {
		ph_t *currph;
		for (i=0,currph=imp_sth->ph; NULL != currph && i < num_fields; currph=currph->nextph,i++) {
			if (currph->isinout) {
				/* When we have Perl 5.7.3 or better as a pre-req:
				   sv_copypv(currph->inout, AvARRAY(av)[i]);
				*/
				const char * const s = SvPV(AvARRAY(av)[i],len);
				sv_setpvn(currph->inout, s, len);
				if (SvUTF8(AvARRAY(av)[i]))
					SvUTF8_on(currph->inout);
				else
					SvUTF8_off(currph->inout);
			}
		}
	}

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_fetch\n", THEADER);
	return av;

} /* end of dbd_st_fetch */


/* ================================================================== */
/* Pop off savepoints to the specified savepoint name */
static void pg_db_free_savepoints_to (pTHX_ imp_dbh_t * imp_dbh, const char *savepoint)
{
	I32 i;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_free_savepoints_to\n", THEADER);

	for (i = av_len(imp_dbh->savepoints); i >= 0; i--) {
		SV * const elem = av_pop(imp_dbh->savepoints);
		if (strEQ(SvPV_nolen(elem), savepoint)) {
			sv_2mortal(elem);
			break;
		}
		sv_2mortal(elem);
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_free_savepoints_to\n", THEADER);
}


/* ================================================================== */
int dbd_st_rows (SV * sth, imp_sth_t * imp_sth)
{
	dTHX;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_rows\n", THEADER);

	return imp_sth->rows;

} /* end of dbd_st_rows */


/* ================================================================== */
int dbd_st_finish (SV * sth, imp_sth_t * imp_sth)
{	
	dTHX;
	D_imp_dbh_from_sth;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbdpg_finish (async: %d)\n",
					THEADER, imp_dbh->async_status);
	
	if (DBIc_ACTIVE(imp_sth) && imp_sth->result) {
		TRACE_PQCLEAR;
		PQclear(imp_sth->result);
		imp_sth->result = NULL;
		imp_sth->rows = 0;
	}
	
	/* Are we in the middle of an async for this statement handle? */
	if (imp_dbh->async_status) {
		if (imp_sth->async_status) {
			handle_old_async(aTHX_ sth, imp_dbh, PG_OLDQUERY_WAIT);
		}
	}

	imp_sth->async_status = 0;
	imp_dbh->async_sth = NULL;

	DBIc_ACTIVE_off(imp_sth);
	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_finish\n", THEADER);
	return 1;

} /* end of dbd_st_finish */


/* ================================================================== */
static int pg_st_deallocate_statement (pTHX_ SV * sth, imp_sth_t * imp_sth)
{
	D_imp_dbh_from_sth;
	char                    tempsqlstate[6];
	char *                  stmt;
	int                     status;
	PGTransactionStatusType tstatus;
	
	if (TSTART) TRC(DBILOGFP, "%sBegin pg_st_deallocate_statement\n", THEADER);

	if (NULL == imp_dbh->conn || NULL == imp_sth->prepare_name) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_st_deallocate_statement (0)\n", THEADER);
		return 0;
	}
	
	tempsqlstate[0] = '\0';

	/* What is our status? */
	tstatus = pg_db_txn_status(aTHX_ imp_dbh);
	if (TRACE5)
		TRC(DBILOGFP, "%stxn_status is %d\n", THEADER, tstatus);

	/* If we are in a failed transaction, rollback before deallocating */
	if (PQTRANS_INERROR == tstatus) {
		if (TRACE4)
			TRC(DBILOGFP, "%sIssuing rollback before deallocate\n", THEADER);
		{
			/* If a savepoint has been set, rollback to the last savepoint instead of the entire transaction */
			I32	alen = av_len(imp_dbh->savepoints);
			if (alen > -1) {
				char	*cmd;
				SV * const sp = *av_fetch(imp_dbh->savepoints, alen, 0);
				New(0, cmd, SvLEN(sp) + 13, char); /* Freed below */
				if (TRACE4)
					TRC(DBILOGFP, "%sRolling back to savepoint %s\n", THEADER, SvPV_nolen(sp));
				sprintf(cmd, "rollback to %s", SvPV_nolen(sp));
				strncpy(tempsqlstate, imp_dbh->sqlstate, strlen(imp_dbh->sqlstate)+1);
				status = _result(aTHX_ imp_dbh, cmd);
				Safefree(cmd);
			}
			else {
				status = _result(aTHX_ imp_dbh, "ROLLBACK");
				imp_dbh->done_begin = DBDPG_FALSE;
			}
		}
		if (PGRES_COMMAND_OK != status) {
			/* This is not fatal, it just means we cannot deallocate */
			if (TRACEWARN) TRC(DBILOGFP, "%sRollback failed, so no deallocate\n", THEADER);
			if (TEND) TRC(DBILOGFP, "%sEnd pg_st_deallocate_statement (cannot deallocate)\n", THEADER);
			return 1;
		}
	}

	New(0, stmt, strlen("DEALLOCATE ") + strlen(imp_sth->prepare_name) + 1, char); /* freed below */

	sprintf(stmt, "DEALLOCATE %s", imp_sth->prepare_name);

	if (TRACE5)
		TRC(DBILOGFP, "%sDeallocating (%s)\n", THEADER, imp_sth->prepare_name);

	status = _result(aTHX_ imp_dbh, stmt);
	Safefree(stmt);
	if (PGRES_COMMAND_OK != status) {
		pg_error(aTHX_ sth, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_st_deallocate_statement (error: status not OK)\n", THEADER);
		return 2;
	}

	Safefree(imp_sth->prepare_name);
	imp_sth->prepare_name = NULL;
	if (tempsqlstate[0]) {
		strncpy(imp_dbh->sqlstate, tempsqlstate, strlen(tempsqlstate)+1);
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_st_deallocate_statement\n", THEADER);
	return 0;

} /* end of pg_st_deallocate_statement */


/* ================================================================== */
void dbd_st_destroy (SV * sth, imp_sth_t * imp_sth)
{
	dTHX;
	D_imp_dbh_from_sth;
	seg_t * currseg;
	seg_t * nextseg;
	ph_t *  currph;
	ph_t *  nextph;

	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_destroy\n", THEADER);

	if (NULL == imp_sth->seg) /* Already been destroyed! */
		croak("dbd_st_destroy called twice!");

	/* If the InactiveDestroy flag has been set, we go no further */
	if (DBIc_IADESTROY(imp_dbh)) {
		if (TRACE4) {
			TRC(DBILOGFP, "%sskipping sth destroy due to InactiveDestroy\n", THEADER);
		}
		DBIc_IMPSET_off(imp_sth); /* let DBI know we've done it */
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_destroy (InactiveDestroy set)\n", THEADER);
		return;
	}

	if (imp_dbh->async_status) {
		handle_old_async(aTHX_ sth, imp_dbh, PG_OLDQUERY_WAIT);
	}

	/* Deallocate only if we named this statement ourselves and we still have a good connection */
	/* On rare occasions, dbd_db_destroy is called first and we can no longer rely on imp_dbh */
	if (imp_sth->prepared_by_us && DBIc_ACTIVE(imp_dbh)) {
		if (pg_st_deallocate_statement(aTHX_ sth, imp_sth)!=0) {
			if (TRACEWARN)
				TRC(DBILOGFP, "%sCould not deallocate\n", THEADER);
		}
	}

	Safefree(imp_sth->prepare_name);
	Safefree(imp_sth->type_info);
	Safefree(imp_sth->firstword);
	Safefree(imp_sth->PQvals);
	Safefree(imp_sth->PQlens);
	Safefree(imp_sth->PQfmts);
	Safefree(imp_sth->PQoids);

	if (imp_sth->result) {
		TRACE_PQCLEAR;
		PQclear(imp_sth->result);
		imp_sth->result = NULL;
	}

	/* Free all the segments */
	currseg = imp_sth->seg;
	while (NULL != currseg) {
		Safefree(currseg->segment);
		currseg->ph = NULL;
		nextseg = currseg->nextseg;
		Safefree(currseg);
		currseg = nextseg;
	}
	imp_sth->seg = NULL;

	/* Free all the placeholders */
	currph = imp_sth->ph;
	while (NULL != currph) {
		Safefree(currph->fooname);
		Safefree(currph->value);
		Safefree(currph->quoted);
		currph->bind_type = NULL;
		nextph = currph->nextph;
		Safefree(currph);
		currph = nextph;
	}
	imp_sth->ph = NULL;

	if (imp_dbh->async_sth)
		imp_dbh->async_sth = NULL;

	DBIc_IMPSET_off(imp_sth); /* let DBI know we've done it */

	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_destroy\n", THEADER);

} /* end of dbd_st_destroy */


/* ================================================================== */
static int
_write_copydata (pTHX_ imp_dbh_t *imp_dbh, SV * dbh, const char * buffer, STRLEN len)
{
	int copystatus;

	TRACE_PQPUTCOPYDATA;
	copystatus = PQputCopyData(imp_dbh->conn, buffer, len);

	if (0 == copystatus) {
		copystatus = coro_flush(aTHX_ imp_dbh) ? -2 : 1;
	}
	if (copystatus < 0) {
		pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
	}

	return copystatus;
}

int
pg_db_putline (pTHX_ SV * dbh, SV * svbuf)
{
	D_imp_dbh(dbh);
	const char *buffer;
	STRLEN len;
	int copystatus;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_putline\n", THEADER);

	/* We must be in COPY IN state */
	if (PGRES_COPY_IN != imp_dbh->copystate)
		croak("pg_putline can only be called directly after issuing a COPY FROM command\n");
	if (!svbuf || !SvOK(svbuf))
		croak("pg_putline can only be called with a defined value\n");

	buffer = SvPV(svbuf,len);

	TRACE_PQPUTCOPYDATA;
	copystatus = _write_copydata(aTHX_ imp_dbh, dbh, buffer, len);
	if (-1 == copystatus) {
		pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_putline (error: copystatus not -1)\n", THEADER);
		return 0;
	}
	else if (1 != copystatus) {
		croak("PQputCopyData gave a value of %d\n", copystatus);
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_putline\n", THEADER);
	return 0;

} /* end of pg_db_putline */


/* ================================================================== */
// Returns bytes read on success, 0 for async, -1 for completion, <= -2 for failure.
// Allows passing in dataline pointer as NULL to ignore the results.
static int
_read_getcopydata (pTHX_ SV * dbh, imp_dbh_t *imp_dbh, SV * dataline, int async)
{
	int copystatus;
	char *tempbuf = NULL;

	while (1) {
		// PQgetCopyData Won't block if async flag is true
		TRACE_PQGETCOPYDATA;
		copystatus = PQgetCopyData(imp_dbh->conn, &tempbuf, 1);
		if (copystatus != 0) break;
		if (async) break;

		if (CORO_WAIT_FOR_READABLE == 1) {
			imp_dbh->coro_error = COERR_INTERRUPTED;
			copystatus = -2;
			break;
		}
		TRACE_PQCONSUMEINPUT;
		PQconsumeInput(imp_dbh->conn);
	}

	if (copystatus > 0) {
		if (dataline)
			sv_setpvn(dataline, tempbuf, copystatus); // status is the length
	}
	else if (async && 0 == copystatus) { /* async and still in progress: consume and return */
		TRACE_PQCONSUMEINPUT;
		if (!PQconsumeInput(imp_dbh->conn)) {
			pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
			copystatus = -2;
		}
	}
	else if (-1 == copystatus) {
		PGresult * result;
		ExecStatusType status;
		if (dataline)
			sv_setpvn(dataline, "",0);
		imp_dbh->copystate=0;
		result = coro_read_result(aTHX_ imp_dbh);
		status = _sqlstate(aTHX_ imp_dbh, result);
		TRACE_PQCLEAR;
		PQclear(result);
		if (PGRES_COMMAND_OK != status) {
			pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
			copystatus = -2;
		}
	}
	else {
		pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
	}

	if (tempbuf) {
		TRACE_PQFREEMEM;
		PQfreemem(tempbuf);
	}

	return copystatus;
}

int
pg_db_getline (pTHX_ SV * dbh, SV * svbuf, int length)
{
	D_imp_dbh(dbh);
	int    copystatus;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_getline\n", THEADER);

	/* We must be in COPY OUT state */
	if (PGRES_COPY_OUT != imp_dbh->copystate)
		croak("pg_getline can only be called directly after issuing a COPY TO command\n");

	sv_setpvn(svbuf, "",0);
	length = 0; /* Ignored; Make compilers happy */

	copystatus = _read_getcopydata(aTHX_ dbh, imp_dbh, svbuf, 1);
	if (copystatus > 0) copystatus = 0;

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_getline (%d)\n", THEADER, copystatus);
	return copystatus;

} /* end of pg_db_getline */


/* ================================================================== */
int
pg_db_getcopydata (pTHX_ SV * dbh, SV * dataline, int async)
{
	D_imp_dbh(dbh);
	int    copystatus;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_getcopydata\n", THEADER);

	/* We must be in COPY OUT state */
	if (PGRES_COPY_OUT != imp_dbh->copystate)
		croak("pg_getcopydata can only be called directly after issuing a COPY TO command\n");

	copystatus = _read_getcopydata(aTHX_ dbh, imp_dbh, dataline, async);

	if (TEND) {
		if (async && copystatus == -2)
			TRC(DBILOGFP, "%sEnd pg_db_getcopydata (error: async in progress)\n", THEADER);
		else 
			TRC(DBILOGFP, "%sEnd pg_db_getcopydata\n", THEADER);
	}
	return copystatus;

} /* end of pg_db_getcopydata */


/* ================================================================== */
int
pg_db_putcopydata (pTHX_ SV * dbh, SV * dataline)
{
	D_imp_dbh(dbh);
	int copystatus;
	STRLEN len;
	char *line;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_putcopydata\n", THEADER);

	/* We must be in COPY IN state */
	if (PGRES_COPY_IN != imp_dbh->copystate)
		croak("pg_putcopydata can only be called directly after issuing a COPY FROM command\n");

	line = SvUTF8(dataline) ? SvPVutf8(dataline,len) : SvPV(dataline,len);

	TRACE_PQPUTCOPYDATA;
	copystatus = PQputCopyData(imp_dbh->conn, line, len);

	if (0 == copystatus) {
		copystatus = coro_flush(aTHX_ imp_dbh) ? -2 : 1;
	}
	if (copystatus < 0) {
		pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_putcopydata\n", THEADER);	
	return copystatus == 1 ? 1 : 0;

} /* end of pg_db_putcopydata */


/* ================================================================== */
// 1 for success, 0 for failure
static int
_write_putcopyend(pTHX_ imp_dbh_t *imp_dbh, SV * dbh)
{
	int copystatus;

	TRACE_PQPUTCOPYEND;
	copystatus = PQputCopyEnd(imp_dbh->conn, NULL);

	if (0 == copystatus) {
		copystatus = coro_flush(aTHX_ imp_dbh) ? -2 : 1;
	}

	if (1 == copystatus) {
		PGresult * result;
		ExecStatusType status;
		imp_dbh->copystate = 0;
		result = coro_read_result(aTHX_ imp_dbh);
		status = _sqlstate(aTHX_ imp_dbh, result);
		TRACE_PQCLEAR;
		PQclear(result);
		if (PGRES_COMMAND_OK != status) {
			pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
			return 0;
		}
	}
	else {
		pg_error(aTHX_ dbh, PGRES_FATAL_ERROR, _error_message(imp_dbh));
		return 0;
	}
	return 1;
}

int pg_db_putcopyend (pTHX_ SV * dbh)
{

	/* If in COPY_IN mode, terminate the COPYing */
	/* Returns 1 on success, otherwise 0 (plus a probably warning/error) */

	D_imp_dbh(dbh);
	int status;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_putcopyend\n", THEADER);

	if (0 == imp_dbh->copystate) {
		warn("pg_putcopyend cannot be called until a COPY is issued");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_putcopyend (warning: copystate is 0)\n", THEADER);
		return 0;
	}

	if (PGRES_COPY_OUT == imp_dbh->copystate) {
		warn("PQputcopyend does not need to be called when using PGgetcopydata");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_putcopyend (warning: copy state is OUT)\n", THEADER);
		return 0;
	}

	/* Must be PGRES_COPY_IN at this point */

	status = _write_putcopyend(aTHX_ imp_dbh, dbh); // 1 on success
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_putcopyend%s\n", THEADER, status ? "" : " (error!)");
	return status;

} /* end of pg_db_putcopyend */


/* ================================================================== */
// -1 for success, 0 for failure
int pg_db_endcopy (pTHX_ SV * dbh)
{
	D_imp_dbh(dbh);
	int copystatus;
	int status;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_endcopy\n", THEADER);

	if (0==imp_dbh->copystate)
		croak("pg_endcopy cannot be called until a COPY is issued");

	if (PGRES_COPY_IN == imp_dbh->copystate) {
		copystatus = _write_putcopyend(aTHX_ imp_dbh, dbh);
		status = (copystatus == 1) ? -1 : 0;
	}
	else {
		// PQendcopy doesn't work when using PQgetCopyData 
		// (which is necessary for non-blocking),
		// so just consume the rest of the stream.
		do {
			copystatus = _read_getcopydata(aTHX_ dbh, imp_dbh, NULL, 0);
		} while (copystatus > 0);
		status = (copystatus == -1) ? -1 : 0;
	}

	imp_dbh->copystate = 0;
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_endcopy (%d)\n", THEADER, status);
	return status;

} /* end of pg_db_endcopy */


/* ================================================================== */
void pg_db_pg_server_trace (SV * dbh, FILE * fh)
{
	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_pg_server_trace\n", THEADER);

	TRACE_PQTRACE;
	PQtrace(imp_dbh->conn, fh);

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_pg_server_trace\n", THEADER);

} /* end of pg_db_pg_server_trace */


/* ================================================================== */
void pg_db_pg_server_untrace (SV * dbh)
{
	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_pg_server_untrace\n", THEADER);

	TRACE_PQUNTRACE;
	PQuntrace(imp_dbh->conn);

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_pg_server_untrace\n", THEADER);

} /* end of pg_db_pg_server_untrace */


/* ================================================================== */
int pg_db_savepoint (SV * dbh, imp_dbh_t * imp_dbh, char * savepoint)
{
	dTHX;
	int    status;
	char * action;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_savepoint (name: %s)\n", THEADER, savepoint);

	if (imp_dbh->pg_server_version < 80000)
		croak("Savepoints are only supported on server version 8.0 or higher");

	/* no action if AutoCommit = on or the connection is invalid */
	if ((NULL == imp_dbh->conn) || (DBIc_has(imp_dbh, DBIcf_AutoCommit))) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_savepoint (0)\n", THEADER);
		return 0;
	}

	/* Start a new transaction if this is the first command */
	if (!imp_dbh->done_begin) {
		status = _result(aTHX_ imp_dbh, "begin");
		if (PGRES_COMMAND_OK != status) {
			pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
			if (TEND) TRC(DBILOGFP, "%sEnd pg_db_savepoint (error: status not OK for begin)\n", THEADER);
			return -2;
		}
		imp_dbh->done_begin = DBDPG_TRUE;
	}

	New(0, action, strlen(savepoint) + 11, char); /* freed below */
	sprintf(action, "savepoint %s", savepoint);
	status = _result(aTHX_ imp_dbh, action);
	Safefree(action);

	if (PGRES_COMMAND_OK != status) {
		pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_savepoint (error: status not OK for savepoint)\n", THEADER);
		return 0;
	}

	av_push(imp_dbh->savepoints, newSVpv(savepoint,0));
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_savepoint\n", THEADER);
	return 1;

} /* end of pg_db_savepoint */


/* ================================================================== */
int pg_db_rollback_to (SV * dbh, imp_dbh_t * imp_dbh, const char *savepoint)
{
	dTHX;
	int    status;
	char * action;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_rollback_to (name: %s)\n", THEADER, savepoint);

	if (imp_dbh->pg_server_version < 80000)
		croak("Savepoints are only supported on server version 8.0 or higher");

	/* no action if AutoCommit = on or the connection is invalid */
	if ((NULL == imp_dbh->conn) || (DBIc_has(imp_dbh, DBIcf_AutoCommit))) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_to (0)\n", THEADER);
		return 0;
	}

	New(0, action, strlen(savepoint) + 13, char);
	sprintf(action, "rollback to %s", savepoint);
	status = _result(aTHX_ imp_dbh, action);
	Safefree(action);

	if (PGRES_COMMAND_OK != status) {
		pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_to (error: status not OK for rollback)\n", THEADER);
		return 0;
	}

	pg_db_free_savepoints_to(aTHX_ imp_dbh, savepoint);
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_rollback_to\n", THEADER);
	return 1;

} /* end of pg_db_rollback_to */


/* ================================================================== */
int pg_db_release (SV * dbh, imp_dbh_t * imp_dbh, char * savepoint)
{
	dTHX;
	int    status;
	char * action;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_release (name: %s)\n", THEADER, savepoint);

	if (imp_dbh->pg_server_version < 80000)
		croak("Savepoints are only supported on server version 8.0 or higher");

	/* no action if AutoCommit = on or the connection is invalid */
	if ((NULL == imp_dbh->conn) || (DBIc_has(imp_dbh, DBIcf_AutoCommit))) {
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_release (0)\n", THEADER);
		return 0;
	}

	New(0, action, strlen(savepoint) + 9, char);
	sprintf(action, "release %s", savepoint);
	status = _result(aTHX_ imp_dbh, action);
	Safefree(action);

	if (PGRES_COMMAND_OK != status) {
		pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_release (error: status not OK for release)\n", THEADER);
		return 0;
	}

	pg_db_free_savepoints_to(aTHX_ imp_dbh, savepoint);
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_release\n", THEADER);
	return 1;

} /* end of pg_db_release */


/* ================================================================== */
/* For lo_* functions. Used to ensure we are in a transaction */
static int pg_db_start_txn (pTHX_ SV * dbh, imp_dbh_t * imp_dbh)
{
	int status = -1;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_start_txn\n", THEADER);

	/* If not autocommit, start a new transaction */
	if (!imp_dbh->done_begin) {
		status = _result(aTHX_ imp_dbh, "begin");
		if (PGRES_COMMAND_OK != status) {
			pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
			if (TEND) TRC(DBILOGFP, "%sEnd pg_db_start_txn (error: status not OK for begin)\n", THEADER);
			return 0;
		}
		/* disable non-blocking mode while in lo_ mode */
		TRACE_PQSETNONBLOCKING;
		PQsetnonblocking(imp_dbh->conn,0);
		imp_dbh->done_begin = DBDPG_TRUE;
	}
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_start_txn\n", THEADER);

	return 1;

} /* end of pg_db_start_txn */


/* ================================================================== */
/* For lo_import and lo_export functions. Used to commit or rollback a 
   transaction, but only if AutoCommit is on. */
static int pg_db_end_txn (pTHX_ SV * dbh, imp_dbh_t * imp_dbh, int commit)
{
	int status = -1;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_end_txn with %s\n",
					THEADER, commit ? "commit" : "rollback");

	status = _result(aTHX_ imp_dbh, commit ? "commit" : "rollback");
	imp_dbh->done_begin = DBDPG_FALSE;
	TRACE_PQSETNONBLOCKING;
	PQsetnonblocking(imp_dbh->conn,1);
	if (PGRES_COMMAND_OK != status) {
		pg_error(aTHX_ dbh, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_end_txn (error: status not OK for %s)\n",
					  THEADER, commit ? "commit" : "rollback");
		return 0;
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_end_txn\n", THEADER);

	return 1;

} /* end of pg_db_end_txn */

/* Large object functions */

/* ================================================================== */
unsigned int pg_db_lo_creat (SV * dbh, int mode)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_pg_lo_creat (mode: %d)\n", THEADER, mode);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_creat when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh)) {
		return 0; /* No other option, because lo_creat returns an Oid */
	}

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_creat\n", THEADER);
	}

	return lo_creat(imp_dbh->conn, mode); /* 0 on error */

}

/* ================================================================== */
int pg_db_lo_open (SV * dbh, unsigned int lobjId, int mode)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_pg_lo_open (mode: %d objectid: %d)\n",
					THEADER, mode, lobjId);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_open when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -2;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_open\n", THEADER);
	}

	return lo_open(imp_dbh->conn, lobjId, mode); /* -1 on error */

}

/* ================================================================== */
int pg_db_lo_close (SV * dbh, int fd)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_close (fd: %d)\n", THEADER, fd);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_close when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -1;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_close\n", THEADER);
	}

	return lo_close(imp_dbh->conn, fd); /* <0 on error, 0 if ok */

}

/* ================================================================== */
int pg_db_lo_read (SV * dbh, int fd, char * buf, size_t len)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_read (fd: %d length: %d)\n",
					THEADER, fd, (int)len);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_read when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -1;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_read\n", THEADER);
	}

	return lo_read(imp_dbh->conn, fd, buf, len); /* bytes read, <0 on error */

}

/* ================================================================== */
int pg_db_lo_write (SV * dbh, int fd, char * buf, size_t len)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_write (fd: %d length: %d)\n",
					THEADER, fd, (int)len);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_write when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -1;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_write\n", THEADER);
	}

	return lo_write(imp_dbh->conn, fd, buf, len); /* bytes written, <0 on error */

}

/* ================================================================== */
int pg_db_lo_lseek (SV * dbh, int fd, int offset, int whence)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_lseek (fd: %d offset: %d whence: %d)\n",
					THEADER, fd, offset, whence);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_lseek when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -1;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_lseek\n", THEADER);
	}

	return lo_lseek(imp_dbh->conn, fd, offset, whence); /* new position, -1 on error */

}

/* ================================================================== */
int pg_db_lo_tell (SV * dbh, int fd)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_tell (fd: %d)\n", THEADER, fd);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_tell when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -1;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_tell\n", THEADER);
	}

	return lo_tell(imp_dbh->conn, fd); /* current position, <0 on error */

}

/* ================================================================== */
int pg_db_lo_unlink (SV * dbh, unsigned int lobjId)
{

	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_unlink (objectid: %d)\n", THEADER, lobjId);

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		croak("Cannot call pg_lo_unlink when AutoCommit is on");
	}

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -1;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_unlink\n", THEADER);
	}

	return lo_unlink(imp_dbh->conn, lobjId); /* 1 on success, -1 on failure */

}

/* ================================================================== */
unsigned int pg_db_lo_import (SV * dbh, char * filename)
{

	Oid loid;
	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_import (filename: %s)\n", THEADER, filename);

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return 0; /* No other option, because lo_import returns an Oid */

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_import\n", THEADER);
	}
	loid = lo_import(imp_dbh->conn, filename); /* 0 on error */

	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		if (!pg_db_end_txn(aTHX_ dbh, imp_dbh, 0==loid ? 0 : 1))
			return 0;
	}

	return loid;

}

/* ================================================================== */
int pg_db_lo_export (SV * dbh, unsigned int lobjId, char * filename)
{

	Oid loid;
	dTHX;
	D_imp_dbh(dbh);

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_lo_export (objectid: %d filename: %s)\n",
					THEADER, lobjId, filename);

	if (!pg_db_start_txn(aTHX_ dbh,imp_dbh))
		return -2;

	if (TLIBPQ) {
		TRC(DBILOGFP, "%slo_export\n", THEADER);
	}
	loid = lo_export(imp_dbh->conn, lobjId, filename); /* 1 on success, -1 on failure */
	if (DBIc_has(imp_dbh, DBIcf_AutoCommit)) {
		if (!pg_db_end_txn(aTHX_ dbh, imp_dbh, -1==loid ? 0 : 1))
			return -1;
	}

	return loid;
}


/* ================================================================== */
int dbd_st_blob_read (SV * sth, imp_sth_t * imp_sth, int lobjId, long offset, long len, SV * destrv, long destoffset)
{
	dTHX;
	D_imp_dbh_from_sth;

	int    ret, lobj_fd, nbytes;
	STRLEN nread;
	SV *   bufsv;
	char * tmp;
	
	if (TSTART) TRC(DBILOGFP, "%sBegin dbd_st_blob_read (objectid: %d offset: %ld length: %ld)\n",
					THEADER, lobjId, offset, len);

	/* safety checks */
	if (lobjId <= 0) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, "dbd_st_blob_read: lobjId <= 0");
		return 0;
	}
	if (offset < 0) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, "dbd_st_blob_read: offset < 0");
		return 0;
	}
	if (len < 0) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, "dbd_st_blob_read: len < 0");
		return 0;
	}
	if (! SvROK(destrv)) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, "dbd_st_blob_read: destrv not a reference");
		return 0;
	}
	if (destoffset < 0) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, "dbd_st_blob_read: destoffset < 0");
		return 0;
	}
	
	/* dereference destination and ensure it's writable string */
	bufsv = SvRV(destrv);
	if (0==destoffset) {
		sv_setpvn(bufsv, "", 0);
	}
	
	/* open large object */
	lobj_fd = lo_open(imp_dbh->conn, (unsigned)lobjId, INV_READ);
	if (lobj_fd < 0) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_blob_read (error: open failed)\n", THEADER);
		return 0;
	}
	
	/* seek on large object */
	if (offset > 0) {
		ret = lo_lseek(imp_dbh->conn, lobj_fd, (int)offset, SEEK_SET);
		if (ret < 0) {
			pg_error(aTHX_ sth, PGRES_FATAL_ERROR, _error_message(imp_dbh));
			if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_blob_read (error: bad seek)\n", THEADER);
			return 0;
		}
	}
	
	/* read from large object */
	nread = 0;
	SvGROW(bufsv, (STRLEN)(destoffset + nread + BUFSIZ + 1));
	tmp = (SvPVX(bufsv)) + destoffset + nread;
	while ((nbytes = lo_read(imp_dbh->conn, lobj_fd, tmp, BUFSIZ)) > 0) {
		nread += nbytes;
		/* break if user wants only a specified chunk */
		if (len > 0 && nread > (STRLEN)len) {
			nread = (STRLEN)len;
			break;
		}
		SvGROW(bufsv, (STRLEN)(destoffset + nread + BUFSIZ + 1));
		tmp = (SvPVX(bufsv)) + destoffset + nread;
	}
	
	/* terminate string */
	SvCUR_set(bufsv, (STRLEN)(destoffset + nread));
	*SvEND(bufsv) = '\0';
	
	/* close large object */
	ret = lo_close(imp_dbh->conn, lobj_fd);
	if (ret < 0) {
		pg_error(aTHX_ sth, PGRES_FATAL_ERROR, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_blob_read (error: close failed)\n", THEADER);
		return 0;
	}
	
	if (TEND) TRC(DBILOGFP, "%sEnd dbd_st_blob_read (bytes: %d)\n", THEADER, (int)nread);
	return (int)nread;

} /* end of dbd_st_blob_read */


/* ================================================================== */
/* Return the result of an asynchronous query, waiting if needed */
int pg_db_result (SV *h, imp_dbh_t *imp_dbh)
{
	dTHX;
	PGresult *result;
	ExecStatusType status = PGRES_FATAL_ERROR;
	int rows = 0;
	char *cmdStatus = NULL;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_result\n", THEADER);

	if (1 != imp_dbh->async_status) {
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, "No asynchronous query is running\n");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_result (error: no async)\n", THEADER);
		return -2;
	}	

	imp_dbh->copystate = 0; /* Assume not in copy mode until told otherwise */

	TRACE_PQGETRESULT;
	while ((result = PQgetResult(imp_dbh->conn)) != NULL) {
		/* TODO: Better multiple result-set handling */
		status = _sqlstate(aTHX_ imp_dbh, result);
		switch (status) {
		case PGRES_TUPLES_OK:
			TRACE_PQNTUPLES;
			rows = PQntuples(result);

			if (imp_dbh->async_sth) {
				imp_dbh->async_sth->cur_tuple = 0;
				TRACE_PQNFIELDS;
				DBIc_NUM_FIELDS(imp_dbh->async_sth) = PQnfields(result);
				DBIc_ACTIVE_on(imp_dbh->async_sth);
			}

			break;
		case PGRES_COMMAND_OK:
			/* non-select statement */
			TRACE_PQCMDSTATUS;
			cmdStatus = PQcmdStatus(result);
			if (0 == strncmp(cmdStatus, "INSERT", 6)) {
				/* INSERT(space)oid(space)numrows */
				for (rows=8; cmdStatus[rows-1] != ' '; rows++) {
				}
				rows = atoi(cmdStatus + rows);
			}
			else if (0 == strncmp(cmdStatus, "MOVE", 4)) {
				rows = atoi(cmdStatus + 5);
			}
			else if (0 == strncmp(cmdStatus, "DELETE", 6)
					 || 0 == strncmp(cmdStatus, "UPDATE", 6)) {
				rows = atoi(cmdStatus + 7);
			}
			break;
		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
			/* Copy Out/In data transfer in progress */
			imp_dbh->copystate = status;
			rows = -1;
			break;
		case PGRES_EMPTY_QUERY:
		case PGRES_BAD_RESPONSE:
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		default:
			rows = -2;
			pg_error(aTHX_ h, status, _error_message(imp_dbh));
			break;
		}

		if (imp_dbh->async_sth) {
			if (imp_dbh->async_sth->result) { /* For potential multi-result sets */
				TRACE_PQCLEAR;
				PQclear(imp_dbh->async_sth->result);
			}
			imp_dbh->async_sth->result = result;
		}
		else {
			TRACE_PQCLEAR;
			PQclear(result);
		}
	}

	if (imp_dbh->async_sth) {
		imp_dbh->async_sth->rows = rows;
		imp_dbh->async_sth->async_status = 0;
	}
	imp_dbh->async_status = 0;
	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_result (rows: %d)\n", THEADER, rows);
	return rows;

} /* end of pg_db_result */


/* 
==================================================================
Indicates if an asynchronous query has finished yet
Accepts either a database or a statement handle
Returns:
  -1 if no query is running (and raises an exception)
  +1 if the query is finished
   0 if the query is still running
  -2 for other errors
==================================================================
*/
int pg_db_ready(SV *h, imp_dbh_t *imp_dbh)
{
	dTHX;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_ready (async status: %d)\n",
					THEADER, imp_dbh->async_status);

	if (0 == imp_dbh->async_status) {
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, "No asynchronous query is running\n");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_ready (error: no async)\n", THEADER);
		return -1;
	}	

	TRACE_PQCONSUMEINPUT;
	if (!PQconsumeInput(imp_dbh->conn)) {
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_ready (error: consume failed)\n", THEADER);
		return -2;
	}

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_ready\n", THEADER);
	TRACE_PQISBUSY;
	return PQisBusy(imp_dbh->conn) ? 0 : 1;

} /* end of pg_db_ready */


/*
Attempt to cancel a running asynchronous query
Returns true if the cancel succeeded, and false if it did not
If it did successfully cancel the query, it will also do a rollback.
Note that queries which have finished do not cause a rollback.
In this case, pg_cancel will return false.
NOTE: We only return true if we cancelled and rolled back!
*/

/* ================================================================== */
int pg_db_cancel(SV *h, imp_dbh_t *imp_dbh)
{
	dTHX;
	PGcancel *cancel;
	char errbuf[256];
	PGresult *result;
	ExecStatusType status;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_cancel (async status: %d)\n",
					THEADER, imp_dbh->async_status);

	if (0 == imp_dbh->async_status) {
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, "No asynchronous query is running");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel (error: no async)\n", THEADER);
		return DBDPG_FALSE;
	}

	if (-1 == imp_dbh->async_status) {
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, "Asychronous query has already been cancelled");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel (error: async cancelled)\n", THEADER);
		return DBDPG_FALSE;
	}

	/* Get the cancel structure */
	TRACE_PQGETCANCEL;
	cancel = PQgetCancel(imp_dbh->conn);

	/* This almost always works. If not, free our structure and complain loudly */
	TRACE_PQGETCANCEL;
	if (! PQcancel(cancel,errbuf,sizeof(errbuf))) {
		TRACE_PQFREECANCEL;
		PQfreeCancel(cancel);
		if (TRACEWARN) { TRC(DBILOGFP, "%sPQcancel failed: %s\n", THEADER, errbuf); }
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, "PQcancel failed");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel (error: cancel failed)\n", THEADER);
		return DBDPG_FALSE;
	}
	TRACE_PQFREECANCEL;
	PQfreeCancel(cancel);

	/* Whatever else happens, we should no longer be inside of an async query */
	imp_dbh->async_status = -1;
	if (imp_dbh->async_sth)
		imp_dbh->async_sth->async_status = -1;

	/* Read in the result - assume only one */
	TRACE_PQGETRESULT;
	result = PQgetResult(imp_dbh->conn);
	if (!result) {
		pg_error(aTHX_ h, PGRES_FATAL_ERROR, "Failed to get a result after PQcancel");
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel (error: no result)\n", THEADER);
		return DBDPG_FALSE;
	}

	status = _sqlstate(aTHX_ imp_dbh, result);

	/* If we actually cancelled a running query, perform a rollback */
	if (0 == strncmp(imp_dbh->sqlstate, "57014", 5)) {
		if (TRACE3) { TRC(DBILOGFP, "%sRolling back after cancelled query\n", THEADER); }
		dbd_db_rollback(h, imp_dbh);
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel (rollback)\n", THEADER);
		return DBDPG_TRUE;
	}

	/* If we got any other error, make sure we report it */
	if (0 != strncmp(imp_dbh->sqlstate, "00000", 5)) {
		if (TRACEWARN) TRC(DBILOGFP,
						   "%sQuery was not cancelled: was already finished\n", THEADER);
		pg_error(aTHX_ h, status, _error_message(imp_dbh));
		if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel (error)\n", THEADER);
	}
	else if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel\n", THEADER);
	return DBDPG_FALSE;
					
} /* end of pg_db_cancel */


/* ================================================================== */
int pg_db_cancel_sth(SV *sth, imp_sth_t *imp_sth)
{
	dTHX;
	D_imp_dbh_from_sth;
	bool cancel_result;

	if (TSTART) TRC(DBILOGFP, "%sBegin pg_db_cancel_sth (async status: %d)\n",
					THEADER, imp_dbh->async_status);

	cancel_result = pg_db_cancel(sth, imp_dbh);

	dbd_st_finish(sth, imp_sth);

	if (TEND) TRC(DBILOGFP, "%sEnd pg_db_cancel_sth\n", THEADER);
	return cancel_result;

} /* end of pg_db_cancel_sth */


/*
Finish up an existing async query, either by cancelling it,
or by waiting for a result.

 */
/* ================================================================== */
static int handle_old_async(pTHX_ SV * handle, imp_dbh_t * imp_dbh, const int asyncflag) {

	PGresult *result;
	ExecStatusType status;

	if (TSTART) TRC(DBILOGFP, "%sBegin handle_old_sync (flag: %d)\n", THEADER, asyncflag);

	if (asyncflag & PG_OLDQUERY_CANCEL) {
		/* Cancel the outstanding query */
		if (TRACE3) { TRC(DBILOGFP, "%sCancelling old async command\n", THEADER); }
		TRACE_PQISBUSY;
		if (PQisBusy(imp_dbh->conn)) {
			PGcancel *cancel;
			char errbuf[256];
			int cresult;
			if (TRACE3) TRC(DBILOGFP, "%sAttempting to cancel query\n", THEADER);
			TRACE_PQGETCANCEL;
			cancel = PQgetCancel(imp_dbh->conn);
			TRACE_PQCANCEL;
			cresult = PQcancel(cancel,errbuf,255);
			if (! cresult) {
				if (TRACEWARN) { TRC(DBILOGFP, "%sPQcancel failed: %s\n", THEADER, errbuf); }
				pg_error(aTHX_ handle, PGRES_FATAL_ERROR, "Could not cancel previous command");
				if (TEND) TRC(DBILOGFP, "%sEnd handle_old_async (error: could not cancel)\n", THEADER);
				return -2;
			}
			TRACE_PQFREECANCEL;
			PQfreeCancel(cancel);
			/* Suck up the cancellation notice */
			TRACE_PQGETRESULT;
			while ((result = PQgetResult(imp_dbh->conn)) != NULL) {
			}
			/* We need to rollback! - reprepare!? */
			TRACE_PQEXEC;
			PQexec(imp_dbh->conn, "rollback");
			imp_dbh->done_begin = DBDPG_FALSE;
		}
	}
	else if (asyncflag & PG_OLDQUERY_WAIT || imp_dbh->async_status == -1) {
		/* Finish up the outstanding query and throw out the result, unless an error */
		if (TRACE3) { TRC(DBILOGFP, "%sWaiting for old async command to finish\n", THEADER); }
		TRACE_PQGETRESULT;
		while ((result = PQgetResult(imp_dbh->conn)) != NULL) {
			status = _sqlstate(aTHX_ imp_dbh, result);
			TRACE_PQCLEAR;
			PQclear(result);
			if (status == PGRES_COPY_IN) { /* In theory, this should be caught by copystate, but we'll be careful */
				TRACE_PQPUTCOPYEND;
				if (-1 == PQputCopyEnd(imp_dbh->conn, NULL)) {
					pg_error(aTHX_ handle, PGRES_FATAL_ERROR, _error_message(imp_dbh));
					if (TEND) TRC(DBILOGFP, "%sEnd handle_old_async (error: PQputCopyEnd)\n", THEADER);
					return -2;
				}
			}
			else if (status == PGRES_COPY_OUT) { /* Won't be as nice with this one */
				pg_error(aTHX_ handle, PGRES_FATAL_ERROR, "Must finish copying first");
				if (TEND) TRC(DBILOGFP, "%sEnd handle_old_async (error: COPY_OUT status)\n", THEADER);
				return -2;
			}
			else if (status != PGRES_EMPTY_QUERY
					 && status != PGRES_COMMAND_OK
					 && status != PGRES_TUPLES_OK) {
				pg_error(aTHX_ handle, status, _error_message(imp_dbh));
				if (TEND) TRC(DBILOGFP, "%sEnd handle_old_async (error: bad status)\n", THEADER);
				return -2;
			}
		}
	}
	else {
		pg_error(aTHX_ handle, PGRES_FATAL_ERROR, "Cannot execute until previous async query has finished");
		if (TEND) TRC(DBILOGFP, "%sEnd handle_old_async (error: unfinished)\n", THEADER);
		return -2;
	}

	/* If we made it this far, safe to assume there is no running query */
	imp_dbh->async_status = 0;
	if (imp_dbh->async_sth)
		imp_dbh->async_sth->async_status = 0;

	if (TEND) TRC(DBILOGFP, "%sEnd handle_old_async\n", THEADER);
	return 0;

} /* end of handle_old_async */



/*
Some information to keep you sane:
typedef enum
{	
0	PGRES_EMPTY_QUERY = 0,  // empty query string was executed 
1	PGRES_COMMAND_OK,      // a query command that doesn't return anything was executed properly by the backend 
2	PGRES_TUPLES_OK,       // a query command that returns tuples was executed properly by the backend, PGresult contains the result tuples 
3	PGRES_COPY_OUT,	       // Copy Out data transfer in progress 
4	PGRES_COPY_IN,	       // Copy In data transfer in progress 
5	PGRES_BAD_RESPONSE,    // an unexpected response was recv'd from the backend 
6	PGRES_NONFATAL_ERROR,  // notice or warning message 
7	PGRES_FATAL_ERROR      // query failed 
} ExecStatusType;

*/

static SV *coro_init (pTHX_ imp_dbh_t *imp_dbh, PGconn *conn)
{
	dSP;

	if (TCORO) TRC(DBILOGFP, "%scoro_init\n", THEADER);

	TRACE_PQSETNONBLOCKING;
	PQsetnonblocking(conn,1);

	TRACE_PQSOCKET;
        imp_dbh->socket_fd = PQsocket(conn);
        imp_dbh->last_result_err = NULL;
        imp_dbh->coro_handle = NULL;
        imp_dbh->coro_error = COERR_OK;

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSViv(imp_dbh->socket_fd)));
	PUTBACK;
	call_pv("DBD::Pg::dr::_coro_init", G_SCALAR|G_EVAL);
	SPAGAIN;

	if (SvTRUE(ERRSV)) {
		/* XXX: cop-out error handling; croak never returns */
		/* ideally pass ERRSV up to the perl stack? */
		croak("error in coro init: %s", SvPVX(ERRSV));
	}
	else {
		imp_dbh->coro_handle = POPs;
		SvREFCNT_inc(imp_dbh->coro_handle);
	}

	FREETMPS;
	LEAVE;

	return imp_dbh->coro_handle;
}

static void coro_cleanup (pTHX_ imp_dbh_t *imp_dbh)
{
	if (imp_dbh->coro_handle) {
		SvREFCNT_dec(imp_dbh->coro_handle);
		imp_dbh->coro_handle = NULL;
	}
}

static int coro_wait_for (pTHX_ imp_dbh_t *imp_dbh, const char *func) 
{
	dSP;
	int c;
	int result = 0;

	if (TCORO) TRC(DBILOGFP, "%scoro_wait_for: %s\n", THEADER, func);

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	XPUSHs(imp_dbh->coro_handle);
	PUTBACK;
	c = call_method(func, G_SCALAR|G_EVAL);
	SPAGAIN;

	if (SvTRUE(ERRSV)) {
		_pg_warn(aTHX_ imp_dbh, "exception while waiting for Pg connection %d to become %s: %s",
			 imp_dbh->socket_fd, (func==READABLE)?"readable":"writable", SvPV_nolen(ERRSV));
	}
	else {
		result = POPi;
	}

	FREETMPS;
	LEAVE;

	return result;
}

static PGconn * coro_PQconnectdb(pTHX_ imp_dbh_t *imp_dbh, const char *conn_str) {
	PGconn *conn;
	PostgresPollingStatusType next_state;
	int fd;

	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_PQconnectdb (%s)\n", THEADER, conn_str);

	imp_dbh->socket_fd = -1;

	/* XXX: blocks on network lookup */
	TRACE_PQCONNECTSTART;
	conn = PQconnectStart(conn_str);
	if (conn == NULL) {
		imp_dbh->coro_error = COERR_FATAL;
		goto finish_coro_PQconnectStart;
	}

	TRACE_PQSTATUS;
	if (PQstatus(conn) == CONNECTION_BAD) {
		imp_dbh->coro_error = COERR_PGFATAL;
		goto finish_coro_PQconnectStart;
	}

	if (!coro_init(aTHX_ imp_dbh, conn)) {
		imp_dbh->coro_error = COERR_FATAL;
		goto finish_coro_PQconnectStart;
	}

	// docs say to assume polling for writability first
	next_state = PGRES_POLLING_WRITING;

	do {
		if (next_state == PGRES_POLLING_WRITING) {
			if (CORO_WAIT_FOR_WRITABLE != 1) {
				imp_dbh->coro_error = COERR_FATAL;
				goto finish_coro_PQconnectStart;
			}
		}
		else if (next_state == PGRES_POLLING_READING) {
			if (CORO_WAIT_FOR_READABLE != 1) {
				imp_dbh->coro_error = COERR_FATAL;
				goto finish_coro_PQconnectStart;
			}
		}
		else {
			imp_dbh->coro_error = COERR_FATAL;
			goto finish_coro_PQconnectStart;
			break;
		}

		TRACE_PQCONNECTPOLL;
	} while ((next_state = PQconnectPoll(conn)) != PGRES_POLLING_OK);

	TRACE_PQSTATUS;
	if (PQstatus(conn) != CONNECTION_OK) {
		imp_dbh->coro_error = COERR_PGFATAL;
	}
	else {
		imp_dbh->coro_error = COERR_OK;
	}

finish_coro_PQconnectStart:
	if (imp_dbh->coro_error == COERR_PGFATAL && conn) {
		PQfinish(conn);
		conn = NULL;
	}
	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_PQconnectStart %d\n", THEADER, imp_dbh->socket_fd);
	return conn;
}

static int coro_flush (pTHX_ imp_dbh_t *imp_dbh)
{
	int s = 1;
	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_flush %d\n", THEADER, imp_dbh->socket_fd);

	// Don't try to flush if the connection isn't OK
	TRACE_PQSTATUS;
	if (PQstatus(imp_dbh->conn) != CONNECTION_OK) {
		imp_dbh->coro_error = COERR_PGFATAL;
		if (TEND || TCORO) {
			TRACE_PQERRORMESSAGE;
			TRC(DBILOGFP, "%sEnd coro_flush %d (Conn error: %s)\n", THEADER, imp_dbh->socket_fd, PQerrorMessage(imp_dbh->conn));
		}
		return -1;
	}

	while (s == 1) {
		if (CORO_WAIT_FOR_WRITABLE != 1) {
			imp_dbh->coro_error = COERR_INTERRUPTED;
			if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_flush %d (INTERRUPTED)\n", THEADER, imp_dbh->socket_fd);
			return -1;
		}
		TRACE_PQFLUSH;
		s = PQflush(imp_dbh->conn);
	}
	if (s == -1) {
		imp_dbh->coro_error = COERR_PGFATAL;
		TRACE_PQERRORMESSAGE;
		_pg_warn(aTHX_ imp_dbh, "PQflush %d failed: %s", imp_dbh->socket_fd, PQerrorMessage(imp_dbh->conn));
	}
	else {
		imp_dbh->coro_error = COERR_OK;
	}
	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_flush %d (%d)\n", THEADER, imp_dbh->socket_fd, s);
	return s;
}

static PGresult *coro_read_one_result(pTHX_ imp_dbh_t *imp_dbh, bool expected)
{
	PGresult *result = NULL;
	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_read_one_result %d\n", THEADER, imp_dbh->socket_fd);

	TRACE_PQISBUSY;
	while (PQisBusy(imp_dbh->conn)) {
		if (CORO_WAIT_FOR_READABLE != 1) {
			imp_dbh->coro_error = COERR_INTERRUPTED;
			goto finish_coro_read_one_result;
		}

		TRACE_PQCONSUMEINPUT;
		if (!PQconsumeInput(imp_dbh->conn)) {
			imp_dbh->coro_error = COERR_PGFATAL;
			TRACE_PQERRORMESSAGE;
			_pg_warn(aTHX_ imp_dbh, "PQconsumeInput failed during coro_read_one_result %d: %s", imp_dbh->socket_fd, PQerrorMessage(imp_dbh->conn));
			goto finish_coro_read_one_result;
		}

		TRACE_PQISBUSY; // for the next iteration
	}

	TRACE_PQGETRESULT;
	result = PQgetResult(imp_dbh->conn);
	if (expected && !result) {
		imp_dbh->coro_error = COERR_PGFATAL;
		TRACE_PQERRORMESSAGE;
		_pg_warn(aTHX_ imp_dbh, "PQgetResult failed during coro_read_one_result %d: %s", imp_dbh->socket_fd, PQerrorMessage(imp_dbh->conn));
		goto finish_coro_read_one_result;
	}

finish_coro_read_one_result:
	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_read_one_result %d\n", THEADER, imp_dbh->socket_fd);
	return result;
}

static PGresult *coro_read_result(pTHX_ imp_dbh_t *imp_dbh)
{
	PGresult *result = NULL;
	PGresult *next_result = NULL;
	ExecStatusType status;
	bool has_error = false;

	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_read_result %d\n", THEADER, imp_dbh->socket_fd);
        imp_dbh->last_result_err = NULL;
        imp_dbh->last_result_status = 0;

	result = coro_read_one_result(aTHX_ imp_dbh, true);
	if (!result) goto finish_coro_read_result;

	TRACE_PQRESULTSTATUS;
	status = PQresultStatus(result);
        imp_dbh->last_result_status = status;
	if (!IS_PG_OK_STATUS(status)) {
		imp_dbh->coro_error = COERR_PGRESULT;
                TRACE_PQRESULTERRORFIELD;
                imp_dbh->last_result_err = PQresultErrorField(result,PG_DIAG_MESSAGE_PRIMARY);
		has_error = true;
	}
	if (status == PGRES_COPY_OUT || status == PGRES_COPY_IN) {
		if (TCORO) TRC(DBILOGFP, "%shandle entered copy state\n", THEADER);
		goto finish_coro_read_result;
	}

	// Make sure to consume all results.  Loop also exits if coro is
	// interrupted.
	while ((next_result = coro_read_one_result(aTHX_ imp_dbh, false))) {
		// just keep consuming if we've hit an error.
		// Only the first result should be returned.
		if (has_error) {
			_pg_warn(aTHX_ imp_dbh, "coro_read_result: cleaned-up extra result\n");
			TRACE_PQCLEAR;
			PQclear(next_result);
			continue;
		}

		TRACE_PQCLEAR;
		PQclear(result);
		result = next_result;

		TRACE_PQRESULTSTATUS;
		status = PQresultStatus(result);
                imp_dbh->last_result_status = status;
		if (!IS_PG_OK_STATUS(status)) {
			imp_dbh->coro_error = COERR_PGRESULT;
                        TRACE_PQRESULTERRORFIELD;
                        imp_dbh->last_result_err = PQresultErrorField(result,PG_DIAG_MESSAGE_PRIMARY);
			has_error = true;
		}
	}

finish_coro_read_result:
	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_read_result %d\n", THEADER, imp_dbh->socket_fd);
	return result;
}

static const char const *_error_message(const imp_dbh_t *imp_dbh)
{
	// disabled: only needed for TRACE_PQERRORMESSAGE
	// dTHX;
	switch (imp_dbh->coro_error) {
		case COERR_OK:
		case COERR_PGFATAL:
			// TRACE_PQERRORMESSAGE; 
			return PQerrorMessage(imp_dbh->conn);
		case COERR_EXTRA_RESULT:
			return "Got an unexpected extra result!";
		case COERR_INTERRUPTED:
			return "Fatal interrupt during non-blocking wait.";
		case COERR_FATAL:
			return "Fatal error during non-blocking wait.";
                case COERR_PGRESULT:
                        return (imp_dbh->last_result_err) ? imp_dbh->last_result_err : "Unknown statement error";
	}
	return NULL;
}

/* Put a call to "s = PQsendXxx(...)" between a BEGIN and END.  It won't run
 * if the connection is in an error state.
 */
#define CORO_PGRESULT_BEGIN \
if (imp_dbh->coro_error == COERR_OK) {
	int s = 0;

/* A "1" status could mean we couldn't flush to the socket.  Only if that
 * works should we read the result.
 */
#define CORO_PGRESULT_END \
	if (s != 1 && coro_flush(aTHX_ imp_dbh)) { s = -1; } \
	if (s == 1) { result = coro_read_result(aTHX_ imp_dbh); } \
}

/* From the Pg docs for PQexec:
 *  [the sql param] is allowed to include multiple SQL commands (separated by
 *  semicolons) in the command string. Multiple queries sent in a single
 *  PQexec call are processed in a single transaction, unless there are
 *  explicit BEGIN/COMMIT commands included in the query string to divide it
 *  into multiple transactions. Note however that the returned PGresult
 *  structure describes only the result of the last command executed from the
 *  string. Should one of the commands fail, processing of the string stops
 *  with it and the returned PGresult describes the error condition.
 */

static PGresult *coro_PQexec(pTHX_ imp_dbh_t *imp_dbh, const char * sql)
{
	PGresult *result = NULL;

	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_PQexec %d: %s\n", THEADER, imp_dbh->socket_fd, sql);

	CORO_PGRESULT_BEGIN;

	TRACE_PQSENDQUERY;
	s = PQsendQuery(imp_dbh->conn, sql);

	CORO_PGRESULT_END;

	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_PQexec %d (%s)\n", THEADER, imp_dbh->socket_fd, _error_message(imp_dbh));
	return result;
}

static PGresult *coro_PQexecParams(pTHX_ imp_sth_t *imp_sth, const char *statement)
{
	D_imp_dbh_from_sth;
	PGresult *result = NULL;

	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_PQexecParams %d\n", THEADER, imp_dbh->socket_fd);

	CORO_PGRESULT_BEGIN;

	TRACE_PQSENDQUERYPARAMS;
	s = PQsendQueryParams(imp_dbh->conn, statement, imp_sth->numphs, imp_sth->PQoids, imp_sth->PQvals, imp_sth->PQlens, imp_sth->PQfmts, 0);

	CORO_PGRESULT_END;

	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_PQexecParams %d (%s)\n", THEADER, imp_dbh->socket_fd, _error_message(imp_dbh));
	return result;
}

static PGresult *coro_PQprepare (pTHX_ imp_sth_t *imp_sth, const char *statement, int params)
{
	D_imp_dbh_from_sth;
	PGresult *result = NULL;

	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_PQprepare %d\n", THEADER, imp_dbh->socket_fd);

	CORO_PGRESULT_BEGIN;

	TRACE_PQSENDPREPARE;
	s = PQsendPrepare(imp_dbh->conn, imp_sth->prepare_name, statement, params, imp_sth->PQoids);

	CORO_PGRESULT_END;

	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_PQprepare %d (%s)\n", THEADER, imp_dbh->socket_fd, _error_message(imp_dbh));
	return result;
}

static PGresult *coro_PQexecPrepared(pTHX_ imp_sth_t *imp_sth)
{
	D_imp_dbh_from_sth;
	PGresult *result = NULL;

	if (TSTART || TCORO) TRC(DBILOGFP, "%sBegin coro_PQexecPrepared %d\n", THEADER, imp_dbh->socket_fd);

	CORO_PGRESULT_BEGIN;

	TRACE_PQSENDQUERYPREPARED;
	s = PQsendQueryPrepared(imp_dbh->conn, imp_sth->prepare_name, imp_sth->numphs, imp_sth->PQvals, imp_sth->PQlens, imp_sth->PQfmts, 0);

	CORO_PGRESULT_END;

	if (TEND || TCORO) TRC(DBILOGFP, "%sEnd coro_PQexecPrepared %d (%s)\n", THEADER, imp_dbh->socket_fd, _error_message(imp_dbh));
	return result;
}

/* end of dbdimp.c */
