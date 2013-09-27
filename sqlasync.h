/* Copyright (c) 2013 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/* This library implements a background thread and message queue in order to
 * communicate with an SQLite3 database asynchronously. It can be used to
 * serialize database operations from multiple application threads and/or to
 * perform database operations with an event loop without blocking.
 *
 * The main concepts and reasoning for this library have been described in
 *   http://dev.yorhel.nl/doc/sqlaccess
 *
 * This library requires at least SQLite 3.5.0 (2007-09-04), but it does not
 * have to be compiled with thread safety enabled. Your compiler must
 * understand C99 (C++ likely won't work), and your system needs to support
 * clock_gettime() and pthread_condattr_setclock() with CLOCK_MONOTONIC.
 * Patches to relax any of these requirements are of course welcome.
 *
 * TODO:
 * - Cache prepared statements
 * - Don't create query results if the application has specified a NULL result
 *   queue or has called sqlasync_queue_destroy().
 * - Separate the result queue handling abstraction into a different library?
 *   It may be useful for more stuff.
 * - Handle and report malloc failure
 */

#ifndef SQLASYNC_H
#define SQLASYNC_H

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include <sqlite3.h>


/* When passing a memory buffer (SQL query, or a SQLITE3_TEXT or SQLITE_BLOB
 * bind value) to a function, the following flags can be used to specify how
 * that memory should be managed. */
typedef enum {
	/* Make an immediate copy of the buffer for internal use. */
	SQLASYNC_COPY = 0,
	/* Pass ownership of the buffer to sqlasync. The buffer will be free()'d
	 * after use. */
	SQLASYNC_FREE = 1,
	/* The buffer is assumed to stay alive and unmodified for as long as it is
	 * referenced. Note that the application usually has no idea how long that
	 * is, so this makes most sense for static strings and the like. */
	SQLASYNC_STATIC = 2
} sqlasync_bufmanage_t;

typedef struct sqlasync_t sqlasync_t;




/* Generic SQLite value, used for query binding and passing back query results.
 *
 * The sqlasync_value_t struct is always passed around by value or embedded
 * directly into a containing structure/array.
 */

typedef struct {
	unsigned int type : 3; /* SQLITE_INTEGER/FLOAT/BLOB/NULL/TEXT */
	unsigned int freeptr : 1;
	unsigned int length; /* Only used for BLOB. TEXT values are always zero-terminated */
	union {
		void *ptr;         /* BLOB, TEXT */
		sqlite3_int64 i64; /* INTEGER */
		double dbl;        /* FLOAT */
	} val;
	/* Note that val.ptr may be NULL for zero-length BLOBs, as that's what
	 * sqlite_column_blob() gives back. For bind values, you can also provide a
	 * NULL value as val.ptr to bind a zeroblob. */
} sqlasync_value_t;


/* Convenient functions to create a value */

static inline sqlasync_value_t sqlasync_null() {
	sqlasync_value_t r = { SQLITE_NULL, 0, 0, { 0 } };
	return r;
}

static inline sqlasync_value_t sqlasync_int(sqlite3_int64 val) {
	sqlasync_value_t r = { SQLITE_INTEGER, 0, 0, { .i64 = val } };
	return r;
}

static inline sqlasync_value_t sqlasync_float(double val) {
	sqlasync_value_t r = { SQLITE_FLOAT, 0, 0, { .dbl = val } };
	return r;
}

static inline sqlasync_value_t sqlasync_text(sqlasync_bufmanage_t m, const char *str) {
	sqlasync_value_t r = { SQLITE3_TEXT, m != SQLASYNC_STATIC, 0, { 0 } };
	if(m == SQLASYNC_COPY) {
		r.val.ptr = malloc(strlen(str)+1);
		strcpy(r.val.ptr, str);
	} else
		r.val.ptr = (void *)str;
	return r;
}

static inline sqlasync_value_t sqlasync_blob(sqlasync_bufmanage_t m, unsigned int length, const void *buf) {
	sqlasync_value_t r = { SQLITE_BLOB, m != SQLASYNC_STATIC, length, { 0 } };
	if(buf && m == SQLASYNC_COPY) {
		r.val.ptr = malloc(length);
		memcpy(r.val.ptr, buf, length);
	} else
		r.val.ptr = (void *)buf;
	return r;
}




/* Wakeup object for event loop integration.
 *
 * A wakeup object is used for integrating the dispatching of asynchronous
 * results with an event loop. The same sqlasync_wakeup_t object can be used
 * with multiple sqlasync_queue_t and sqlasync_t objects.
 */

typedef struct sqlasync_wakeup_t sqlasync_wakeup_t;
typedef void(*sqlasync_wakeup_func_t)(sqlasync_wakeup_t *wakeup, void *data);

/* Creates a new wakeup object. The wakeup callback should schedule a call to
 * sqlasync_dispatch() in the near future (e.g. next event loop iteration). The
 * callback will be called only once when there is stuff to dispatch. It will
 * not be called again until after sqlasync_dispatch() has returned. The
 * callback will be run from the context of the database thread or from
 * sqlasync_queue_get() [1]. It should not call any sqlasync_ functions itself.
 *
 * The schedule callback will be called to indicate that something has been
 * scheduled, before the wakeup callback is called. This can be useful for
 * registering a poll watcher with the event loop or for reference counting.
 * The schedule callback will only be run from the context of another public
 * sqlasync_*() function associated with the wakeup object, for example from
 * sqlasync_sql() with a queue object created by sqlasync_queue_async() with
 * the wakeup object. This means that, as long as you only use the wakeup
 * object from a single thread, the schedule callback will only be called in
 * that same thread. It is still not allowed to call any sqlasync_*() functions
 * from this callback, because it may be called while a lock is held. The
 * schedule callback may be NULL if you don't need it.
 *
 * 1. Only when sqlasync_queue_get() is called from outside of a result
 *    callback and it returned the last result scheduled for the wakeup object.
 */
sqlasync_wakeup_t *sqlasync_wakeup_create(sqlasync_wakeup_func_t wakeup, sqlasync_wakeup_func_t schedule, void *data);

/* Free the wakeup object. It is an error to call this function from a wakeup,
 * schedule or result callback, and it should not be called when an action has
 * been scheduled for this wakeup object. */
void sqlasync_wakeup_destroy(sqlasync_wakeup_t *wakeup);

/* Should be called after receiving the wakeup callback. This function will in
 * turn invoke callbacks registered with sqlasync_queue_async(). */
void sqlasync_dispatch(sqlasync_wakeup_t *wakeup);




/* Obtaining query results.
 *
 * Query results should always have a single reader, it is an error to call
 * sqlasync_queue_* functions on the same sqlasync_queue_t object from multiple
 * threads simultaneously.
 *
 * The same results queue can be used for multiple queries. The results are
 * always returned in FIFO order; If you use sqlasync_sql() twice with the same
 * queue, the results of the first query are given first, followed by the
 * results on the second query. The `last' field functions as a separator
 * between the results of different queries.
 *
 * A single result queue can only be used with a single sqlasync_t object at a
 * time. You must ensure that all results have been consumed from the queue
 * before re-using it with another sqlasync_t object.
 * (It's probably easier to just use multiple queue objects...)
 */

typedef struct sqlasync_queue_t sqlasync_queue_t;

typedef void(*sqlasync_result_func_t)(sqlasync_queue_t *q, void *data);

typedef struct sqlasync_result_t sqlasync_result_t;
struct sqlasync_result_t {
	/* Used internally, always NULL by the time the application sees this
	 * object. */
	sqlasync_result_t *next;
	sqlasync_queue_t *queue;
	/* SQLITE_* result code. */
	int result : 31;
	/* if 1, then this is the last result for this query or operation */
	unsigned int last : 1;
	/* Query results / columns */
	unsigned int numcol;
	sqlasync_value_t col[];
};


/* Create a new queue for receiving results synchronously. "Synchronous" here
 * means that sqlasync_queue_get() will block, but it's not truly synchronous
 * in the sense that the database thread will block while processing the query.
 * The result queue still buffers an unbounded number of results. */
sqlasync_queue_t *sqlasync_queue_sync();

/* Create a new asynchronous result queue.
 *
 * If `each' is 0, then the callback will only be invoked when there is a
 * result available with the `last' flag set. That is, when all results for a
 * particular query are available. If `each' is 1, the callback will be invoked
 * as soon as any result is available.
 *
 * The callback must use sqlasync_queue_destroy() or call sqlasync_result_get()
 * at least once, but may call it multiple times if there is more than one
 * result available. The callback is level-triggered, it will be called for as
 * long as there are still results queued.
 */
sqlasync_queue_t *sqlasync_queue_async(sqlasync_wakeup_t *wakeup, int each, sqlasync_result_func_t func, void *data);

/* Set a maximum size on the number of results in this queue.  This function
 * should be called after creating the queue and before the queue has been used
 * for anything. If len=0, then the queue size is unbounded (default). Using
 * any other value will cause the sqlite thread to wait with processing more
 * data until a result has been read from the queue with sqlasync_queue_get().
 *
 * Although this functionality is useful, and in some cases even necessary, to
 * put a limit on the memory used by passing around query results, you should
 * be aware of the implications of setting this limit. If the thread which
 * reads the queue results somehow fails to make progress, then the sqlite
 * thread will not be able to process further queries and may in turn cause a
 * deadlock situation. Similarly, using this function on an async queue with
 * `each' set to 0 will result in a deadlock if more than this number of
 * results are queued as part of a single query.
 */
sqlasync_queue_t *sqlasync_queue_buffersize(sqlasync_queue_t *q, unsigned int len);

/* Get a result from the queue. The behaviour of this function when the queue
 * is empty depends on how the queue was created.
 *
 * If it was created with sqlasync_queue_sync(), then this function will block
 * until a result becomes available.
 *
 * If it was created with sqlasync_queue_async(), then this function will
 * always return immediately. NULL is returned if no results are available at
 * this moment.
 *
 * The returned object should be freed with sqlasync_result_free().
 */
sqlasync_result_t *sqlasync_queue_get(sqlasync_queue_t *q);

/* Discard any (old or new) results and free the queue. */
void sqlasync_queue_destroy(sqlasync_queue_t *q);

/* Free a result obtained with sqlasync_queue_get().
 *
 * This function frees any buffers associated with sqlasync_value_t objects. If
 * you want to take control over a specific buffer, make sure to reset its
 * `freeptr' value to 0 before calling this function.
 */
void sqlasync_result_free(sqlasync_result_t *res);




/* Create a new thread. A thread can be used to interact with one SQLite
 * connection at a time.
 *
 * If `transtimeout' is not NULL, multiple queries executed in this database
 * thread within the specified timeout will be grouped together in a single
 * transaction. There are a few important things to keep in mind when using
 * this functionality:
 * - SQLite may keep an exclusive lock on the database file for as long the
 *   transaction is open. This means that, if this database is also used from
 *   other processes, those other processes may get blocked for the timeout
 *   specified here, or more, depending on how many processes use the database.
 *   In such a case, you will want to keep this timeout rather small, say,
 *   100ms, depending on what kind of timing requirements you need.
 *   If you don't expect other processes to access the same database, then a
 *   timeout in the order of 5 to 10 seconds will probably be fine and may
 *   result in significantly better performance.
 * - If your queries make modifications to the database, then these
 *   modifications may not be flushed to disk until after the timeout has
 *   expired (or after the next flush, see next point). This means that if your
 *   application crashes or otherwise doesn't shut down orderly, any
 *   modifications before the last flush will be lost.
 * - A transaction may be flushed before the timeout expired. This happens when
 *   the database is closed or when a query with the SQLASYNC_LAST or
 *   SQLASYNC_SINGLE flag is processed.
 * - Since the actual database modifications for a query are deferred, such
 *   modifications may be lost even if the query resulted in an
 *   SQLITE_OK/SQLITE_DONE. Errors when committing such a transaction are
 *   reported on the queue given to sqlasync_open().
 * - If one query fails with an error while a transaction is active, the entire
 *   transaction will be rolled back and any modifications will be lost.
 */
sqlasync_t *sqlasync_create(const struct timespec *transtimeout);

/* Opens an SQLite database. It is an error to call this function on an
 * sqlasync_t object which already has an SQLite database opened. (You can
 * always use a "ATTACH DATABASE" query if you want to handle multiple
 * databases with a single sqlasync thread).
 *
 * The `filename' argument will be copied internally.
 *
 * The `open_flags' argument is passed as-is to sqlite3_open_v2(). If its value
 * is 0, sqlite3_open() will be used instead.
 *
 * The result of the sqlite3_open() or sqlite3_open_v2() call is passed back on
 * the first queue. If the open failed, the `result' will indicate the error
 * code, and the error message is passed in the first column as SQLITE3_TEXT.
 *
 * The second queue is used for reporting asynchronous errors that are not
 * directly linked to a certain action. This only happens when a COMMIT has
 * failed that has been initiated as part of a transaction timeout (see
 * sqlasync_create()) or as part of an SQLASYNC_SINGLE query or
 * sqlasync_close() or sqlasync_destroy(). Such an error is not fatal for the
 * database connection, but may indicate the modifications of earlier queries
 * to have been rolled back. If the transtimeout given to sqlasync_create() is
 * NULL, then no such errors are possible.
 *
 * An SQLITE_OK result with the `last' flag set will be passed to the second
 * queue when the database connection has been closed, either because the open
 * failed or after sqlasync_close() has been processed.
 *
 * You should not use a queue created with sqlasync_queue_async() with `each'
 * set to 0 for the second queue, otherwise you will not receive any errors
 * before the database has been closed.  Similarly, you can't re-use this queue
 * for other operations until the database has been closed.
 *
 * Either or both of the queues can be NULL if you're not interested in the
 * messages. You can also pass the same queue twice, in which case the result
 * of the open will be passed first (with the `last' flag set), followed by
 * whatever the second queue would receive.
 */
sqlasync_queue_t *sqlasync_open(sqlasync_t *sql, sqlasync_queue_t *q, sqlasync_queue_t *errq, const char *filename, int open_flags);

/* Closes the database. This is a no-op is no database is currently open. If a
 * database has been closed successfully, a SQLITE_OK result with `last' set is
 * passed to the second queue given to sqlasync_open(). */
void sqlasync_close(sqlasync_t *sql);

/* Stops the database thread and frees any resources. This function blocks
 * until any queued operations are finalized. If the database is still open
 * after all queued operations are finalized, it will be closed in the same way
 * as if sqlasync_close() has been called before sqlasync_destroy(). */
void sqlasync_destroy(sqlasync_t *sql);


/* Flags for sqlasync_sql() and sqlasync_sql_unlocked(). Can be used together
 * with the sqlasync_bufmanage_t flags to control the memory of the query
 * string argument. */
typedef enum {
	/* The query must be executed in the same transaction as the next query in
	 * the queue.  When using this flag, make sure to add this and the next
	 * query inside a sqlasync_lock(), to ensure that the database thread has a
	 * consistent view of the query chain.
	 * If any query inside a NEXT chain fails, the transaction is aborted and
	 * all subsequent queries in the chain will get an SQLITE_ERROR result.
	 * If the sqlasync_t object has been created with transtimeout = NULL, then
	 * the last query in the chain will behave as if it is given the
	 * SQLASYNC_LAST flag. Otherwise, subsequent queries may be grouped in the
	 * same transaction. */
	SQLASYNC_NEXT   = (1<<2),
	/* Any active transaction must end with this query. The `last' result will
	 * only be passed back after the transaction has been flushed to disk. */
	SQLASYNC_LAST   = (2<<2),
	/* The query must be executed outside of a transaction. This flag has no
	 * effect if the sqlasync_t object has been created with a NULL
	 * transtimeout, since with that it's the default. */
	SQLASYNC_SINGLE = (3<<2)
} sqlasync_flags_t;


/* Perform an SQL query on the database. The `flags' arguments can be flags
 * from sqlasync_bufmanage_t and sqlasync_flags_t, as described above.
 *
 * `bind_num' Specifies the number of bind values you wish to pass. If bind_num
 * > 0, then you should provide exactly that number of sqlasync_value_t
 * structures as variable arguments. For example:
 *
 *   sqlasync_sql(s, q, 0, "SELECT ?, ?", 2, sqlasync_null(), sqlasync_int(5));
 *
 * The results are passed back to the given queue. Pass a NULL queue if you're
 * not interested in the results. Note that query results are fetched and
 * processed regardless of whether you pass a queue. If you wish to ignore any
 * query results, make sure that the query does not return any rows. Use a
 * "LIMIT 0" clause, for example.
 *
 * The results are passed back as follows. All rows are passed with `result' ==
 * SQLITE_ROW. `colnum' and the column types are inferred from the query using
 * sqlite3_column_count() and sqlite3_column_type(). After that, a result with
 * `result' == SQLITE_DONE and the `last' flag set will be passed to indicate
 * the end of the result set.
 *
 * If an error occured at any point before receiving SQLITE_DONE, then this
 * will be passed back with the `last' flag set and `result' indicating the
 * error code. The error result will have `numcol' == 1, and the first column
 * is a human-readable error string obtained from sqlite3_errmsg().
 */
sqlasync_queue_t *sqlasync_sql(sqlasync_t *sql, sqlasync_queue_t *q,
		int flags, const char *query, int bind_num, ...);


/* The functions below are for locked access to the SQL queue. This is useful
 * if you want a set of queries to be executed as a sequence. While locked,
 * other threads will not be able to queue SQL queries, and the database thread
 * will not be able to process new queries.
 *
 * Query chains with the SQLASYNC_NEXT flags _must_ be queued from a single
 * critical section. For example:
 *
 * 	 sqlasync_lock(s);
 * 	 sqlasync_sql_unlocked(s, q, SQLASYNC_NEXT, "query a", ..);
 * 	 sqlasync_sql_unlocked(s, q, SQLASYNC_NEXT, "query b", ..);
 * 	 sqlasync_sql_unlocked(s, q, SQLASYNC_NEXT, "query c", ..);
 * 	 sqlasync_sql_unlocked(s, q, 0,             "query d", ..);
 * 	 sqlasync_unlock(s);
 */
void sqlasync_lock(sqlasync_t *sql);

void sqlasync_unlock(sqlasync_t *sql);

sqlasync_queue_t *sqlasync_sql_unlocked(sqlasync_t *sql, sqlasync_queue_t *q,
		int flags, const char *query, int bind_num, ...);

sqlasync_queue_t *sqlasync_sqlv_unlocked(sqlasync_t *sql, sqlasync_queue_t *q,
		int flags, const char *query, int bind_num, va_list binds);




/* The functions below can be used to access sqlite3 functionality not exposed
 * by the above API, such as incremental BLOB I/O, sqlite3_create_function() or
 * sqlite3_db_release_memory().
 *
 * TODO: Documentation.
 */

typedef void(*sqlasync_custom_func_t)(sqlasync_t *sql, sqlite3 *db, sqlasync_queue_t *q, int val_num, sqlasync_value_t *values);

sqlasync_result_t *sqlasync_result_create(unsigned short result, unsigned short last, unsigned int numcol);
void sqlasync_queue_result(sqlasync_queue_t *q, sqlasync_result_t *r);

sqlasync_queue_t *sqlasync_custom(sqlasync_t *sql, sqlasync_queue_t *q, sqlasync_custom_func_t f, int val_num, ...);

#endif

/* vim: set noet sw=4 ts=4: */
