#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <unit.h>

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

#if 1 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

static void
wakeup_queue_wakeup_del(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	rlist_del_entry(entry, base);
	coro_wakeup(entry->coro);
}

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	std::vector<unsigned> data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = new coro_bus;
	bus->channels = NULL;
	bus->channel_count = 0;

	return bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] != NULL) {
			coro_bus_channel_close(bus, i);
			coro_yield();
		}
	}
	delete[] bus->channels;
	delete bus;
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			bus->channels[i] = new coro_bus_channel;
			bus->channels[i]->size_limit = size_limit;
			rlist_create(&bus->channels[i]->send_queue.coros);
			rlist_create(&bus->channels[i]->recv_queue.coros);
			return i;
		}
	}
	int new_channel_desc = bus->channel_count;

	int new_channel_count = bus->channel_count * 2;
	if (bus->channel_count == 0) {
		new_channel_count = 1;
	}

	coro_bus_channel **new_channels = new coro_bus_channel*[new_channel_count];
	std::copy(bus->channels, bus->channels + bus->channel_count, new_channels);
	delete[] bus->channels;

	bus->channels = new_channels;
	bus->channel_count = new_channel_count;
	bus->channels[new_channel_desc] = new coro_bus_channel;
	bus->channels[new_channel_desc]->size_limit = size_limit;
	rlist_create(&bus->channels[new_channel_desc]->send_queue.coros);
	rlist_create(&bus->channels[new_channel_desc]->recv_queue.coros);

	return new_channel_desc;

	/*
	 * One of the tests will force you to reuse the channel
	 * descriptors. It means, that if your maximal channel
	 * descriptor is N, and you have any free descriptor in
	 * the range 0-N, then you should open the new channel on
	 * that old descriptor.
	 *
	 * A more precise instruction - check if any of the
	 * bus->channels[i] with i = 0 -> bus->channel_count is
	 * free (== NULL). If yes - reuse the slot. Don't grow the
	 * bus->channels array, when have space in it.
	 */
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel_desc)
{
	if (bus->channels == NULL || bus->channels[channel_desc] == NULL) {
		return;
	}

	while (!rlist_empty(&bus->channels[channel_desc]->send_queue.coros)) {
		wakeup_queue_wakeup_del(&bus->channels[channel_desc]->send_queue);
	}
	while (!rlist_empty(&bus->channels[channel_desc]->recv_queue.coros)) {
		wakeup_queue_wakeup_del(&bus->channels[channel_desc]->recv_queue);
	}

	delete bus->channels[channel_desc];
	bus->channels[channel_desc] = NULL;
	
	/*
	 * Be very attentive here. What happens, if the channel is
	 * closed while there are coroutines waiting on it? For
	 * example, the channel was empty, and some coros were
	 * waiting on its recv_queue.
	 *
	 * If you wakeup those coroutines and just delete the
	 * channel right away, then those waiting coroutines might
	 * on wakeup try to reference invalid memory.
	 *
	 * Can happen, for example, if you use an intrusive list
	 * (rlist), delete the list itself (by deleting the
	 * channel), and then the coroutines on wakeup would try
	 * to remove themselves from the already destroyed list.
	 *
	 * Think how you could address that. Remove all the
	 * waiters from the list before freeing it? Yield this
	 * coroutine after waking up the waiters but before
	 * freeing the channel, so the waiters could safely leave?
	 */
}

int
coro_bus_send(struct coro_bus *bus, int channel_desc, unsigned data)
{
	while (coro_bus_try_send(bus, channel_desc, data) != 0) {
		if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			wakeup_queue_suspend_this(&bus->channels[channel_desc]->send_queue);
		}
		else {
			return -1;
		}
	}
	if (bus->channels[channel_desc]->data.size() < bus->channels[channel_desc]->size_limit) {
		wakeup_queue_wakeup_first(&bus->channels[channel_desc]->send_queue);
	}
	return 0;
	
	/*
	 * Try sending in a loop, until success. If error, then
	 * check which one is that. If 'wouldblock', then suspend
	 * this coroutine and try again when woken up.
	 *
	 * If see the channel has space, then wakeup the first
	 * coro in the send-queue. That is needed so when there is
	 * enough space for many messages, and many coroutines are
	 * waiting, they would then wake each other up one by one
	 * as lone as there is still space.
	 */
}

int
coro_bus_try_send(struct coro_bus *bus, int channel_desc, unsigned data)
{
	if (bus->channels == NULL || bus->channels[channel_desc] == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (bus->channels[channel_desc]->data.size() >= bus->channels[channel_desc]->size_limit) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	bus->channels[channel_desc]->data.push_back(data);
	wakeup_queue_wakeup_first(&bus->channels[channel_desc]->recv_queue);
	return 0;

	/*
	 * Append data if has space. Otherwise 'wouldblock' error.
	 * Wakeup the first coro in the recv-queue! To let it know
	 * there is data.
	 */
}

int
coro_bus_recv(struct coro_bus *bus, int channel_desc, unsigned *data)
{
	while (coro_bus_try_recv(bus, channel_desc, data) != 0) {
		if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			wakeup_queue_suspend_this(&bus->channels[channel_desc]->recv_queue);
		}
		else {
			return -1;
		}
	}

	return 0;
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel_desc, unsigned *data)
{
	if (bus->channels == NULL || bus->channels[channel_desc] == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (bus->channels[channel_desc]->data.empty()) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	*data = bus->channels[channel_desc]->data.front();
	bus->channels[channel_desc]->data.erase(bus->channels[channel_desc]->data.begin());
	wakeup_queue_wakeup_first(&bus->channels[channel_desc]->send_queue);
	return 0;
}


#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	if (bus->channels == NULL || bus == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	bool has_non_null = false;
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] != NULL) {
			has_non_null = true;
		}
	}
	if (!has_non_null) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	while (coro_bus_try_broadcast(bus, data) != 0) {
		if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			for (int i = 0; i < bus->channel_count; i++) {
				if (bus->channels[i] != NULL) {
					if (bus->channels[i]->data.size() >= bus->channels[i]->size_limit) {
						wakeup_queue_suspend_this(&bus->channels[i]->send_queue);
					}
				}
			}
		}
		else {
			return -1;
		}
	}
	return 0;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (bus->channels == NULL || bus == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	bool has_non_null = false;
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] != NULL) {
			has_non_null = true;
			if (bus->channels[i]->data.size() >= bus->channels[i]->size_limit) {
				coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
				return -1;
			}
		}
	}
	if (!has_non_null) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] != NULL) {
			bus->channels[i]->data.push_back(data);
			wakeup_queue_wakeup_first(&bus->channels[i]->recv_queue);
		}
	}

	return 0;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel_desc, const unsigned *data, unsigned count)
{
	int retval = -1;
	while (retval < 0) {
		retval = coro_bus_try_send_v(bus, channel_desc, data, count);
		if (retval >= 0) {
			break;
		}
		else if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			wakeup_queue_suspend_this(&bus->channels[channel_desc]->send_queue);
		}
		else {
			return -1;
		}
	}

	if (bus->channels[channel_desc]->data.size() < bus->channels[channel_desc]->size_limit) {
		wakeup_queue_wakeup_first(&bus->channels[channel_desc]->send_queue);
	}

	return retval;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel_desc, const unsigned *data, unsigned count)
{
	if (bus->channels == NULL || bus->channels[channel_desc] == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	
	size_t size_left = bus->channels[channel_desc]->size_limit - bus->channels[channel_desc]->data.size();
	if (size_left <= 0) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned send_count = count;
	if (count > size_left) {
		send_count = size_left;
	}

	for (unsigned i = 0; i < send_count; i++) {
		bus->channels[channel_desc]->data.push_back(data[i]);
	}

	wakeup_queue_wakeup_first(&bus->channels[channel_desc]->recv_queue);

	return send_count;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel_desc, unsigned *data, unsigned capacity)
{
	int retval = -1;
	while (retval < 0) {
		retval = coro_bus_try_recv_v(bus, channel_desc, data, capacity);
		if (retval >= 0) {
			break;
		}
		else if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			wakeup_queue_suspend_this(&bus->channels[channel_desc]->recv_queue);
		}
		else {
			return -1;
		}
	}
	
	if (!bus->channels[channel_desc]->data.empty()) {
        wakeup_queue_wakeup_first(&bus->channels[channel_desc]->recv_queue);
    }

	return retval;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel_desc, unsigned *data, unsigned capacity)
{
	if (bus->channels == NULL || bus->channels[channel_desc] == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (bus->channels[channel_desc]->data.empty()) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned recv_count = capacity;
	if (capacity > bus->channels[channel_desc]->data.size()) {
		recv_count = bus->channels[channel_desc]->data.size();
	}

	for (unsigned i = 0; i < recv_count; i++) {
		data[i] = bus->channels[channel_desc]->data.front();
		bus->channels[channel_desc]->data.erase(bus->channels[channel_desc]->data.begin());
	}

	wakeup_queue_wakeup_first(&bus->channels[channel_desc]->send_queue);

	return recv_count;
}

#endif
