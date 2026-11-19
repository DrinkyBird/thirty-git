// Thirty, a ClassiCube (Minecraft Classic) server
// Copyright (C) 2024 Sean Baggaley
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "server.h"
#include "buffer.h"
#include "client.h"
#include "map.h"
#include "packet.h"
#include "rng.h"
#include "util.h"
#include "mapgen.h"
#include "config.h"
#include "log.h"
#include "namelist.h"
#include "commands.h"

#ifndef _WIN32
#include <netinet/tcp.h>
#endif

#define HEARTBEAT_INTERVAL (45.0)

void server_accept(void);
bool server_generate_salt(char *out, size_t length);

server_t server;

bool server_init(void) {
	server.starttime = time(NULL);
	server.port = config.server.port;
	server.global_rng = rng_create((int)time(NULL));
	server.last_heartbeat = 0.0;
	server.num_clients = 0;
	server.num_spawned_clients = 0;

	if (config.debug.fixed_salt[0] == '\0') {
		if (!server_generate_salt(server.salt, SERVER_SALT_LENGTH)) {
			return false;
		}
	}
	else {
		memcpy(server.salt, config.debug.fixed_salt, util_min(strlen(config.debug.fixed_salt), SERVER_SALT_LENGTH));
	}

	int err;

	err = (server.socket_fd = socket(AF_INET6, SOCK_STREAM, 0));

	if (err == -1) {
		socket_perror("socket");
		return false;
	}

	struct sockaddr_in6 server_addr = { 0 };
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_addr = in6addr_any;
	server_addr.sin6_port = htons((uint16_t)server.port);

#ifdef _WIN32
	BOOL reuseaddr_value = 1;
	DWORD v6only_value = 0;
#else
	int reuseaddr_value = 1;
	int v6only_value = 0;
#endif
	err = setsockopt(server.socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuseaddr_value, sizeof(reuseaddr_value));
	if (err == -1) {
		socket_perror("setsockopt(SO_REUSEADDR)");
		return false;
	}
	err = setsockopt(server.socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only_value, sizeof(v6only_value));
	if (err == -1) {
		socket_perror("setsockopt(IPPROTO_IPV6)");
		return false;
	}

	err = bind(server.socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
	if (err == -1) {
		socket_perror("bind");
		return false;
	}

	// TODO: magic number
	err = listen(server.socket_fd, 10);
	if (err == -1) {
		socket_perror("listen");
		return false;
	}

#ifdef _WIN32
	u_long fionbio_value = 1;
#else
	int fionbio_value = 1;
#endif
	ioctlsocket(server.socket_fd, FIONBIO, &fionbio_value);

	log_printf(log_info, "Server is listening on port %u", server.port);

	log_printf(log_info, "Preparing map...");
	server.map = map_load(config.map.name);

	if (server.map == NULL) {
		log_printf(log_info, "Failed to load map '%s', generating new...", config.map.name);
		server.map = map_create(config.map.name, config.map.width, config.map.depth, config.map.height);

		log_printf(log_info, "Generating map...");
		double start = get_time_s();
		map_generate(server.map, config.map.generator);
		double duration = get_time_s() - start;
		log_printf(log_info, "Map generation took %f seconds", duration);

		map_save(server.map);
	}

	server.ops = namelist_create("ops.txt");
	server.banned_users = namelist_create("banned_users.txt");
	server.banned_ips = namelist_create("banned_ips.txt");
	server.whitelist = namelist_create("whitelist.txt");

	server_heartbeat();
	server.last_heartbeat = get_time_s();

	create_console_standin();

	return true;
}

void server_shutdown(void) {
	namelist_destroy(server.whitelist);
	namelist_destroy(server.banned_ips);
	namelist_destroy(server.banned_users);
	namelist_destroy(server.ops);
	if (server.map != NULL) {
		map_save(server.map);
		map_destroy(server.map);
	}
	closesocket(server.socket_fd);
	rng_destroy(server.global_rng);
}

void server_tick(void) {
	command_tick_readline();
	server_accept();
	map_tick(server.map);

	for (size_t i = 0; i < server.num_clients; i++) {
		client_tick(&server.clients[i]);
	}

	bool removed = false;
	for (size_t i = 0; i < server.num_clients; i++) {
		client_t *client = &server.clients[i];
		if (client->connected) {
			continue;
		}

		client_destroy(client);

		memmove(server.clients + i, server.clients + i + 1, (server.num_clients - i - 1) * sizeof(*server.clients));
		server.num_clients--;
		removed = true;
	}

	if (removed) {
		if (server.num_clients == 0) {
			free(server.clients);
			server.clients = NULL;
			map_save(server.map);
		}
		else {
			server.clients = realloc(server.clients, sizeof(*server.clients) * server.num_clients);
		}
	}

	// Calculate this here, to avoid race condition when the heartbeat thread accesses it.
	server.num_spawned_clients = 0;
	for (size_t i = 0; i < server.num_clients; i++) {
		if (server.clients[i].spawned) {
			server.num_spawned_clients++;
		}
	}

	if (get_time_s() - server.last_heartbeat > HEARTBEAT_INTERVAL) {
		server_heartbeat();
		server.last_heartbeat = get_time_s();
	}

	server.tick++;
}

void server_accept(void) {
	struct sockaddr_storage client_addr;
	socklen_t addr_size = sizeof(client_addr);

	socket_t acceptfd = accept(server.socket_fd, (struct sockaddr *)&client_addr, &addr_size);
	if (acceptfd == INVALID_SOCKET) {
		int e = socket_error();
		if (e == EAGAIN || e == SOCKET_EWOULDBLOCK) {
			// No connections.
			return;
		}

		log_printf(log_error, "accept error %d", e);
		return;
	}

#ifdef _WIN32
	u_long fionbio_value = 1;
	const char nodelay_value = 1;
#else
	int fionbio_value = 1;
	int nodelay_value = 1;
#endif
	ioctlsocket(acceptfd, FIONBIO, &fionbio_value);
	setsockopt(acceptfd, IPPROTO_TCP, TCP_NODELAY, &nodelay_value, sizeof(nodelay_value));

	struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&client_addr;

	char addrstr[NI_MAXHOST];
	stringify_address(sin, addrstr, NI_MAXHOST);

	log_printf(log_info, "Incoming connection from %s:%u", addrstr, sin->sin6_port);

	size_t conn_idx = server.num_clients++;
	server.clients = realloc(server.clients, server.num_clients * sizeof(*server.clients));
	client_t *client = &server.clients[conn_idx];
	client_init(client, acceptfd, conn_idx);
	memcpy(&client->address, sin, sizeof(client->address));

	if (namelist_contains(server.banned_ips, addrstr)) {
		log_printf(log_info, "Client %s is banned!", addrstr);
		client_disconnect(client, "You are banned from this server!");
	}
}

void server_broadcast(const char *msg, ...) {
	char buffer[65];

	va_list args;
	va_start(args, msg);
	vsnprintf(buffer, 65, msg, args);
	va_end(args);

	log_printf(log_info, "%s", buffer);

	for (size_t i = 0; i < server.num_clients; i++) {
		client_t *client = &server.clients[i];
		if (client->protocol_version < 3) {
			continue;
		}

		client_send_message(client, msgtype_chat, "%s", buffer);
	}
}

bool server_generate_salt(char *out, size_t length) {
	const char *chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	const size_t num_chars = strlen(chars);

	if (!util_secure_random(out, length)) {
		return false;
	}

	for (size_t i = 0; i < length; i++) {
		out[i] = chars[out[i] % num_chars];
	}

	return true;
}
