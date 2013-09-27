/* Copyright (c) 2012 Yoran Heling

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

#if defined(EV_CONFIG_H)
#include EV_CONFIG_H
#elif defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include "dbusev.h"
#include <stdlib.h>


/* dbus <-> libev IO watchers */

typedef struct {
	ev_io io;
	DBusWatch *w;
} de_io_t;


static void io_handle(EV_P_ ev_io *io, int revents) {
	dbus_watch_handle(((de_io_t *)io)->w, (revents & EV_READ ? DBUS_WATCH_READABLE : 0) | (revents & EV_WRITE ? DBUS_WATCH_WRITABLE : 0));
}


static void io_toggle(DBusWatch *w, void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = data;
#endif
	ev_io *io = dbus_watch_get_data(w);
	if(dbus_watch_get_enabled(w)) {
		int f = dbus_watch_get_flags(w);
		ev_io_set(io, dbus_watch_get_unix_fd(w), (f & DBUS_WATCH_READABLE ? EV_READ : 0) | (f & DBUS_WATCH_WRITABLE ? EV_WRITE : 0));
		ev_io_start(EV_A_ io);
	} else
		ev_io_stop(EV_A_ io);
}


static dbus_bool_t io_add(DBusWatch *w, void *data) {
	ev_io *io = malloc(sizeof(de_io_t));
	if(!io)
		return FALSE;
	((de_io_t *)io)->w = w;
	dbus_watch_set_data(w, io, NULL);
	ev_init(io, io_handle);
	io_toggle(w, data);
	return TRUE;
}


static void io_remove(DBusWatch *w, void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = data;
#endif
	ev_io *io = dbus_watch_get_data(w);
	ev_io_stop(EV_A_ io);
	free(io);
}



/* dbus <-> libev timeout functions */

typedef struct {
  ev_timer timer;
  DBusTimeout *t;
} de_timer_t;


static void timer_handle(EV_P_ ev_timer *timer, int revents) {
	dbus_timeout_handle(((de_timer_t *)timer)->t);
}


static void timer_toggle(DBusTimeout *t, void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = data;
#endif
	ev_timer *timer = dbus_timeout_get_data(t);
	if(dbus_timeout_get_enabled(t)) {
		timer->repeat = ((ev_tstamp)dbus_timeout_get_interval(t))/1000.0;
		ev_timer_again(EV_A_ timer);
	} else
		ev_timer_stop(EV_A_ timer);
}


static dbus_bool_t timer_add(DBusTimeout *t, void *data) {
	ev_timer *timer = malloc(sizeof(de_timer_t));
	if(!timer)
		return FALSE;
	((de_timer_t *)timer)->t = t;
	dbus_timeout_set_data(t, timer, NULL);
	ev_init(timer, timer_handle);
	timer_toggle(t, data);
	return TRUE;
}


static void timer_remove(DBusTimeout *t, void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = data;
#endif
	ev_timer *timer = dbus_timeout_get_data(t);
	ev_timer_stop(EV_A_ timer);
	free(timer);
}



/* dbus <-> libev dispatch handling */

typedef struct {
	ev_idle idl;
	DBusConnection *c;
#if EV_MULTIPLICITY
	struct ev_loop *loop;
#endif
} dispatch_t;


static void dispatch(EV_P_ ev_idle *idl, int revents) {
	if(dbus_connection_dispatch(((dispatch_t *)idl)->c) == DBUS_DISPATCH_COMPLETE)
		ev_idle_stop(EV_A_ idl);
}


static void dispatch_change(DBusConnection *con, DBusDispatchStatus s, void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = ((dispatch_t *)data)->loop;
#endif
	ev_idle_start(EV_A_ (ev_idle *)data);
}



/* dbus <-> libev async main loop wakeup */

typedef struct {
	ev_async async;
#if EV_MULTIPLICITY
	struct ev_loop *loop;
#endif
} async_t;


static void async_handle(EV_P_ ev_async *async, int revents) {
	/* Nothing to do here, if this function is called then the ev_loop has already woken up. */
}


static void async_wakeup(void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = ((async_t *)data)->loop;
#endif
	ev_async_send(EV_A_ (ev_async *)data);
}


static void async_free(void *data) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = ((async_t *)data)->loop;
#endif
	ev_async_stop(EV_A_ data);
	free(data);
}




void dbusev_register(EV_P_ DBusConnection *con) {
#if !EV_MULTIPLICITY
#define loop NULL
#endif

	dbus_connection_set_watch_functions(con, io_add, io_remove, io_toggle, loop, NULL);
	dbus_connection_set_timeout_functions(con, timer_add, timer_remove, timer_toggle, loop, NULL);

	dispatch_t *dispatcher = malloc(sizeof(dispatch_t));
	ev_idle_init((ev_idle *)dispatcher, dispatch);
	dispatcher->c = con;
#if EV_MULTIPLICITY
	dispatcher->loop = loop;
#endif
	dbus_connection_set_dispatch_status_function(con, dispatch_change, dispatcher, free);

	async_t *async = malloc(sizeof(async_t));
	ev_async_init((ev_async *)async, async_handle);
	ev_async_start(EV_A_ (ev_async *)async);
#if EV_MULTIPLICITY
	async->loop = loop;
#endif
	dbus_connection_set_wakeup_main_function(con, async_wakeup, async, async_free);
}

/* vim: set noet sw=4 ts=4: */
