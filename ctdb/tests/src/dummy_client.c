/*
   Dummy CTDB client for testing

   Copyright (C) Amitay Isaacs  2017

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
#include "system/network.h"

#include <popt.h>
#include <talloc.h>
#include <tevent.h>

#include "common/logging.h"

#include "client/client.h"

static struct {
	const char *sockpath;
	const char *debuglevel;
	int timelimit;
	const char *srvidstr;
} options;

static struct poptOption cmdline_options[] = {
	POPT_AUTOHELP
	{ "socket", 's', POPT_ARG_STRING, &options.sockpath, 0,
		"Unix domain socket path", "filename" },
	{ "debug", 'd', POPT_ARG_STRING, &options.debuglevel, 0,
		"debug level", "ERR|WARNING|NOTICE|INFO|DEBUG" } ,
	{ "timelimit", 't', POPT_ARG_INT, &options.timelimit, 0,
		"time limit", "seconds" },
	{ "srvid", 'S', POPT_ARG_STRING, &options.srvidstr, 0,
		"srvid to register", "srvid" },
	POPT_TABLEEND
};

static void dummy_handler(uint64_t srvid, TDB_DATA data, void *private_data)
{
	bool *done = (bool *)private_data;

	*done = true;
}

int main(int argc, const char *argv[])
{
	TALLOC_CTX *mem_ctx;
	struct tevent_context *ev;
	struct ctdb_client_context *client;
	const char *ctdb_socket;
	poptContext pc;
	int opt, ret;
	int log_level;
	bool status, done;

	/* Set default options */
	options.sockpath = CTDB_SOCKET;
	options.debuglevel = "ERR";
	options.timelimit = 60;
	options.srvidstr = NULL;

	ctdb_socket = getenv("CTDB_SOCKET");
	if (ctdb_socket != NULL) {
		options.sockpath = ctdb_socket;
	}

	pc = poptGetContext(argv[0], argc, argv, cmdline_options,
			    POPT_CONTEXT_KEEP_FIRST);
	while ((opt = poptGetNextOpt(pc)) != -1) {
		fprintf(stderr, "Invalid option %s\n", poptBadOption(pc, 0));
		exit(1);
	}

	if (options.sockpath == NULL) {
		fprintf(stderr, "Please specify socket path\n");
		poptPrintHelp(pc, stdout, 0);
		exit(1);
	}

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		fprintf(stderr, "Memory allocation error\n");
		exit(1);
	}

	ev = tevent_context_init(mem_ctx);
	if (ev == NULL) {
		fprintf(stderr, "Memory allocation error\n");
		exit(1);
	}

	status = debug_level_parse(options.debuglevel, &log_level);
	if (! status) {
		fprintf(stderr, "Invalid debug level\n");
		poptPrintHelp(pc, stdout, 0);
		exit(1);
	}

	setup_logging("dummy_client", DEBUG_STDERR);
	DEBUGLEVEL = log_level;

	ret = ctdb_client_init(mem_ctx, ev, options.sockpath, &client);
	if (ret != 0) {
		D_ERR("Failed to initialize client, ret=%d\n", ret);
		exit(1);
	}

	done = false;
	if (options.srvidstr != NULL) {
		uint64_t srvid;

		srvid = strtoull(options.srvidstr, NULL, 0);

		ret = ctdb_client_set_message_handler(ev, client, srvid,
						      dummy_handler, &done);
		if (ret != 0) {
			D_ERR("Failed to register srvid, ret=%d\n", ret);
			talloc_free(client);
			exit(1);
		}

		D_INFO("Registered SRVID 0x%"PRIx64"\n", srvid);
	}

	ret = ctdb_client_wait_timeout(ev, &done,
			tevent_timeval_current_ofs(options.timelimit, 0));
	if (ret != 0 && ret == ETIME) {
		D_ERR("client_wait_timeout() failed, ret=%d\n", ret);
		talloc_free(client);
		exit(1);
	}

	talloc_free(client);
	exit(0);
}
