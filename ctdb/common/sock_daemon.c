/*
   A server based on unix domain socket

   Copyright (C) Amitay Isaacs  2016

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/filesys.h"
#include "system/network.h"
#include "system/wait.h"

#include <talloc.h>
#include <tevent.h>

#include "lib/async_req/async_sock.h"
#include "lib/util/debug.h"
#include "lib/util/blocking.h"
#include "lib/util/dlinklist.h"
#include "lib/util/tevent_unix.h"

#include "common/logging.h"
#include "common/reqid.h"
#include "common/comm.h"
#include "common/pidfile.h"
#include "common/sock_daemon.h"

struct sock_socket {
	struct sock_socket *prev, *next;

	const char *sockpath;
	struct sock_socket_funcs *funcs;
	void *private_data;

	int fd;
	struct tevent_req *req;
};

struct sock_client {
	struct sock_client *prev, *next;

	struct tevent_req *req;
	struct sock_client_context *client_ctx;
};

struct sock_client_context {
	struct tevent_context *ev;
	struct sock_socket *sock;
	int fd;
	struct comm_context *comm;

	struct sock_client *client;
};

struct sock_daemon_context {
	struct sock_daemon_funcs *funcs;
	void *private_data;

	struct pidfile_context *pid_ctx;
	struct sock_socket *socket_list;
	struct tevent_req *req;
};

/*
 * Process a single client
 */

static void sock_client_read_handler(uint8_t *buf, size_t buflen,
				     void *private_data);
static void sock_client_read_done(struct tevent_req *subreq);
static void sock_client_dead_handler(void *private_data);
static int sock_client_context_destructor(
				struct sock_client_context *client_ctx);

static int sock_client_context_init(TALLOC_CTX *mem_ctx,
				    struct tevent_context *ev,
				    struct sock_socket *sock,
				    int client_fd,
				    struct sock_client *client,
				    struct sock_client_context **result)
{
	struct sock_client_context *client_ctx;
	int ret;

	client_ctx = talloc_zero(mem_ctx, struct sock_client_context);
	if (client_ctx == NULL) {
		return ENOMEM;
	}

	client_ctx->ev = ev;
	client_ctx->sock = sock;
	client_ctx->fd = client_fd;
	client_ctx->client = client;

	ret = comm_setup(client_ctx, ev, client_fd,
			 sock_client_read_handler, client_ctx,
			 sock_client_dead_handler, client_ctx,
			 &client_ctx->comm);
	if (ret != 0) {
		talloc_free(client_ctx);
		return ret;
	}

	if (sock->funcs->connect != NULL) {
		bool status;

		status = sock->funcs->connect(client_ctx, sock->private_data);
		if (! status) {
			talloc_free(client_ctx);
			close(client_fd);
			return 0;
		}
	}

	talloc_set_destructor(client_ctx, sock_client_context_destructor);

	*result = client_ctx;
	return 0;
}

static void sock_client_read_handler(uint8_t *buf, size_t buflen,
				     void *private_data)
{
	struct sock_client_context *client_ctx = talloc_get_type_abort(
		private_data, struct sock_client_context);
	struct sock_socket *sock = client_ctx->sock;
	struct tevent_req *subreq;

	subreq = sock->funcs->read_send(client_ctx, client_ctx->ev,
					client_ctx, buf, buflen,
					sock->private_data);
	if (subreq == NULL) {
		talloc_free(client_ctx);
		return;
	}
	tevent_req_set_callback(subreq, sock_client_read_done, client_ctx);
}

static void sock_client_read_done(struct tevent_req *subreq)
{
	struct sock_client_context *client_ctx = tevent_req_callback_data(
		subreq, struct sock_client_context);
	struct sock_socket *sock = client_ctx->sock;
	int ret;
	bool status;

	status = sock->funcs->read_recv(subreq, &ret);
	if (! status) {
		D_ERR("client read failed with ret=%d\n", ret);
		talloc_free(client_ctx);
	}
}

static void sock_client_dead_handler(void *private_data)
{
	struct sock_client_context *client_ctx = talloc_get_type_abort(
		private_data, struct sock_client_context);
	struct sock_socket *sock = client_ctx->sock;

	if (sock->funcs->disconnect != NULL) {
		sock->funcs->disconnect(client_ctx, sock->private_data);
	}

	talloc_free(client_ctx);
}

static int sock_client_context_destructor(
				struct sock_client_context *client_ctx)
{
	TALLOC_FREE(client_ctx->client);
	TALLOC_FREE(client_ctx->comm);
	if (client_ctx->fd != -1) {
		close(client_ctx->fd);
		client_ctx->fd = -1;
	}

	return 0;
}

/*
 * Process a single listening socket
 */

static int socket_setup(const char *sockpath, bool remove_before_use)
{
	struct sockaddr_un addr;
	size_t len;
	int ret, fd;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	len = strlcpy(addr.sun_path, sockpath, sizeof(addr.sun_path));
	if (len >= sizeof(addr.sun_path)) {
		D_ERR("socket path too long: %s\n", sockpath);
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		D_ERR("socket create failed - %s\n", sockpath);
		return -1;
	}

	ret = set_blocking(fd, false);
	if (ret != 0) {
		D_ERR("socket set nonblocking failed - %s\n", sockpath);
		close(fd);
		return -1;
	}

	if (remove_before_use) {
		unlink(sockpath);
	}

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret != 0) {
		D_ERR("socket bind failed - %s\n", sockpath);
		close(fd);
		return -1;
	}

	ret = listen(fd, 10);
	if (ret != 0) {
		D_ERR("socket listen failed - %s\n", sockpath);
		close(fd);
		return -1;
	}

	return fd;
}

static int sock_socket_destructor(struct sock_socket *sock);

static int sock_socket_init(TALLOC_CTX *mem_ctx, const char *sockpath,
			    struct sock_socket_funcs *funcs,
			    void *private_data,
			    bool remove_before_use,
			    struct sock_socket **result)
{
	struct sock_socket *sock;

	if (funcs == NULL) {
		return EINVAL;
	}
	if (funcs->read_send == NULL || funcs->read_recv == NULL) {
		return EINVAL;
	}

	sock = talloc_zero(mem_ctx, struct sock_socket);
	if (sock == NULL) {
		return ENOMEM;
	}

	sock->sockpath = sockpath;
	sock->funcs = funcs;
	sock->private_data = private_data;

	sock->fd = socket_setup(sockpath, remove_before_use);
	if (sock->fd == -1) {
		talloc_free(sock);
		return EIO;
	}

	talloc_set_destructor(sock, sock_socket_destructor);

	*result = sock;
	return 0;
}

static int sock_socket_destructor(struct sock_socket *sock)
{
	if (sock->fd != -1) {
		close(sock->fd);
		sock->fd = -1;
	}

	unlink(sock->sockpath);
	return 0;
}


struct sock_socket_start_state {
	struct tevent_context *ev;
	struct sock_socket *sock;

	struct sock_client *client_list;
};

static int sock_socket_start_state_destructor(
				struct sock_socket_start_state *state);
static void sock_socket_start_new_client(struct tevent_req *subreq);
static int sock_socket_start_client_destructor(struct sock_client *client);

static struct tevent_req *sock_socket_start_send(TALLOC_CTX *mem_ctx,
						 struct tevent_context *ev,
						 struct sock_socket *sock)
{
	struct tevent_req *req, *subreq;
	struct sock_socket_start_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct sock_socket_start_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->sock = sock;

	talloc_set_destructor(state, sock_socket_start_state_destructor);

	subreq = accept_send(state, ev, sock->fd);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, sock_socket_start_new_client, req);

	return req;
}

static int sock_socket_start_state_destructor(
				struct sock_socket_start_state *state)
{
	struct sock_client *client;

	while ((client = state->client_list) != NULL) {
		talloc_free(client);
	}

	return 0;
}

static void sock_socket_start_new_client(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_socket_start_state *state = tevent_req_data(
		req, struct sock_socket_start_state);
	struct sock_client *client;
	int client_fd, ret;

	client_fd = accept_recv(subreq, NULL, NULL, &ret);
	TALLOC_FREE(subreq);
	if (client_fd == -1) {
		D_ERR("failed to accept new connection\n");
	}

	subreq = accept_send(state, state->ev, state->sock->fd);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, sock_socket_start_new_client, req);

	if (client_fd == -1) {
		return;
	}

	client = talloc_zero(state, struct sock_client);
	if (tevent_req_nomem(client, req)) {
		close(client_fd);
		return;
	}

	client->req = req;

	ret = sock_client_context_init(client, state->ev, state->sock,
				       client_fd, client, &client->client_ctx);
	if (ret != 0) {
		talloc_free(client);
		return;
	}

	talloc_set_destructor(client, sock_socket_start_client_destructor);
	DLIST_ADD(state->client_list, client);
}

static int sock_socket_start_client_destructor(struct sock_client *client)
{
	struct sock_socket_start_state *state = tevent_req_data(
		client->req, struct sock_socket_start_state);

	DLIST_REMOVE(state->client_list, client);
	TALLOC_FREE(client->client_ctx);

	return 0;
}

static bool sock_socket_start_recv(struct tevent_req *req, int *perr)
{
	struct sock_socket_start_state *state = tevent_req_data(
		req, struct sock_socket_start_state);
	int ret;

	state->sock->req = NULL;

	if (tevent_req_is_unix_error(req, &ret)) {
		if (perr != NULL) {
			*perr = ret;
		}
		return false;
	}

	return true;
}

/*
 * Send message to a client
 */

struct sock_socket_write_state {
	int status;
};

static void sock_socket_write_done(struct tevent_req *subreq);

struct tevent_req *sock_socket_write_send(TALLOC_CTX *mem_ctx,
					 struct tevent_context *ev,
					 struct sock_client_context *client_ctx,
					 uint8_t *buf, size_t buflen)
{
	struct tevent_req *req, *subreq;
	struct sock_socket_write_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct sock_socket_write_state);
	if (req == NULL) {
		return NULL;
	}

	subreq = comm_write_send(state, ev, client_ctx->comm, buf, buflen);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, sock_socket_write_done, req);

	return req;
}

static void sock_socket_write_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_socket_write_state *state = tevent_req_data(
		req, struct sock_socket_write_state);
	int ret;
	bool status;

	status = comm_write_recv(subreq, &ret);
	TALLOC_FREE(subreq);
	if (! status) {
		state->status = ret;
		return;
	}
}

bool sock_socket_write_recv(struct tevent_req *req, int *perr)
{
	struct sock_socket_write_state *state = tevent_req_data(
		req, struct sock_socket_write_state);
	int ret;

	if (tevent_req_is_unix_error(req, &ret)) {
		if (perr != NULL) {
			*perr = ret;
		}
		return false;
	}

	if (state->status != 0) {
		if (perr != NULL) {
			*perr = state->status;
		}
		return false;
	}

	if (perr != NULL) {
		*perr = 0;
	}
	return true;
}
/*
 * Socket daemon
 */

static int sock_daemon_context_destructor(struct sock_daemon_context *sockd);

int sock_daemon_setup(TALLOC_CTX *mem_ctx, const char *daemon_name,
		      const char *logging, const char *debug_level,
		      const char *pidfile,
		      struct sock_daemon_funcs *funcs,
		      void *private_data,
		      struct sock_daemon_context **out)
{
	struct sock_daemon_context *sockd;
	int ret;

	sockd = talloc_zero(mem_ctx, struct sock_daemon_context);
	if (sockd == NULL) {
		return ENOMEM;
	}

	sockd->funcs = funcs;
	sockd->private_data = private_data;

	ret = logging_init(sockd, logging, debug_level, daemon_name);
	if (ret != 0) {
		fprintf(stderr,
			"Failed to initialize logging, logging=%s, debug=%s\n",
			logging, debug_level);
		return ret;
	}

	if (pidfile != NULL) {
		ret = pidfile_create(sockd, pidfile, &sockd->pid_ctx);
		if (ret != 0) {
			talloc_free(sockd);
			return EEXIST;
		}
	}

	talloc_set_destructor(sockd, sock_daemon_context_destructor);

	*out = sockd;
	return 0;
}

static int sock_daemon_context_destructor(struct sock_daemon_context *sockd)
{
	if (sockd->req != NULL) {
		tevent_req_done(sockd->req);
	}

	return 0;
}

int sock_daemon_add_unix(struct sock_daemon_context *sockd,
			 const char *sockpath,
			 struct sock_socket_funcs *funcs,
			 void *private_data)
{
	struct sock_socket *sock;
	int ret;
	bool remove_before_use = false;

	remove_before_use = (sockd->pid_ctx != NULL) ? true : false;

	ret = sock_socket_init(sockd, sockpath, funcs, private_data,
			       remove_before_use, &sock);
	if (ret != 0) {
		return ret;
	}

	D_NOTICE("listening on %s\n", sockpath);

	DLIST_ADD(sockd->socket_list, sock);
	return 0;
}

/*
 * Run socket daemon
 */

struct sock_daemon_start_state {
	struct tevent_context *ev;
	struct sock_daemon_context *sockd;
	pid_t pid_watch;

	int fd;
};

static void sock_daemon_started(struct tevent_req *subreq);
static void sock_daemon_signal_handler(struct tevent_context *ev,
				       struct tevent_signal *se,
				       int signum, int count, void *siginfo,
				       void *private_data);
static void sock_daemon_socket_fail(struct tevent_req *subreq);
static void sock_daemon_watch_pid(struct tevent_req *subreq);
static void sock_daemon_reconfigure(struct sock_daemon_start_state *state);
static void sock_daemon_shutdown(struct sock_daemon_start_state *state);

struct tevent_req *sock_daemon_run_send(TALLOC_CTX *mem_ctx,
					struct tevent_context *ev,
					struct sock_daemon_context *sockd,
					pid_t pid_watch)
{
	struct tevent_req *req, *subreq;
	struct sock_daemon_start_state *state;
	struct tevent_signal *se;
	struct sock_socket *sock;

	req = tevent_req_create(mem_ctx, &state,
				struct sock_daemon_start_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->sockd = sockd;
	state->pid_watch = pid_watch;
	state->fd  = -1;

	subreq = tevent_wakeup_send(state, ev,
				    tevent_timeval_current_ofs(0, 0));
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, sock_daemon_started, req);

	se = tevent_add_signal(ev, state, SIGHUP, 0,
			       sock_daemon_signal_handler, req);
	if (tevent_req_nomem(se, req)) {
		return tevent_req_post(req, ev);
	}

	se = tevent_add_signal(ev, state, SIGUSR1, 0,
			       sock_daemon_signal_handler, req);
	if (tevent_req_nomem(se, req)) {
		return tevent_req_post(req, ev);
	}

	se = tevent_add_signal(ev, state, SIGINT, 0,
			       sock_daemon_signal_handler, req);
	if (tevent_req_nomem(se, req)) {
		return tevent_req_post(req, ev);
	}

	se = tevent_add_signal(ev, state, SIGTERM, 0,
			       sock_daemon_signal_handler, req);
	if (tevent_req_nomem(se, req)) {
		return tevent_req_post(req, ev);
	}

	for (sock = sockd->socket_list; sock != NULL; sock = sock->next) {
		subreq = sock_socket_start_send(state, ev, sock);
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq, sock_daemon_socket_fail, req);

		sock->req = subreq;
	}

	if (pid_watch > 1) {
		subreq = tevent_wakeup_send(state, ev,
					    tevent_timeval_current_ofs(1,0));
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq, sock_daemon_watch_pid, req);
	}

	sockd->req = req;

	return req;
}

static void sock_daemon_started(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_daemon_start_state *state = tevent_req_data(
		req, struct sock_daemon_start_state);
	struct sock_daemon_context *sockd = state->sockd;

	D_NOTICE("daemon started, pid=%u\n", getpid());

	if (sockd->funcs != NULL && sockd->funcs->startup != NULL) {
		sockd->funcs->startup(sockd->private_data);
	}
}

static void sock_daemon_signal_handler(struct tevent_context *ev,
				       struct tevent_signal *se,
				       int signum, int count, void *siginfo,
				       void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);
	struct sock_daemon_start_state *state = tevent_req_data(
		req, struct sock_daemon_start_state);

	D_NOTICE("Received signal %d\n", signum);

	if (signum == SIGHUP || signum == SIGUSR1) {
		sock_daemon_reconfigure(state);
		return;
	}

	if (signum == SIGINT || signum == SIGTERM) {
		sock_daemon_shutdown(state);
		tevent_req_error(req, EINTR);
	}
}

static void sock_daemon_reconfigure(struct sock_daemon_start_state *state)
{
	struct sock_daemon_context *sockd = state->sockd;

	if (sockd->funcs != NULL && sockd->funcs->reconfigure != NULL) {
		sockd->funcs->reconfigure(sockd->private_data);
	}
}

static void sock_daemon_shutdown(struct sock_daemon_start_state *state)
{
	struct sock_daemon_context *sockd = state->sockd;
	struct sock_socket *sock;

	D_NOTICE("Shutting down\n");

	while ((sock = sockd->socket_list) != NULL) {
		DLIST_REMOVE(sockd->socket_list, sock);
		TALLOC_FREE(sock->req);
		TALLOC_FREE(sock);
	}

	if (sockd->funcs != NULL && sockd->funcs->shutdown != NULL) {
		sockd->funcs->shutdown(sockd->private_data);
	}

	TALLOC_FREE(sockd->pid_ctx);
}

static void sock_daemon_socket_fail(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_daemon_start_state *state = tevent_req_data(
		req, struct sock_daemon_start_state);
	int ret = 0;
	bool status;

	status = sock_socket_start_recv(subreq, &ret);
	TALLOC_FREE(subreq);
	if (! status) {
		tevent_req_error(req, ret);
	} else {
		tevent_req_done(req);
	}

	sock_daemon_shutdown(state);
}

static void sock_daemon_watch_pid(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_daemon_start_state *state = tevent_req_data(
		req, struct sock_daemon_start_state);
	int ret;
	bool status;

	status = tevent_wakeup_recv(subreq);
	TALLOC_FREE(subreq);
	if (! status) {
		tevent_req_error(req, EIO);
		return;
	}

	ret = kill(state->pid_watch, 0);
	if (ret == -1) {
		if (errno == ESRCH) {
			D_ERR("PID %d gone away, exiting\n", state->pid_watch);
			sock_daemon_shutdown(state);
			tevent_req_error(req, ESRCH);
			return;
		} else {
			D_ERR("Failed to check PID status %d, ret=%d\n",
			      state->pid_watch, errno);
		}
	}

	subreq = tevent_wakeup_send(state, state->ev,
				    tevent_timeval_current_ofs(5,0));
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, sock_daemon_watch_pid, req);
}

bool sock_daemon_run_recv(struct tevent_req *req, int *perr)
{
	int ret;

	if (tevent_req_is_unix_error(req, &ret)) {
		if (perr != NULL) {
			*perr = ret;
		}
		return false;
	}

	return true;
}

int sock_daemon_run(struct tevent_context *ev,
		    struct sock_daemon_context *sockd,
		    pid_t pid_watch)
{
	struct tevent_req *req;
	int ret;
	bool status;

	req = sock_daemon_run_send(ev, ev, sockd, pid_watch);
	if (req == NULL) {
		return ENOMEM;
	}

	tevent_req_poll(req, ev);

	status = sock_daemon_run_recv(req, &ret);
	sockd->req = NULL;
	TALLOC_FREE(req);
	if (! status) {
		return ret;
	}

	return 0;
}