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

/* TODO:
 * - Test transaction grouping (how?)
 * - Test SQLITE_BUSY handling
 */

#ifdef NDEBUG
#error These tests should not be compiled with -DNDEBUG!
#endif


#include "sqlasync.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>


/* These checks are implemented as macros to make error reporting with assert()
 * more useful. */

#define check_done_res(_q) do {\
		sqlasync_result_t *_r = sqlasync_queue_get(_q);\
		assert(_r->result == SQLITE_DONE && _r->numcol == 0 && _r->last);\
		sqlasync_result_free(_r);\
	} while(0)

#define check_err_res(_q) do {\
		sqlasync_result_t *_r = sqlasync_queue_get(_q);\
		assert(_r->result != SQLITE_DONE && _r->result != SQLITE_OK && _r->numcol == 1 && _r->last);\
		assert(_r->col[0].type == SQLITE_TEXT && _r->col[0].val.ptr);\
		sqlasync_result_free(_r);\
	} while(0)


/* Verifies that the results sent over the given queue are the same as what the
 * following query would give:
 *   SELECT NULL, 125, 123.5, 'String', zeroblob(0), X'ffaa00ff'
 */
#define check_canon_res(_q) do {\
		sqlasync_result_t *_r = sqlasync_queue_get(_q);\
		assert(_r->result == SQLITE_ROW && _r->numcol == 6 && !_r->last);\
		assert(_r->col[0].type == SQLITE_NULL);\
		assert(_r->col[1].type == SQLITE_INTEGER && _r->col[1].val.i64 == 125);\
		assert(_r->col[2].type == SQLITE_FLOAT   && _r->col[2].val.dbl == 123.5);\
		assert(_r->col[3].type == SQLITE_TEXT    && strcmp(_r->col[3].val.ptr, "String") == 0);\
		assert(_r->col[4].type == SQLITE_BLOB    && _r->col[4].length == 0 && _r->col[4].val.ptr == NULL);\
		assert(_r->col[5].type == SQLITE_BLOB    && _r->col[5].length == 4 && memcmp(_r->col[5].val.ptr, "\xff\xaa\x00\xff", 4) == 0);\
		sqlasync_result_free(_r);\
		check_done_res(_q);\
	} while(0)


static void test_sql() {
	sqlasync_t *sql = sqlasync_create(NULL);
	sqlasync_queue_t *q = sqlasync_queue_sync(), *qr = sqlasync_queue_sync();
	sqlasync_queue_buffersize(qr, 1);
	sqlasync_result_t *r;
	int i;

	/* Should return an error */
	assert(sqlasync_open(sql, q, q, "abcdeffg", SQLITE_OPEN_READONLY) == q);
	r = sqlasync_queue_get(q);
	assert(r && r->result != SQLITE_OK && r->last);
	sqlasync_result_free(r);

	/* Closing when nothing is opened is a no-op */
	sqlasync_close(sql);

	sqlasync_open(sql, q, q, ":memory:", 0);
	r = sqlasync_queue_get(q);
	assert(r->result == SQLITE_OK && r->last);
	sqlasync_result_free(r);

	/* The canonical query */
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "SELECT NULL, 125, 123.5, 'String', zeroblob(0), X'ffaa00ff'", 0);
	check_canon_res(qr);

	/* The same query, constructed using bind values */
	sqlasync_sql(sql, qr, 0, "SELECT ?, ?, ?, ?, ?, ?", 6,
			sqlasync_null(),
			sqlasync_int(125),
			sqlasync_float(123.5),
			sqlasync_text(0, "String"),
			sqlasync_blob(0, 0, NULL),
			sqlasync_blob(SQLASYNC_COPY, 4, "\xff\xaa\x00\xff"));
	check_canon_res(qr);

	/* Queue and fetch multiple queries */
	for(i=0; i<100; i++)
		sqlasync_sql(sql, qr, 0, "SELECT ?", 1, sqlasync_int(i));
	for(i=0; i<100; i++) {
		r = sqlasync_queue_get(qr);
		assert(r->result == SQLITE_ROW && r->numcol == 1 && !r->last);
		assert(r->col[0].type == SQLITE_INTEGER && r->col[0].val.i64 == i);
		sqlasync_result_free(r);
		check_done_res(qr);
	}

	/* "Empty" queries */
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "", 0);
	check_done_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "   ", 0);
	check_done_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "/* comment */", 0);
	check_done_res(qr);

	sqlasync_sql(sql, qr, SQLASYNC_STATIC|SQLASYNC_SINGLE, "CREATE TABLE sqlasync_a (x UNIQUE)", 0);
	check_done_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC|SQLASYNC_SINGLE, "INSERT INTO sqlasync_a VALUES ('s')", 0);
	check_done_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC|SQLASYNC_SINGLE, "CREATE TABLE sqlasync_b (x UNIQUE)", 0);
	check_done_res(qr);

	/* Some errors */
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "CREATE TABLE sqlasync_a (a)", 0);
	check_err_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "SELECT * FROM sqlasync_noexist", 0);
	check_err_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "INSERT INTO sqlasync_a VALUES ('s')", 0); /* dupe */
	check_err_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC|SQLASYNC_SINGLE, "INSERT INTO sqlasync_a VALUES ('s')", 0); /* dupe */
	check_err_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC|SQLASYNC_SINGLE, "NONEXISTINGQUERY", 0);
	check_err_res(qr);
	sqlasync_sql(sql, qr, SQLASYNC_STATIC, "SELECT '", 0);
	check_err_res(qr);


	/* NEXT chaining */
	sqlasync_lock(sql);
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "INSERT INTO sqlasync_b VALUES (87)", 0); /* 1 */
	/* This query fails in _prepare(), doesn't abort the transaction */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "SELECT '", 0); /* 2 */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "", 0); /* 3 */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "SELECT 1 LIMIT 0", 0); /* 4 */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "SELECT COUNT(x), MAX(x) FROM sqlasync_b", 0); /* 5 */
	/* This query fails in _step(), should abort the transaction */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "INSERT INTO sqlasync_b VALUES (87)", 0); /* 6 */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC|SQLASYNC_NEXT, "", 0); /* 7 */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC, "SELECT 1 LIMIT 0", 0); /* 8 */
	/* Transaction aborted, table should be empty. (Doesn't have to be done in the lock, but whatever) */
	sqlasync_sql_unlocked(sql, qr, SQLASYNC_STATIC, "SELECT * FROM sqlasync_b", 0); /* 9 */
	sqlasync_unlock(sql);

	check_done_res(qr); /* 1 */
	check_err_res(qr); /* 2 */
	check_done_res(qr); /* 3 */
	check_done_res(qr); /* 4 */
	r = sqlasync_queue_get(qr); /* 5 */
	assert(r->result == SQLITE_ROW && r->numcol == 2 && !r->last);
	assert(r->col[0].type == SQLITE_INTEGER && r->col[0].val.i64 == 1);
	assert(r->col[1].type == SQLITE_INTEGER && r->col[1].val.i64 == 87);
	sqlasync_result_free(r);
	check_done_res(qr);
	check_err_res(qr); /* 6 */
	check_err_res(qr); /* 7 */
	check_err_res(qr); /* 8 */
	check_done_res(qr); /* 9 */


	sqlasync_destroy(sql);
	sqlasync_queue_destroy(q);
	sqlasync_queue_destroy(qr);
}




static int schedcount = 0;
static int event = 0;
static int asyncpipe[2];
static sqlasync_t *asyncsql;
static sqlasync_wakeup_t *asyncw;


static void async_slowreply(sqlasync_t *sql, sqlite3 *db, sqlasync_queue_t *q, int val_num, sqlasync_value_t *values) {
	assert(sql == asyncsql);
	assert(val_num == 1);
	assert(values[0].type == SQLITE_INTEGER);
	assert(db != NULL);

	int i;
	for(i=values[0].val.i64; i>0; i--) {
		sleep(1);
		sqlasync_result_t *r = sqlasync_result_create(SQLITE_ROW, 0, 1);
		r->col[0] = sqlasync_int(i);
		sqlasync_queue_result(q, r);
	}

	sqlasync_queue_result(q, sqlasync_result_create(SQLITE_DONE, 1, 0));
}


static void async_wakeup(sqlasync_wakeup_t *w, void *data) {
	assert(data == asyncpipe);
	assert(w == asyncw);
	assert(schedcount == 1);
	schedcount = 0;
	assert(write(asyncpipe[1], "", 1) == 1);
}


static void async_schedule(sqlasync_wakeup_t *w, void *data) {
	assert(data == asyncpipe);
	assert(w == asyncw);
	schedcount++;
}


static void async_result(sqlasync_queue_t *q, void *data) {
	int i;
	assert(q != NULL);
	sqlasync_result_t *r = sqlasync_queue_get(q);
	assert(r != NULL);

	switch(event) {
	case 0:
		assert(data == 0);
		assert(r->result == SQLITE_OK && r->numcol == 0 && r->last);
		assert(sqlasync_queue_get(q) == NULL);
		sqlasync_queue_destroy(q);
		/* Single operation with multiple results, should be received in one go
		 * regardless of how long it takes. */
		sqlasync_custom(asyncsql, sqlasync_queue_async(asyncw, 0, async_result, (void*)1),
				async_slowreply, 1, sqlasync_int(3));
		break;
	case 1:
		assert(data == (void*)1);
		for(i=3; i>0; i--) {
			assert(r != NULL);
			assert(r->result == SQLITE_ROW && r->numcol == 1 && !r->last);
			assert(r->col[0].type == SQLITE_INTEGER && r->col[0].val.i64 == i);
			sqlasync_result_free(r);
			r = sqlasync_queue_get(q);
		}
		assert(r->result == SQLITE_DONE && r->numcol == 0 && r->last);
		sqlasync_queue_destroy(q);
		/* Queue two queries */
		sqlasync_sql(asyncsql, sqlasync_queue_buffersize(sqlasync_queue_async(asyncw, 1, async_result, (void*)2), 1),
				SQLASYNC_STATIC, "select 1 as id union select 2 union select 3 order by id desc", 0);
		sqlasync_sql(asyncsql, sqlasync_queue_async(asyncw, 1, async_result, (void*)3),
				SQLASYNC_STATIC, "select '", 0);
		break;
	case 2:
		assert(data == (void*)2);
		assert(r->result == SQLITE_ROW && r->numcol == 1 && !r->last);
		assert(r->col[0].type == SQLITE_INTEGER && r->col[0].val.i64 == 3);
		/* Destroy the queue after the first result.
		 * TODO: Also try this with the async_slowreply function to test
		 * deferred-free. */
		sqlasync_queue_destroy(q);
		break;
	case 3:
		assert(data == (void*)3);
		assert(r->result != SQLITE_DONE && r->result != SQLITE_OK && r->numcol == 1 && r->last);
		assert(r->col[0].type == SQLITE_TEXT && r->col[0].val.ptr);
		sqlasync_queue_destroy(q);
		sqlasync_close(asyncsql);
		break;
	case 4:
		assert(data == (void*)4);
		assert(r->result == SQLITE_OK && r->numcol == 0 && r->last);
		assert(sqlasync_queue_get(q) == NULL);
		sqlasync_queue_destroy(q);
		sqlasync_destroy(asyncsql);
		break;
	}
	event++;
	sqlasync_result_free(r);
}


static void test_async() {
	assert(pipe(asyncpipe) == 0);
	asyncw = sqlasync_wakeup_create(async_wakeup, async_schedule, asyncpipe);

	asyncsql = sqlasync_create(NULL);
	sqlasync_open(asyncsql,
			sqlasync_queue_async(asyncw, 1, async_result, 0),
			sqlasync_queue_async(asyncw, 1, async_result, (void*)4),
			":memory:", 0);

	while(event <= 4) {
		char buf[2];
		assert(read(asyncpipe[0], buf, 2) == 1);
		assert(schedcount == 0);
		sqlasync_dispatch(asyncw);
	}
	sqlasync_wakeup_destroy(asyncw);
}




int main(int argc, char **argv) {
	test_sql();
	test_async();
	return 0;
}

/* vim: set noet sw=4 ts=4: */
