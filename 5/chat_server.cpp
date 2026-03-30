#include "chat.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <queue>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include "unit.h"
#include <fcntl.h>

struct chat_peer {
	/** Client's socket. To read/write messages. */
	int socket;
	/** Output buffer. */
	std::string output_buffer;
	/* PUT HERE OTHER MEMBERS */
	std::string name;
	bool name_set = false;
	std::string recv_buffer;
};

struct chat_server {
	/** Listening socket. To accept new clients. */
	int socket = -1;
	/** Array of peers. */
	std::vector<chat_peer *> peers;
	/* PUT HERE OTHER MEMBERS */
	int kq = -1;
	std::queue<chat_message *> received_messages;
	std::string feed_buffer;
	std::string output_buffer;
};

struct chat_server *
chat_server_new(void)
{
	chat_server *new_chat_server = new chat_server();
	new_chat_server->kq = kqueue();
	return new_chat_server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);

	if (server->kq >= 0) {
        close(server->kq);
    }

	for (chat_peer *peer : server->peers) {
        if (peer->socket >= 0)
            close(peer->socket);
        delete peer;
    }

	while (!server->received_messages.empty()) {
        delete server->received_messages.front();
        server->received_messages.pop();
    }

	delete server;
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket > 0) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = htons(port);
	/* Listen on all IPs of this machine. */
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	/*
	 * 1) Create a server socket (function socket()).
	 * 2) Bind the server socket to addr (function bind()).
	 * 3) Listen the server socket (function listen()).
	 * 4) Create epoll/kqueue if needed.
	 */

	int new_socket = socket(AF_INET, SOCK_STREAM, 0);

	if (bind(new_socket, (sockaddr *)&addr, sizeof(addr)) < 0) {
        close(new_socket);
		if (errno == EADDRINUSE) {
			return CHAT_ERR_PORT_BUSY;
		}
        return CHAT_ERR_SYS;
    }

	listen(new_socket, SOMAXCONN);
	

	int flags = fcntl(new_socket, F_GETFL, 0);
    fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);

	server->socket = new_socket;
	if (server->kq < 0) {
		server->kq = kqueue();
	}

	struct kevent ev;
	EV_SET(&ev, server->socket, EVFILT_READ, EV_ADD|EV_CLEAR, 0, 0, NULL);
	if (kevent(server->kq, &ev, 1, NULL, 0, NULL) < 0) {
		return CHAT_ERR_SYS;
	}

	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	if (server->received_messages.empty()) {
		return NULL;
	}

	chat_message *msg = server->received_messages.front();
	server->received_messages.pop();

	return msg;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	/*
	 * 1) Wait on epoll/kqueue/poll for update on any socket.
	 * 2) Handle the update.
	 * 2.1) If the update was on listen-socket, then you probably need to
	 *     call accept() on it - a new client wants to join.
	 * 2.2) If the update was on a client-socket, then you might want to
	 *     read/write on it.
	 */
	if (server->socket < 0) {
		return CHAT_ERR_NOT_STARTED;
	}

	timespec ts;
	ts.tv_sec = (time_t)timeout;
	ts.tv_nsec = (long)((timeout - ts.tv_sec) * 1000000000);
	if (timeout == 0) {
		ts.tv_nsec = 100000;
	}

	struct kevent events[1024];
	int kevent_count = kevent(server->kq, NULL, 0, events, 1024, &ts);

	if (kevent_count == 0 && server->output_buffer.empty()) {
		return CHAT_ERR_TIMEOUT;
	}
	if (kevent_count < 0) {
		return CHAT_ERR_SYS;
	}

	for (int i = 0; i < kevent_count; i++) {
		if (events[i].udata == NULL) {
			int new_socket = 1;
			while (new_socket > 0) {
				struct sockaddr_in new_addr;
				socklen_t addr_len = sizeof(new_addr);
				new_socket = accept(server->socket,(sockaddr *)&new_addr, &addr_len);
				if (new_socket > 0) {
					int flags = fcntl(new_socket, F_GETFL, 0);
					fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);

					chat_peer *new_peer = new chat_peer();
					new_peer->socket = new_socket;
					server->peers.push_back(new_peer);

					struct kevent ev_read;
					EV_SET(&ev_read, new_socket, EVFILT_READ, EV_ADD|EV_CLEAR, 0, 0, new_peer);
					kevent(server->kq, &ev_read, 1, NULL, 0, NULL);

					struct kevent ev_write;
					EV_SET(&ev_write, new_socket, EVFILT_WRITE, EV_ADD|EV_CLEAR, 0, 0, new_peer);
					kevent(server->kq, &ev_write, 1, NULL, 0, NULL);

				}
				else if (new_socket < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
					}
                }
			}
		}
		else {
			chat_peer *peer = (chat_peer *) events[i].udata;
			if (!peer) {
				continue;
			}
			if (events[i].filter == EVFILT_READ) {
				ssize_t received = 1;
				while (received > 0) {
					char read_buf[1024 * 16];
					received = recv(peer->socket, read_buf, sizeof(read_buf), 0);
					if (received > 0) {
						peer->recv_buffer.append(read_buf, received);
					}
					else if (received == 0) {
						close(peer->socket);
					}
					else if (received < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
					}
				}
				
				size_t msg_begin = 0;
				size_t msg_end = 0;
				while ((msg_end = peer->recv_buffer.find('\n', msg_begin)) != std::string::npos) {
					std::string msg = peer->recv_buffer.substr(msg_begin, msg_end - msg_begin);
					msg_begin = msg_end + 1;

					if (!peer->name_set) {
						peer->name = msg;
						peer->name_set = true;
					}
					else {
						chat_message *new_chat_message = new chat_message();
						new_chat_message->author = peer->name;
						new_chat_message->data = msg;
						server->received_messages.push(new_chat_message);

						for (chat_peer *server_peer : server->peers) {
							if (server_peer != peer) {
								server_peer->output_buffer.append(
									new_chat_message->author + ":" + new_chat_message->data + "\n");
								
								while (!server_peer->output_buffer.empty()) {
									ssize_t sent = send(server_peer->socket, 
														server_peer->output_buffer.c_str(), 
														server_peer->output_buffer.size(), 0);
									if (sent > 0) {
										server_peer->output_buffer.erase(0, sent);
									}
									else if (sent < 0) {
										if (errno == EAGAIN || errno == EWOULDBLOCK) {
											break;
										}
									}
								}
								
							}
						}
					}
				}
				if (msg_begin > 0) {
					if (msg_begin >= peer->recv_buffer.size()) {
						peer->recv_buffer.clear();
					} else {
						peer->recv_buffer.erase(0, msg_begin);
					}
				}
			}
			if (events[i].filter == EVFILT_WRITE) {
				while (!peer->output_buffer.empty()) {
					ssize_t sent = send(peer->socket, 
										peer->output_buffer.c_str(), 
										peer->output_buffer.size(), 0);
					if (sent > 0) {
						peer->output_buffer.erase(0, sent);
					}
					else if (sent < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							break;
						}
					}
				}
			}
		}
	}

	if (!server->output_buffer.empty()) {
        size_t msg_begin = 0;
        size_t msg_end;
        
        while ((msg_end = server->output_buffer.find('\n', msg_begin)) != std::string::npos) {
            std::string msg = server->output_buffer.substr(msg_begin, msg_end - msg_begin);
			msg_begin = msg_end + 1;
            
			chat_message *new_chat_message = new chat_message();
			new_chat_message->author = "server";
			new_chat_message->data = msg;
			server->received_messages.push(new_chat_message);
			
			for (chat_peer *server_peer : server->peers) {
				if (server_peer->socket >= 0) {
					server_peer->output_buffer.append(
						new_chat_message->author + ":" + new_chat_message->data + "\n");
				}

				while (!server_peer->output_buffer.empty()) {
					ssize_t sent = send(server_peer->socket, 
										server_peer->output_buffer.c_str(), 
										server_peer->output_buffer.size(), 0);
					if (sent > 0) {
						server_peer->output_buffer.erase(0, sent);
					}
					else if (sent < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							break;
						}
					}
				}
			}
            
        }
		if (msg_begin > 0) {
			server->output_buffer.erase(0, msg_begin);
		}
		if (kevent_count == 0) {
			return CHAT_ERR_TIMEOUT;
		}
    }

	return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */

	/*
	 * Server has multiple sockets - own and from connected clients. Hence
	 * you can't return a socket here. But if you are using epoll/kqueue,
	 * then you can return their descriptor. These descriptors can be polled
	 * just like sockets and will return an event when any of their owned
	 * descriptors has any events.
	 *
	 * For example, assume you created an epoll descriptor and added to
	 * there a listen-socket and a few client-sockets. Now if you will call
	 * poll() on the epoll's descriptor, then on return from poll() you can
	 * be sure epoll_wait() can return something useful for some of those
	 * sockets.
	 */
#endif
	return server->kq;
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	if (server->socket < 0) {
		return 0;
	}
	for (chat_peer *peer : server->peers) {
		if (!peer->output_buffer.empty()) {
			return CHAT_EVENT_INPUT|CHAT_EVENT_OUTPUT;
		}
	}
	return CHAT_EVENT_INPUT;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
	if (server->socket < 0) {
		return CHAT_ERR_NOT_STARTED;
	}

	server->feed_buffer.append(msg, msg_size);

	size_t msg_begin = 0;
	size_t msg_end = 0;
	while ((msg_end = server->feed_buffer.find('\n', msg_begin)) != std::string::npos) {
		std::string msg = server->feed_buffer.substr(msg_begin, msg_end - msg_begin);

		size_t trimmed_msg_begin = msg.find_first_not_of(" \n");

		if (trimmed_msg_begin != std::string::npos) {
			size_t trimmed_msg_end = msg.find_last_not_of(" \n");
			std::string trimmed_msg = msg.substr(trimmed_msg_begin, trimmed_msg_end - trimmed_msg_begin + 1);
			server->output_buffer.append(trimmed_msg + '\n');
		}
		msg_begin = msg_end + 1;
	}
	if (msg_begin > 0) {
		if (msg_begin >= server->feed_buffer.size()) {
			server->feed_buffer.clear();
		} else {
			server->feed_buffer.erase(0, msg_begin);
		}
	}

	return 0;
}
