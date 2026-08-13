#include "local_serial.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

struct SendArgs {
	NetplayLocalSerialEndpoint* endpoint;
	unsigned char sent;
	unsigned char received;
};

static void* send_byte(void* opaque) {
	SendArgs* args = static_cast<SendArgs*>(opaque);
	args->received = args->endpoint->send(args->sent, true);
	return 0;
}

int main() {
	NetplayLocalSerialBus bus;
	NetplayLocalSerialEndpoint a(bus, 0);
	NetplayLocalSerialEndpoint b(bus, 1);

	SendArgs args = { &a, 0x42, 0 };
	pthread_t thread;
	assert(pthread_create(&thread, 0, send_byte, &args) == 0);

	unsigned char incoming = 0;
	bool fast = false;
	bool received = false;
	for (unsigned i = 0; i < 100 && !received; ++i) {
		received = b.check(0x99, incoming, fast);
		if (!received) usleep(100);
	}
	assert(received);
	assert(incoming == 0x42);
	assert(fast);
	assert(pthread_join(thread, 0) == 0);
	assert(args.received == 0x99);
	NetplayLocalSerialStats stats;
	bus.snapshot(stats);
	assert(stats.exchanges == 1);
	assert(stats.send_timeouts == 0);
	assert(stats.simultaneous_sends == 0);

	/* A poll with no owner is bounded and non-fatal. */
	assert(!b.check(0x11, incoming, fast));
	bus.snapshot(stats);
	assert(stats.poll_timeouts >= 1);

	bus.reset();
	bus.snapshot(stats);
	assert(stats.exchanges == 0);
	assert(stats.send_timeouts == 0);
	assert(stats.poll_timeouts == 0);
	assert(stats.simultaneous_sends == 0);

	/* If both identical instances assert internal clock together, the second
	 * send completes the first instead of leaving both blocked in send(). */
	SendArgs left = { &a, 0x12, 0 };
	SendArgs right = { &b, 0x34, 0 };
	pthread_t left_thread, right_thread;
	assert(pthread_create(&left_thread, 0, send_byte, &left) == 0);
	usleep(1000);
	assert(pthread_create(&right_thread, 0, send_byte, &right) == 0);
	assert(pthread_join(left_thread, 0) == 0);
	assert(pthread_join(right_thread, 0) == 0);
	assert(left.received == 0x34);
	assert(right.received == 0x12);
	bus.snapshot(stats);
	assert(stats.exchanges == 1);
	assert(stats.simultaneous_sends == 1);
	assert(stats.send_timeouts == 0);

	/* Active-state tracking must not turn the hot check() path into a wait. */
	bus.reset();
	bus.setActive(0, true);
	SendArgs rendezvous = { &a, 0x56, 0 };
	assert(pthread_create(&thread, 0, send_byte, &rendezvous) == 0);
	received = false;
	for (unsigned i = 0; i < 100 && !received; ++i) {
		received = b.check(0x78, incoming, fast);
		if (!received) usleep(100);
	}
	assert(received);
	assert(pthread_join(thread, 0) == 0);
	bus.setActive(0, false);
	assert(incoming == 0x56);
	assert(rendezvous.received == 0x78);

	/* A core at its frame boundary can observe a late request and run a small
	 * service slice instead of making the sender time out. */
	bus.reset();
	bus.setActive(0, true);
	SendArgs boundary = { &a, 0x9A, 0 };
	assert(pthread_create(&thread, 0, send_byte, &boundary) == 0);
	assert(bus.waitForService(1, 2000));
	assert(b.check(0xBC, incoming, fast));
	assert(pthread_join(thread, 0) == 0);
	bus.setActive(0, false);
	assert(incoming == 0x9A);
	assert(boundary.received == 0xBC);

	puts("local serial coordinator: ok");
	return 0;
}
