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

#include "sqlasync.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>



/* A simple FIFO queue abstraction. The queue struct is assumed to have a
 * `first' and `last' pointer, and the elements must have a `next' pointer.
 * Entries are inserted in `last' and processed starting from `first' */

/* Append a list starting with _f and ending with _l to _q. _l == _f for a
 * single item. */
#define queue_push(_q, _f, _l) do {\
		(_l)->next = NULL;\
		if((_q)->last)\
			(_q)->last->next = (_f);\
		else\
			(_q)->first = (_f);\
		(_q)->last = (_l);\
	} while(0)

/* Removes the first element from the queue. */
#define queue_pop(_q) do {\
		if(!(_q)->first->next)\
			(_q)->last = NULL;\
		(_q)->first = (_q)->first->next;\
	} while(0)




struct sqlasync_wakeup_t {
	pthread_mutex_t lock; /* Protects haswoken, numscheduled, first and last */
	sqlasync_wakeup_func_t wakeup;
	sqlasync_wakeup_func_t schedule;
	void *data;
	sqlasync_result_t *first;
	sqlasync_result_t *last;
	unsigned int numscheduled; /* Number of queries scheduled */
	unsigned int haswoken;
};


/* Note: Be careful not to interleave read-only members (sync,each, etc) with
 * read-write members protected by a mutex (first, last, etc). A write may not
 * be atomic and may temporarily access other fields, too. */
struct sqlasync_queue_t {
	unsigned int sync : 1;
	unsigned int each : 1;
	/* For async results */
	sqlasync_wakeup_t *wakeup;
	sqlasync_result_func_t func;
	void *data;
	/* The mutex protects everything below, but only for synchronous results.
	 * For asynchronous results, `cond' is not used and the other fields are
	 * protected by the wakeup object. */
	pthread_mutex_t lock;
	pthread_cond_t cond;
	/* The result queue. */
	sqlasync_result_t *first;
	sqlasync_result_t *last;
	unsigned int numscheduled;
	unsigned int destroyed;
	/* Holds the total number of queued results associated with this object. In
	 * the asynchronous case, that includes results queued in `first'/`last'
	 * in addition to those in queued in the wakeup object. */
	unsigned int numresults;
	unsigned int maxresults;
};


/* Special operation flags (Not supposed to be combined with other flags) */
#define SQLASYNC_OPEN    (1<<8)
#define SQLASYNC_CLOSE   (2<<8)
#define SQLASYNC_QUIT    (3<<8)
#define SQLASYNC_CUSTOM  (4<<8)

typedef struct sqlasync_op_t sqlasync_op_t;
struct sqlasync_op_t {
	sqlasync_op_t *next;
	sqlasync_queue_t *q;
	char *str;
	unsigned short flags;
	unsigned short numargs;
	sqlasync_value_t args[];
};


struct sqlasync_t {
	pthread_t thread;
	struct timespec transtimeout;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	sqlasync_op_t *first;
	sqlasync_op_t *last;

	sqlite3 *db;
	/* The queue given to sqlasync_open() */
	sqlasync_queue_t *dbqueue;
	/* Cached prepared staments for common queries */
	sqlite3_stmt *begin, *commit, *rollback;
	/* Time when the current transaction should be committed */
	struct timespec trans;
	/* Set when a transaction is currently open */
	unsigned int intrans : 1;
	/* We're in a SQLASYNC_NEXT chain, but the transaction had to be rolled back due to an error. */
	unsigned int errtrans : 1;
	/* Previous operation was a SQLASYNC_NEXT */
	unsigned int donext : 1;
};




sqlasync_wakeup_t *sqlasync_wakeup_create(sqlasync_wakeup_func_t wakeup, sqlasync_wakeup_func_t schedule, void *data) {
	sqlasync_wakeup_t *w = calloc(1, sizeof(sqlasync_wakeup_t));
	w->wakeup = wakeup;
	w->schedule = schedule;
	w->data = data;
	pthread_mutex_init(&w->lock, NULL);
	return w;
}


void sqlasync_wakeup_destroy(sqlasync_wakeup_t *w) {
	assert("Can't destroy a wakeup object while there are still events scheduled" && !w->numscheduled);
	pthread_mutex_destroy(&w->lock);
	free(w);
}


sqlasync_queue_t *sqlasync_queue_sync() {
	sqlasync_queue_t *q = calloc(1, sizeof(sqlasync_queue_t));
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->cond, NULL);
	q->sync = 1;
	q->maxresults = UINT_MAX;
	return q;
}


sqlasync_queue_t *sqlasync_queue_async(sqlasync_wakeup_t *wakeup, int each, sqlasync_result_func_t func, void *data) {
	sqlasync_queue_t *q = calloc(1, sizeof(sqlasync_queue_t));
	q->each = !!each;
	q->wakeup = wakeup;
	q->func = func;
	q->data = data;
	q->maxresults = UINT_MAX;
	return q;
}


sqlasync_queue_t *sqlasync_queue_buffersize(sqlasync_queue_t *q, unsigned int len) {
	/* Should be called before the queue is used, so no need to lock here */
	q->maxresults = len ? len : UINT_MAX;
	return q;
}


sqlasync_result_t *sqlasync_queue_get(sqlasync_queue_t *q) {
	sqlasync_result_t *res = NULL;
	int shouldwakeup = 0;
	pthread_mutex_t *lock = q->sync ? &q->lock : &q->wakeup->lock;
	pthread_mutex_lock(lock);

	if(q->sync) {
		while(!q->first)
			pthread_cond_wait(&q->cond, lock);
		res = q->first;
		queue_pop(q);
	} else if(q->wakeup->first && q->wakeup->first->queue == q) {
		res = q->wakeup->first;
		queue_pop(q->wakeup);
	}

	if(res) {
		q->numresults--;
		pthread_cond_signal(&q->cond);
		if(res->last) {
			q->numscheduled--;
			if(!q->sync && !--q->wakeup->numscheduled && !q->wakeup->haswoken)
				shouldwakeup = q->wakeup->haswoken = 1;
		}
		/* Application shouldn't use these, so let's reset them */
		res->queue = NULL;
		res->next = NULL;
	}

	pthread_mutex_unlock(lock);

	if(shouldwakeup)
		q->wakeup->wakeup(q->wakeup, q->wakeup->data);
	return res;
}


/* Called when an action has been scheduled for this queue. Always called from
 * the context of a public API function. */
static void sqlasync_queue_schedule(sqlasync_queue_t *q) {
	if(!q)
		return;
	int shouldsched = 0;
	pthread_mutex_t *lock = q->sync ? &q->lock : &q->wakeup->lock;
	pthread_mutex_lock(lock);
	q->numscheduled++;
	if(!q->sync)
		shouldsched = !q->wakeup->numscheduled++;
	pthread_mutex_unlock(lock);

	if(shouldsched && q->wakeup->schedule)
		q->wakeup->schedule(q->wakeup, q->wakeup->data);
}


/* Called only when the queue is empty and no more results are scheduled. */
static void sqlasync_queue_free(sqlasync_queue_t *q) {
	if(q->sync) {
		pthread_mutex_destroy(&q->lock);
		pthread_cond_destroy(&q->cond);
	}
	free(q);
}


void sqlasync_queue_destroy(sqlasync_queue_t *q) {
	if(!q)
		return;
	int shouldfree = 0;
	pthread_mutex_t *lock = q->sync ? &q->lock : &q->wakeup->lock;
	pthread_mutex_lock(lock);
	q->destroyed = 1;

	/* We can already immediately free the queue that is directly associated
	 * with this queue item. Results associated with a wakeup object are freed
	 * in the dispatch function. */
	while(q->first) {
		sqlasync_result_t *r = q->first;
		queue_pop(q);
		q->numresults--;
		/* There's no need to decrement q->wakeup->numscheduled if !q->sync,
		 * because this queue will act as temporary a buffer in that case and
		 * there will not be a 'last' result. */
		if(r->last)
			q->numscheduled--;
		sqlasync_result_free(r);
	}
	pthread_cond_signal(&q->cond);

	shouldfree = !q->numscheduled && !q->numresults;
	pthread_mutex_unlock(lock);

	if(shouldfree)
		sqlasync_queue_free(q);
}


/* Called from the database thread when a result has become available.
 * TODO: This is also called when no queue has been specified or when the queue
 * has already been destroyed.  It'd be much more efficient to communicate back
 * to the SQL processing part to stop creating result objects. */
void sqlasync_queue_result(sqlasync_queue_t *q, sqlasync_result_t *r) {
	if(!q) {
		sqlasync_result_free(r);
		return;
	}
	sqlasync_wakeup_t *w = q->wakeup;
	int shouldwakeup = 0, shouldfree = 0;
	pthread_mutex_t *lock = q->sync ? &q->lock : &q->wakeup->lock;
	pthread_mutex_lock(lock);

	if(q->destroyed) {
		if(r->last) {
			q->numscheduled--;
			if(!q->sync)
				shouldwakeup = !--w->numscheduled;
			shouldfree = !q->numscheduled && !q->numresults;
		}
		sqlasync_result_free(r);
		goto final;
	}

	while(q->numresults >= q->maxresults)
		pthread_cond_wait(&q->cond, lock);
	q->numresults++;

	if(q->sync) {
		queue_push(q, r, r);
		pthread_cond_signal(&q->cond);
		goto final;
	}

	r->queue = q;
	if(!q->each && !r->last) /* Buffer */
		queue_push(q, r, r);
	if(!q->each && r->last && q->first) { /* Unbuffer */
		queue_push(w, q->first, q->last);
		q->first = q->last = NULL;
	}
	if(q->each || r->last) {
		queue_push(w, r, r);
		shouldwakeup = 1;
	}

final:
	if(shouldwakeup) {
		shouldwakeup = !w->haswoken;
		w->haswoken = 1;
	}
	pthread_mutex_unlock(lock);

	if(shouldwakeup)
		w->wakeup(w, w->data);
	if(shouldfree)
		sqlasync_queue_free(q);
}


void sqlasync_dispatch(sqlasync_wakeup_t *w) {
	pthread_mutex_lock(&w->lock);
	while(w->first) {
		sqlasync_result_t *res = w->first;
		sqlasync_queue_t *q = res->queue;

		if(q->destroyed) {
			queue_pop(w);
			q->numresults--;
			if(res->last) {
				q->numscheduled--;
				w->numscheduled--;
			}
			sqlasync_result_free(res);
			if(!q->numscheduled && !q->numresults)
				sqlasync_queue_free(q);
		} else {
			/* The callback may call any sqlasync_* function other than
			 * sqlasync_wakeup_destroy(w). That includes freeing the queue object,
			 * emptying our queue and scheduling more events. So we have to be
			 * careful to not assume much about our state after calling this. */
			pthread_mutex_unlock(&w->lock);
			q->func(q, q->data);
			pthread_mutex_lock(&w->lock);
		}
	}
	w->haswoken = 0;
	int shouldschedule = !!w->numscheduled;
	pthread_mutex_unlock(&w->lock);

	if(shouldschedule && w->schedule)
		w->schedule(w, w->data);
}




sqlasync_result_t *sqlasync_result_create(unsigned short result, unsigned short last, unsigned int numcol) {
	sqlasync_result_t *r = malloc(offsetof(sqlasync_result_t, col) + (numcol* sizeof(sqlasync_value_t)));
	r->result = result;
	r->last = !!last;
	r->numcol = numcol;
	return r;
}


void sqlasync_result_free(sqlasync_result_t *r) {
	if(!r)
		return;
	while(r->numcol> 0)
		if(r->col[--r->numcol].freeptr)
			free(r->col[r->numcol].val.ptr);
	free(r);
}


static sqlasync_op_t *sqlasync_op_create(sqlasync_queue_t *q, const char *str, int flags, unsigned short numargs) {
	sqlasync_op_t *op = malloc(offsetof(sqlasync_op_t, args) + (numargs * sizeof(sqlasync_value_t)));
	op->next = NULL;
	op->q = q;
	if(!str || (flags & (SQLASYNC_STATIC|SQLASYNC_FREE)))
		op->str = (char *)str;
	else {
		op->str = malloc(strlen(str)+1);
		strcpy(op->str, str);
	}
	op->flags = flags;
	op->numargs = numargs;
	return op;
}


static void sqlasync_op_free(sqlasync_op_t *op) {
	if(!op)
		return;
	if(op->str && !(op->flags & SQLASYNC_STATIC))
		free(op->str);
	while(op->numargs > 0)
		if(op->args[--op->numargs].freeptr)
			free(op->args[op->numargs].val.ptr);
	free(op);
}




#define sqlasync_havetranstimeout(s) ((s)->transtimeout.tv_sec != 0 || (s)->transtimeout.tv_nsec != 0)

static inline struct timespec sqlasync_timespec_add(struct timespec a, struct timespec b) {
	a.tv_sec += b.tv_sec;
	a.tv_nsec += b.tv_nsec;
	if(a.tv_nsec > 1000000000) {
		a.tv_sec++;
		a.tv_nsec -= 1000000000;
	}
	return a;
}




/* This function will "consume" arguments from the op struct. i.e. by taking
 * ownership of string/blob buffers and resetting their `freeptr' value. As
 * such, the argument list of the operation should be considered invalid after
 * calling this function. */
static void sqlasync_thread_bind(sqlasync_op_t *op, sqlite3_stmt *st) {
	unsigned short i;
	for(i=1; i<=op->numargs; i++) {
		sqlasync_value_t *v = op->args+(i-1);
		switch(v->type) {
		case SQLITE_NULL:
			sqlite3_bind_null(st, i);
			break;
		case SQLITE_INTEGER:
			sqlite3_bind_int64(st, i, v->val.i64);
			break;
		case SQLITE_FLOAT:
			sqlite3_bind_double(st, i, v->val.dbl);
			break;
		case SQLITE3_TEXT:
			sqlite3_bind_text(st, i, v->val.ptr, -1, v->freeptr ? free : SQLITE_STATIC);
			v->freeptr = 0;
			break;
		case SQLITE_BLOB:
			/* COMPAT: sqlite3_bind_zeroblob() was added in SQLite 3.4.0 (2007-06-18) */
			if(v->val.ptr)
				sqlite3_bind_blob(st, i, v->val.ptr, v->length, v->freeptr ? free : SQLITE_STATIC);
			else
				sqlite3_bind_zeroblob(st, i, v->length);
			v->freeptr = 0;
			break;
		default:
			/* This assertion is going to be painful to debug... */
			assert("Invalid type for an sqlasync_value_t");
		}
	}
}


/* Creates a result object from the current statement handler and sends it to
 * the queue. */
static void sqlasync_thread_row(sqlasync_queue_t *q, sqlite3_stmt *st) {
	sqlasync_result_t *r = sqlasync_result_create(SQLITE_ROW, 0, sqlite3_column_count(st));
	unsigned int i;
	for(i=0; i<r->numcol; i++) {
		sqlasync_value_t *c = r->col+i;
		switch(sqlite3_column_type(st, i)) {
		case SQLITE_NULL:
			*c = sqlasync_null();
			break;
		case SQLITE_INTEGER:
			*c = sqlasync_int(sqlite3_column_int64(st, i));
			break;
		case SQLITE_FLOAT:
			*c = sqlasync_float(sqlite3_column_double(st, i));
			break;
		case SQLITE3_TEXT:
			*c = sqlasync_text(SQLASYNC_COPY, (char *)sqlite3_column_text(st, i));
			break;
		case SQLITE_BLOB:
			*c = sqlasync_blob(SQLASYNC_COPY, sqlite3_column_bytes(st, i), sqlite3_column_blob(st, i));
			break;
		default:
			assert("Invalid type returned by sqlite3_column_type()");
		}
	}
	sqlasync_queue_result(q, r);
}


/* It is assumed that we aren't in a transaction when this function is called.
 * It then shouldn't be able to fail, either. */
static void sqlasync_thread_begin(sqlasync_t *s) {
	if(!s->begin)
		assert(sqlite3_prepare_v2(s->db, "BEGIN", -1, &s->begin, NULL) == SQLITE_OK);
	sqlite3_step(s->begin);
	sqlite3_reset(s->begin);
	s->intrans = 1;
}


/* Failure is ignored. In either case the current transaction is aborted. */
static void sqlasync_thread_rollback(sqlasync_t *s) {
	if(!s->rollback)
		assert(sqlite3_prepare_v2(s->db, "ROLLBACK", -1, &s->rollback, NULL) == SQLITE_OK);
	sqlite3_step(s->rollback);
	sqlite3_reset(s->rollback);
	s->intrans = 0;
}


/* Tries to do a commit. If it failed (r != SQLITE_OK), then a rollback is done
 * instead. */
static int sqlasync_thread_commit(sqlasync_t *s) {
	int r;
	if(!s->commit)
		assert(sqlite3_prepare_v2(s->db, "COMMIT", -1, &s->commit, NULL) == SQLITE_OK);
	while((r = sqlite3_step(s->commit)) == SQLITE_BUSY)
		;
	sqlite3_reset(s->commit);
	if(r != SQLITE_DONE)
		sqlasync_thread_rollback(s);
	s->intrans = 0;
	return r;
}


/* Prepares, binds, and executes a query and sends back query results. Doesn't
 * send the `last' status result. Returns SQLITE_DONE on success. If st ==
 * NULL, then this was either a empty query, or one that failed validation.
 * Such queries have no effect on the state of the current transaction. */
static int sqlasync_thread_exec(sqlasync_t *s, sqlasync_op_t *op, sqlite3_stmt **st) {
	int r;

	/* COMPAT: sqlite3_prepare_v2() was added in SQLite 3.3.9 (2007-01-04) */
	if((r = sqlite3_prepare_v2(s->db, op->str, -1, st, NULL)) != SQLITE_OK) {
		if(*st)
			sqlite3_finalize(*st);
		return r;
	}

	/* We have an "empty" query, let's just behave as if it didn't return
	 * anything.
	 * TODO: Return more information, so that the application can differentiate
	 * between an empty query and a query that simply doesn't return anything?
	 * */
	if(!*st)
		return SQLITE_DONE;

	sqlasync_thread_bind(op, *st);

	r = SQLITE_ROW;
	while(r == SQLITE_ROW) {
		/* If we get an SQLITE_BUSY outside of a transaction, then we should
		 * just retry. If we're inside a transaction, then BUSY is an error. */
		if(s->intrans)
			r = sqlite3_step(*st);
		else
			while((r = sqlite3_step(*st)) == SQLITE_BUSY)
				;

		if(r == SQLITE_ROW)
			sqlasync_thread_row(op->q, *st);
	}
	return r;
}


/* Send back the `last' status. If the code isn't SQLITE_OK or SQLITE_DONE, an
 * error message will be included. */
static void sqlasync_thread_final(sqlasync_t *s, sqlasync_op_t *op, int r) {
	int okay = r == SQLITE_OK || r == SQLITE_DONE;
	sqlasync_result_t *res = sqlasync_result_create(r, 1, okay ? 0 : 1);
	if(!okay)
		res->col[0] = sqlasync_text(SQLASYNC_COPY, sqlite3_errmsg(s->db));
	sqlasync_queue_result(op->q, res);
}


static void sqlasync_thread_sql(sqlasync_t *s, sqlasync_op_t *op) {
	sqlite3_stmt *st = NULL;
	int r = SQLITE_ERROR;

	/* SINGLE queries can be executed here */
	if((op->flags & SQLASYNC_SINGLE) == SQLASYNC_SINGLE) {
		r = sqlasync_thread_exec(s, op, &st);
		goto final;
	}

	/* If we're in a NEXT-chain and the transaction has been aborted, report error. */
	if(s->errtrans) {
		if((op->flags & SQLASYNC_SINGLE) != SQLASYNC_NEXT)
			s->errtrans = 0;
		/* TODO: More specific error code? What error string will
		 * sqlasync_thread_final() give back, exactly? */
		r = SQLITE_ERROR;
		goto final;
	}

	/* If this is a LAST query, or the last query in a NEXT chain and we don't
	 * have a transaction timeout, then the result of the commit operation
	 * should be sent back as the result of the query. */
	if((op->flags & SQLASYNC_SINGLE) == SQLASYNC_LAST ||
			(!sqlasync_havetranstimeout(s) && s->donext && (op->flags & SQLASYNC_SINGLE) != SQLASYNC_NEXT)) {
		r = sqlasync_thread_exec(s, op, &st);
		/* Rollback even if st == NULL (i.e. query parsing failed). Even if we
		 * could still validly try a commit, we have to return an error anyway.
		 * We have currently no way of saying "Hey, your query failed, but
		 * comitting your previous stuff went fine".
		 * (It's an obscure situation in any case). */
		if(s->intrans && r != SQLITE_DONE)
			sqlasync_thread_rollback(s);
		if(s->intrans && r == SQLITE_DONE)
			r = sqlasync_thread_commit(s);
		goto final;
	}

	/* If we need to start a new transaction, let's do so */
	if(!s->intrans && ((op->flags & SQLASYNC_SINGLE) == SQLASYNC_NEXT || sqlasync_havetranstimeout(s))) {
		sqlasync_thread_begin(s);
		if(sqlasync_havetranstimeout(s)) {
			clock_gettime(CLOCK_MONOTONIC, &s->trans);
			s->trans = sqlasync_timespec_add(s->trans, s->transtimeout);
		}
	}

	/* Normal/NEXT query */
	r = sqlasync_thread_exec(s, op, &st);

	if(st && r != SQLITE_DONE) {
		if(s->intrans)
			sqlasync_thread_rollback(s);
		if((op->flags & SQLASYNC_SINGLE) == SQLASYNC_NEXT)
			s->errtrans = 1;
	}

final:
	sqlasync_thread_final(s, op, r);
	if(st) {
		sqlite3_reset(st);
		sqlite3_finalize(st);
	}
}


static void sqlasync_thread_open(sqlasync_t *s, sqlasync_op_t *op) {
	assert("Database already open" && !s->db);

	/* COMPAT: sqlite3_open_v2() was added in SQLite 3.5.0 (2007-09-04) */
	int r = op->args[0].val.i64
		? sqlite3_open_v2(op->str, &s->db, op->args[0].val.i64, NULL)
		: sqlite3_open(op->str, &s->db);

	sqlasync_result_t *res;
	if(r) {
		res = sqlasync_result_create(r, 1, 1);
		res->col[0] = sqlasync_text(SQLASYNC_COPY, sqlite3_errmsg(s->db));
		sqlite3_close(s->db);
		s->db = NULL;
	} else {
		res = sqlasync_result_create(r, 1, 0);
		s->dbqueue = op->args[1].val.ptr;
		/* TODO: Make the busy handling configurable? */
		sqlite3_busy_timeout(s->db, 10);
	}
	sqlasync_queue_result(op->q, res);

	/* XXX: Don't merge into the above if/else. This result has to be sent
	 * after the normal result, in order to handle the case where the same
	 * queue is used for both purposes. */
	if(r)
		sqlasync_queue_result(op->args[1].val.ptr, sqlasync_result_create(SQLITE_OK, 1, 0));
}


static void sqlasync_thread_close(sqlasync_t *s) {
	/* We may be called even when there isn't a database open, so ensure that
	 * all called functions properly handle NULL arguments. */
	sqlite3_finalize(s->begin);
	sqlite3_finalize(s->commit);
	sqlite3_finalize(s->rollback);
	sqlite3_close(s->db); /* Can't really fail */
	sqlasync_queue_result(s->dbqueue, sqlasync_result_create(SQLITE_OK, 1, 0));
	s->db = NULL;
	s->dbqueue = NULL;
	s->begin = s->commit = s->rollback = NULL;
}


/* Only returns NULL if the current transaction has timed out */
static sqlasync_op_t *sqlasync_thread_getnext(sqlasync_t *s) {
	sqlasync_op_t *op = NULL;

	/* If donext, then we shouldn't wait.
	 * If intrans, then we should do a timedwait,
	 * Otherwise, regular wait.
	 */
	pthread_mutex_lock(&s->lock);
	while(!s->donext && !s->first) {
		if(!s->intrans)
			pthread_cond_wait(&s->cond, &s->lock);
		else if(pthread_cond_timedwait(&s->cond, &s->lock, &s->trans) == ETIMEDOUT)
			break;
	}
	if(s->first) {
		op = s->first;
		queue_pop(s);
	}
	pthread_mutex_unlock(&s->lock);

	assert("An SQLASYNC_NEXT was queued, but there is no next query" && (op || !s->donext));
	return op;
}


static void *sqlasync_thread(void *dat) {
	sqlasync_t *s = dat;
	sqlasync_op_t *op = NULL;

	while(1) {
		sqlasync_op_free(op);
		op = sqlasync_thread_getnext(s);
		int flags = op ? op->flags : 0;

		/* If we need to commit, do so. */
		if(!op || (s->intrans &&
				(flags == SQLASYNC_OPEN || flags == SQLASYNC_CLOSE ||
				 flags == SQLASYNC_QUIT || flags == SQLASYNC_CUSTOM ||
				 (flags & SQLASYNC_SINGLE) == SQLASYNC_SINGLE))) {
			assert("Can't close a transaction when we still have a SQLASYNC_NEXT to process" && !s->donext);
			int r = sqlasync_thread_commit(s);
			if(r != SQLITE_DONE) {
				sqlasync_result_t *res = sqlasync_result_create(r, 0, 1);
				res->col[0] = sqlasync_text(SQLASYNC_COPY, sqlite3_errmsg(s->db));
				sqlasync_queue_result(s->dbqueue, res);
			}
		}

		if(!op)
			continue;

		/* Special operations */
		if(flags == SQLASYNC_OPEN) {
			sqlasync_thread_open(s, op);
			continue;
		} else if(flags == SQLASYNC_CLOSE) {
			sqlasync_thread_close(s);
			continue;
		} else if(flags == SQLASYNC_QUIT)
			break;
		else if(flags == SQLASYNC_CUSTOM) {
			((sqlasync_custom_func_t)op->args[0].val.ptr)(s, s->db, op->q, op->numargs-1, op->args+1);
			continue;
		}

		sqlasync_thread_sql(s, op);
		s->donext = (flags & SQLASYNC_SINGLE) == SQLASYNC_NEXT;
	}
	sqlasync_op_free(op);

	sqlasync_thread_close(s);
	return NULL;
}




sqlasync_queue_t *sqlasync_open(sqlasync_t *s, sqlasync_queue_t *q, sqlasync_queue_t *errq, const char *filename, int flags) {
	sqlasync_op_t *op = sqlasync_op_create(q, filename, SQLASYNC_OPEN, 2);
	op->args[0] = sqlasync_int(flags);
	op->args[1].freeptr = 0;
	op->args[1].val.ptr = errq; /* Abuse the sqlasync_value_t to pass a queue pointer */
	sqlasync_queue_schedule(q);
	sqlasync_queue_schedule(errq);

	pthread_mutex_lock(&s->lock);
	queue_push(s, op, op);
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->lock);

	return q;
}


void sqlasync_close(sqlasync_t *s) {
	sqlasync_op_t *op = sqlasync_op_create(NULL, NULL, SQLASYNC_CLOSE, 0);

	pthread_mutex_lock(&s->lock);
	queue_push(s, op, op);
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->lock);
}


sqlasync_t *sqlasync_create(const struct timespec *transtimeout) {
	sqlasync_t *s = calloc(1, sizeof(sqlasync_t));
	if(transtimeout)
		s->transtimeout = *transtimeout;
	pthread_mutex_init(&s->lock, NULL);

	/* COMPAT: We unconditionally use CLOCK_MONOTONIC in order to avoid
	 * problems when the system time jumps. However,
	 * pthread_condattr_setclock() is relatively new, and not all systems may
	 * have a CLOCK_MONOTONIC. Should there be a compile-time option to use the
	 * default, and likely more portable, CLOCK_REALTIME? */
	pthread_condattr_t cattr;
	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	pthread_cond_init(&s->cond, &cattr);
	pthread_condattr_destroy(&cattr);

	if(pthread_create(&s->thread, NULL, sqlasync_thread, s)) {
		free(s);
		return NULL;
	}

	return s;
}


void sqlasync_lock(sqlasync_t *s)   { pthread_mutex_lock(&s->lock);   }
void sqlasync_unlock(sqlasync_t *s) { pthread_mutex_unlock(&s->lock); }


sqlasync_queue_t *sqlasync_sqlv_unlocked(sqlasync_t *s, sqlasync_queue_t *q,
		int flags, const char *query, int bind_num, va_list binds) {
	sqlasync_op_t *op = sqlasync_op_create(q, query, flags, bind_num);

	int i = 0;
	while(i<bind_num)
		op->args[i++] = va_arg(binds, sqlasync_value_t);

	sqlasync_queue_schedule(q);
	queue_push(s, op, op);
	pthread_cond_signal(&s->cond);

	return q;
}


sqlasync_queue_t *sqlasync_sql_unlocked(sqlasync_t *s, sqlasync_queue_t *q,
		int flags, const char *query, int bind_num, ...) {
	va_list l;
	va_start(l, bind_num);
	sqlasync_queue_t *rq = sqlasync_sqlv_unlocked(s, q, flags, query, bind_num, l);
	va_end(l);
	return rq;
}


sqlasync_queue_t *sqlasync_sql(sqlasync_t *s, sqlasync_queue_t *q,
		int flags, const char *query, int bind_num, ...) {
	va_list l;
	va_start(l, bind_num);
	sqlasync_lock(s);
	sqlasync_queue_t *rq = sqlasync_sqlv_unlocked(s, q, flags, query, bind_num, l);
	sqlasync_unlock(s);
	va_end(l);
	return rq;
}


sqlasync_queue_t *sqlasync_custom(sqlasync_t *s, sqlasync_queue_t *q, sqlasync_custom_func_t f, int val_num, ...) {
	va_list l;
	sqlasync_op_t *op = sqlasync_op_create(q, NULL, SQLASYNC_CUSTOM, val_num+1);
	op->args[0].freeptr = 0;
	op->args[0].val.ptr = f;

	int i = 0;
	va_start(l, val_num);
	while(i<val_num)
		op->args[++i] = va_arg(l, sqlasync_value_t);

	sqlasync_lock(s);
	sqlasync_queue_schedule(q);
	queue_push(s, op, op);
	pthread_cond_signal(&s->cond);
	sqlasync_unlock(s);

	va_end(l);
	return q;
}


void sqlasync_destroy(sqlasync_t *s) {
	sqlasync_op_t *op = sqlasync_op_create(NULL, NULL, SQLASYNC_QUIT, 0);

	pthread_mutex_lock(&s->lock);
	queue_push(s, op, op);
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->lock);

	pthread_join(s->thread, NULL);
	pthread_mutex_destroy(&s->lock);
	pthread_cond_destroy(&s->cond);
	free(s);
}

/* vim: set noet sw=4 ts=4: */
