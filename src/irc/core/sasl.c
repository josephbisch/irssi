/*
    fe-sasl.c : irssi

    Copyright (C) 2015 The Lemon Man

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

#include "module.h"
#include "misc.h"
#include "settings.h"

#include "irc-cap.h"
#include "irc-servers.h"
#include "sasl.h"

/*
 * Based on IRCv3 SASL Extension Specification:
 * http://ircv3.net/specs/extensions/sasl-3.1.html
 */
#define AUTHENTICATE_CHUNK_SIZE 400 // bytes

/*
 * Maximum size to allow the buffer to grow to before the next fragment comes in. Note that
 * due to the way fragmentation works, the maximum message size will actually be:
 * floor(AUTHENTICATE_MAX_SIZE / AUTHENTICATE_CHUNK_SIZE) + AUTHENTICATE_CHUNK_SIZE - 1
 */
#define AUTHENTICATE_MAX_SIZE 8192 // bytes

#define SASL_TIMEOUT (20 * 1000) // ms

static gboolean sasl_timeout(IRC_SERVER_REC *server)
{
	/* The authentication timed out, we can't do much beside terminating it */
	irc_send_cmd_now(server, "AUTHENTICATE *");
	cap_finish_negotiation(server);

	server->sasl_timeout = 0;

	signal_emit("server sasl failure", 2, server, "The authentication timed out");

	return FALSE;
}

static void sasl_start(IRC_SERVER_REC *server, const char *data, const char *from)
{
	IRC_SERVER_CONNECT_REC *conn;

	conn = server->connrec;

	switch (conn->sasl_mechanism) {
		case SASL_MECHANISM_PLAIN:
			irc_send_cmd_now(server, "AUTHENTICATE PLAIN");
			break;

		case SASL_MECHANISM_EXTERNAL:
			irc_send_cmd_now(server, "AUTHENTICATE EXTERNAL");
			break;
	}
	server->sasl_timeout = g_timeout_add(SASL_TIMEOUT, (GSourceFunc) sasl_timeout, server);
}

static void sasl_fail(IRC_SERVER_REC *server, const char *data, const char *from)
{
	char *params, *error;

	/* Stop any pending timeout, if any */
	if (server->sasl_timeout != 0) {
		g_source_remove(server->sasl_timeout);
		server->sasl_timeout = 0;
	}

	params = event_get_params(data, 2, NULL, &error);

	signal_emit("server sasl failure", 2, server, error);

	/* Terminate the negotiation */
	cap_finish_negotiation(server);

	g_free(params);
}

static void sasl_already(IRC_SERVER_REC *server, const char *data, const char *from)
{
	if (server->sasl_timeout != 0) {
		g_source_remove(server->sasl_timeout);
		server->sasl_timeout = 0;
	}

	signal_emit("server sasl success", 1, server);

	/* We're already authenticated, do nothing */
	cap_finish_negotiation(server);
}

static void sasl_success(IRC_SERVER_REC *server, const char *data, const char *from)
{
	if (server->sasl_timeout != 0) {
		g_source_remove(server->sasl_timeout);
		server->sasl_timeout = 0;
	}

	signal_emit("server sasl success", 1, server);

	/* The authentication succeeded, time to finish the CAP negotiation */
	cap_finish_negotiation(server);
}

/*
 * Responsible for reassembling incoming SASL requests. SASL requests must be split
 * into 400 byte requests to stay below the IRC command length limit of 512 bytes.
 * The spec says that if there is 400 bytes, then there is expected to be a
 * continuation in the next chunk. If a message is exactly a multiple of 400 bytes,
 * there must be a blank message of "AUTHENTICATE +" to indicate the end.
 *
 * This function returns the fully reassembled and decoded AUTHENTICATION message if
 * completed or NULL if there are more messages expected.
 */
static gboolean sasl_reassemble_incoming(IRC_SERVER_REC *server, const char *fragment, GString **decoded)
{
	GString *enc_req;
	gsize fragment_len;

	fragment_len = strlen(fragment);

	/* Check if there is an existing fragment to prepend. */
	if (server->sasl_buffer != NULL) {
		if (g_strcmp0("+", fragment) == 0) {
			enc_req = server->sasl_buffer;
		} else {
			enc_req = g_string_append_len(server->sasl_buffer, fragment, fragment_len);
		}
		server->sasl_buffer = NULL;
	} else {
		enc_req = g_string_new_len(fragment, fragment_len);
	}

	/*
	 * Fail authentication with this server. They have sent too much data.
	 */
	if (enc_req->len > AUTHENTICATE_MAX_SIZE) {
		return FALSE;
	}

	/*
	 * If the the request is exactly the chunk size, this is a fragment
	 * and more data is expected.
	 */
	if (fragment_len == AUTHENTICATE_CHUNK_SIZE) {
		server->sasl_buffer = enc_req;
		return TRUE;
	}

	if (enc_req->len == 1 && *enc_req->str == '+') {
		*decoded = g_string_new_len("", 0);
	} else {
		gsize dec_len;
		gchar *tmp;

		tmp = (gchar *) g_base64_decode(enc_req->str, &dec_len);
		*decoded = g_string_new_len(tmp, dec_len);
	}

	g_string_free(enc_req, TRUE);
	return TRUE;
}

/*
 * Splits the response into appropriately sized chunks for the AUTHENTICATION
 * command to be sent to the IRC server. If |response| is NULL, then the empty
 * response is sent to the server.
 */
void sasl_send_response(IRC_SERVER_REC *server, GString *response)
{
	char *enc;
	size_t offset, enc_len, chunk_len;

	if (response == NULL) {
		irc_send_cmdv(server, "AUTHENTICATE +");
		return;
	}

	enc = g_base64_encode((guchar *) response->str, response->len);
	enc_len = strlen(enc);

	for (offset = 0; offset < enc_len; offset += AUTHENTICATE_CHUNK_SIZE) {
		chunk_len = enc_len - offset;
		if (chunk_len > AUTHENTICATE_CHUNK_SIZE)
			chunk_len = AUTHENTICATE_CHUNK_SIZE;

		irc_send_cmdv(server, "AUTHENTICATE %.*s", (int) chunk_len, enc + offset);
	}

	if (offset == enc_len) {
		irc_send_cmdv(server, "AUTHENTICATE +");
	}
	g_free(enc);
}

/*
 * Called when the incoming SASL request is completely received.
 */
static void sasl_step_complete(IRC_SERVER_REC *server, GString *data)
{
	IRC_SERVER_CONNECT_REC *conn;
	GString *resp;

	conn = server->connrec;

	switch (conn->sasl_mechanism) {
		case SASL_MECHANISM_PLAIN:
			/* At this point we assume that conn->sasl_{username, password} are non-NULL.
			 * The PLAIN mechanism expects a NULL-separated string composed by the authorization identity, the
			 * authentication identity and the password.
			 * The authorization identity field is explicitly set to the user provided username.
			 */

			resp = g_string_new(NULL);

			g_string_append(resp, conn->sasl_username);
			g_string_append_c(resp, '\0');
			g_string_append(resp, conn->sasl_username);
			g_string_append_c(resp, '\0');
			g_string_append(resp, conn->sasl_password);

			sasl_send_response(server, resp);
			g_string_free(resp, TRUE);

			break;

		case SASL_MECHANISM_EXTERNAL:
			/* Empty response */
			sasl_send_response(server, NULL);
			break;
	}
}

static void sasl_step_fail(IRC_SERVER_REC *server)
{
	irc_send_cmd_now(server, "AUTHENTICATE *");
	cap_finish_negotiation(server);

	server->sasl_timeout = 0;

	signal_emit("server sasl failure", 2, server, "The server sent an invalid payload");
}

static void sasl_step(IRC_SERVER_REC *server, const char *data, const char *from)
{
	GString *req = NULL;

	/* Stop the timer */
	if (server->sasl_timeout != 0) {
		g_source_remove(server->sasl_timeout);
		server->sasl_timeout = 0;
	}

	if (!sasl_reassemble_incoming(server, data, &req)) {
		sasl_step_fail(server);
		return;
	}

	if (req != NULL) {
		sasl_step_complete(server, req);
		g_string_free(req, TRUE);
	}

	/* We expect a response within a reasonable time */
	server->sasl_timeout = g_timeout_add(SASL_TIMEOUT, (GSourceFunc) sasl_timeout, server);
}

static void sasl_disconnected(IRC_SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	if (!IS_IRC_SERVER(server)) {
		return;
	}

	if (server->sasl_timeout != 0) {
		g_source_remove(server->sasl_timeout);
		server->sasl_timeout = 0;
	}
}

void sasl_init(void)
{
	signal_add_first("server cap ack sasl", (SIGNAL_FUNC) sasl_start);
	signal_add_first("event authenticate", (SIGNAL_FUNC) sasl_step);
	signal_add_first("event 903", (SIGNAL_FUNC) sasl_success);
	signal_add_first("event 902", (SIGNAL_FUNC) sasl_fail);
	signal_add_first("event 904", (SIGNAL_FUNC) sasl_fail);
	signal_add_first("event 905", (SIGNAL_FUNC) sasl_fail);
	signal_add_first("event 906", (SIGNAL_FUNC) sasl_fail);
	signal_add_first("event 907", (SIGNAL_FUNC) sasl_already);
	signal_add_first("server disconnected", (SIGNAL_FUNC) sasl_disconnected);
}

void sasl_deinit(void)
{
	signal_remove("server cap ack sasl", (SIGNAL_FUNC) sasl_start);
	signal_remove("event authenticate", (SIGNAL_FUNC) sasl_step);
	signal_remove("event 903", (SIGNAL_FUNC) sasl_success);
	signal_remove("event 902", (SIGNAL_FUNC) sasl_fail);
	signal_remove("event 904", (SIGNAL_FUNC) sasl_fail);
	signal_remove("event 905", (SIGNAL_FUNC) sasl_fail);
	signal_remove("event 906", (SIGNAL_FUNC) sasl_fail);
	signal_remove("event 907", (SIGNAL_FUNC) sasl_already);
	signal_remove("server disconnected", (SIGNAL_FUNC) sasl_disconnected);
}
