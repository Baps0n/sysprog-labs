#include "chat.h"
#include "chat_client.h"

#include <cstring>
#include <stdlib.h>
#include <unistd.h>
#include <queue>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unit.h>

struct chat_client {
	/** Socket connected to the server. */
	int socket = -1;
	/** Array of received messages. */
	std::queue<chat_message *> received_messages;
	/** Output buffer. */
	std::string output_buffer;
	/* PUT HERE OTHER MEMBERS */
	std::string name;
	std::string recv_buffer;
	std::string feed_buffer;
};

struct chat_client *
chat_client_new(std::string_view name)
{
	/* Ignore 'name' param if don't want to support it for +5 points. */
	chat_client *new_chat_client = new chat_client();
	new_chat_client->name = name;

	return new_chat_client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		close(client->socket);

	while (!client->received_messages.empty()) {
        delete client->received_messages.front();
        client->received_messages.pop();
    }
	
	delete client;
}

int
chat_client_connect(struct chat_client *client, std::string_view addr)
{
	/*
	 * 1) Use getaddrinfo() to resolve addr to struct sockaddr_in.
	 * 2) Create a client socket (function socket()).
	 * 3) Connect it by the found address (function connect()).
	 */
	if (client->socket >= 0) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	size_t split_pos = addr.find(':');
	std::string host(addr.substr(0, split_pos));
	std::string port(addr.substr(split_pos + 1));

	struct addrinfo hints = {};
	struct addrinfo *addrinfo_res = NULL;
	hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addrinfo_res) != 0) {
		return CHAT_ERR_NO_ADDR;
	}

	int new_socket = socket(addrinfo_res->ai_family, addrinfo_res->ai_socktype, addrinfo_res->ai_protocol);
	connect(new_socket, addrinfo_res->ai_addr, addrinfo_res->ai_addrlen);

	int flags = fcntl(new_socket, F_GETFL, 0);
	fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);

	client->socket = new_socket;
	client->output_buffer += client->name + '\n';

	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	if (client->received_messages.empty()) {
		return NULL;
	}

	chat_message *new_message = client->received_messages.front();
	client->received_messages.pop();

	return new_message;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	/*
	 * The easiest way to wait for updates on a single socket with a timeout
	 * is to use poll(). Epoll is good for many sockets, poll is good for a
	 * few.
	 *
	 * You create one struct pollfd, fill it, call poll() on it, handle the
	 * events (do read/write).
	 */
	if (client->socket < 0) {
        return CHAT_ERR_NOT_STARTED;
    }
    
    struct pollfd pfd = {};
    pfd.fd = client->socket;
    pfd.events = POLLIN;
    if (!client->output_buffer.empty()) {
        pfd.events |= POLLOUT;
    }
    
    int poll_res = poll(&pfd, 1, (int)(timeout * 1000));
    if (poll_res == 0) {
        return CHAT_ERR_TIMEOUT;
    }
    else if (poll_res < 0) {
        return CHAT_ERR_SYS;
    }

    if (pfd.revents & POLLIN) {
		ssize_t received = 1;
        while (received > 0) {
            char read_buf[1024 * 16];
            received = recv(client->socket, read_buf, sizeof(read_buf), 0);
            
            if (received > 0) {
                client->recv_buffer.append(read_buf, received);
            }
            else if (received == 0) {
                close(client->socket);
                client->socket = -1;
                return CHAT_ERR_SYS;
            }
            else if (received < 0){
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
            }
        }
        
        size_t msg_begin = 0;
        size_t msg_end = 0;
        while ((msg_end = client->recv_buffer.find('\n', msg_begin)) != std::string::npos) {
            size_t author_end = client->recv_buffer.find(':', msg_begin);
            
            if (author_end != std::string::npos && author_end < msg_end) {
                std::string author = client->recv_buffer.substr(msg_begin, author_end - msg_begin);
                std::string message = client->recv_buffer.substr(author_end + 1, msg_end - (author_end + 1));
                
                chat_message *new_message = new chat_message();
                new_message->author = author;
                new_message->data = message;
                client->received_messages.push(new_message);
            }
            msg_begin = msg_end + 1;
        }
        
        if (msg_begin > 0) {
			if (msg_begin >= client->recv_buffer.size()) {
				client->recv_buffer.clear();
			} else {
				client->recv_buffer.erase(0, msg_begin);
			}
		}
    }

	if (pfd.revents & POLLOUT) {
		while (!client->output_buffer.empty()) {
			ssize_t sent = send(client->socket, 
								client->output_buffer.c_str(), 
								client->output_buffer.size(), 0);
			if (sent > 0) {
				client->output_buffer.erase(0, sent);
			}
			else if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
            }
        }
	}

	return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if (client->socket < 0) {
		return 0;
	}
	if (!client->output_buffer.empty()) {
		return CHAT_EVENT_INPUT|CHAT_EVENT_OUTPUT;
	}
	return CHAT_EVENT_INPUT;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if (client->socket < 0) {
		return CHAT_ERR_NOT_STARTED;
	}
	client->feed_buffer.append(msg, msg_size);

	size_t msg_begin = 0;
	size_t msg_end = 0;
	while ((msg_end = client->feed_buffer.find('\n', msg_begin)) != std::string::npos) {
		std::string msg = client->feed_buffer.substr(msg_begin, msg_end - msg_begin);

		size_t trimmed_msg_begin = msg.find_first_not_of(" \n");
		if (trimmed_msg_begin != std::string::npos) {
			size_t trimmed_msg_end = msg.find_last_not_of(" \n");
			std::string trimmed_msg = msg.substr(trimmed_msg_begin, trimmed_msg_end - trimmed_msg_begin + 1);
			client->output_buffer.append(trimmed_msg + '\n');
		}
		msg_begin = msg_end + 1;
	}
	if (msg_begin > 0) {
		if (msg_begin >= client->feed_buffer.size()) {
			client->feed_buffer.clear();
		} else {
			client->feed_buffer.erase(0, msg_begin);
		}
	}

	return 0;
}
