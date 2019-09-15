/*
 server.c : irssi

    Copyright (C) 2018 Joseph Bisch

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <irssi/src/irc/core/module.h>
#include <irssi/src/core/modules-load.h>
#include <irssi/src/fe-text/module-formats.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/fe-common/core/themes.h>
#include <irssi/src/core/core.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/core/args.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/servers-setup.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/irc-channels.h>
#include <irssi/src/irc/core/irc-cap.h>
#include <irssi/src/irc/core/irc-channels.h>
#include <irssi/src/fe-common/core/fe-windows.h>

/* irc.c */
void irc_init(void);
void irc_deinit(void);

/* irc-session.c */
void irc_session_init(void);
void irc_session_deinit(void);

/* fe-common-irc.c */
void fe_common_irc_init(void);
void fe_common_irc_deinit(void);

/* irc-cap.c */
void cap_init(void);
void cap_deinit(void);

void irc_core_init(void);
void irc_core_deinit(void);

SERVER_REC *server;

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
static void null_logger(const gchar *log_domain,
			GLogLevelFlags log_level,
			const gchar *message,
			gpointer user_data)
{
	return;
}
#endif

void event_connected(IRC_SERVER_REC *server, const char *data, const char *from)
{
	char *params, *nick;

	g_return_if_fail(server != NULL);

	params = event_get_params(data, 1, &nick);

	if (g_strcmp0(server->nick, nick) != 0) {
		/* nick changed unexpectedly .. connected via proxy, etc. */
		g_free(server->nick);
		server->nick = g_strdup(nick);
	}

	/* set the server address */
	g_free(server->real_address);
	server->real_address = from == NULL ?
		g_strdup(server->connrec->address) : /* shouldn't happen.. */
		g_strdup(from);

	/* last welcome message found - commands can be sent to server now. */
	server->connected = 1;
	server->real_connect_time = time(NULL);

	/* let the queue send now that we are identified */
	g_get_current_time(&server->wait_cmd);

	if (server->connrec->usermode != NULL) {
		/* Send the user mode, before the autosendcmd.
		 * Do not pass this through cmd_mode because it
		 * is not known whether the resulting MODE message
		 * (if any) is the initial umode or a reply to this.
		 */
		irc_send_cmdv(server, "MODE %s %s", server->nick,
				server->connrec->usermode);
		g_free_not_null(server->wanted_usermode);
		server->wanted_usermode = g_strdup(server->connrec->usermode);
	}

	signal_emit("event connected", 1, server);
	g_free(params);
}

void irc_server_init_bare_minimum(IRC_SERVER_REC *server) {
	server->isupport = g_hash_table_new((GHashFunc) g_istr_hash,
					    (GCompareFunc) g_istr_equal);

	/* set the standards */
	g_hash_table_insert(server->isupport, g_strdup("CHANMODES"), g_strdup("beI,k,l,imnpst"));
	g_hash_table_insert(server->isupport, g_strdup("PREFIX"), g_strdup("(ohv)@%+"));
}

void test_server() {
	//SERVER_REC *server; /* = g_new0(IRC_SERVER_REC, 1); */
	CHAT_PROTOCOL_REC *proto;
	SERVER_CONNECT_REC *conn;

	proto = chat_protocol_find("IRC");
	conn = server_create_conn(proto->id, "localhost", 0, "", "", "user");
	server = proto->server_init_connect(conn);
	server->session_reconnect = TRUE;
	g_free(server->tag);
	server->tag = g_strdup("testserver");

	/* we skip some initialisations that would try to send data */
	/* irc_servers_deinit(); */
	//irc_core_deinit();
	irc_session_deinit();
	irc_irc_deinit();

	server_connect_finished(server);

	/* make up for the skipped session init */
	irc_server_init_bare_minimum(IRC_SERVER(server));

	irc_irc_init();
	irc_session_init();
	//irc_core_init();
	/* irc_servers_init(); */

	server_connect_unref(conn);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	g_log_set_default_handler(null_logger, NULL);
#endif
	core_register_options();
	fe_common_core_register_options();
	/* no args */
	args_execute(0, NULL);
	core_preinit((*argv)[0]);
	core_init();
	irc_init();
	irc_core_init();
	//irc_channels_init();
	fe_common_core_init();
	fe_common_irc_init();
	signal_add("event 001", (SIGNAL_FUNC) event_connected);
	module_register("core", "fe-fuzz");
	window_create(NULL, 1);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	if (size < 2) return 0;
	gboolean prefixedChoice = (gboolean)*data;
	gboolean inputChoice = (gboolean)*(data+1);
	gchar *copy;
	gchar **lines;
	gchar **head;

	if (size < 2) return 0;

	test_server();


	copy = g_strndup((const gchar *)data+2, size-2);
	//lines = g_strsplit(copy, "\r\n", -1);
	lines = g_strsplit(copy, "\n", -1);
	head = lines;

	signal_emit("send command", 3, "/set autofocus_new_items ON\n", server, NULL);

	for (; *lines != NULL; lines++) {
		gchar *prefixedLine;
		if (/*!inputChoice && */prefixedChoice) {
			prefixedLine = g_strdup_printf(":user %s\n", *lines);
		} else if (0) {//inputChoice) {
			prefixedLine = g_strdup_printf("/%s\n", *lines);
		} else {
			prefixedLine = g_strdup_printf("%s\n", *lines);
		}
		if (1/*!inputChoice*/) {
			signal_emit("server incoming", 2, server, prefixedLine);
		} else {
			signal_emit("send command", 3, prefixedLine, server, NULL);
		}
		g_free(prefixedLine);
	}

	g_strfreev(head);
	g_free(copy);
	server_disconnect(server);
	return 0;
}
