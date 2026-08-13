#ifndef NETPLAY_GAMBATTE_LOCAL_SERIAL_H
#define NETPLAY_GAMBATTE_LOCAL_SERIAL_H

#include "serial_io.h"
#include <pthread.h>
#include <stdint.h>

struct NetplayLocalSerialStats {
	uint64_t exchanges;
	uint64_t send_timeouts;
	uint64_t poll_timeouts;
	uint64_t simultaneous_sends;
	uint64_t total_wait_us;
	uint32_t longest_wait_us;
};

/* In-process two-console transport used by the experimental dual wrapper.
 *
 * Gambatte's clock owner calls send() synchronously while the externally
 * clocked console polls check(). The bus preserves that contract but exchanges
 * bytes directly. External-clock checks are strictly nonblocking and internal
 * sends have a bounded deadline, so a bad game/core state cannot hang the
 * frontend forever. It is deliberately transport-only; cycle-aware
 * yielding belongs to the next prototype if this one confirms that the
 * synchronous contract is still the bottleneck. */
class NetplayLocalSerialBus {
public:
	NetplayLocalSerialBus();
	~NetplayLocalSerialBus();

	void reset();
	void setActive(unsigned endpoint, bool active);
	bool peerActive(unsigned endpoint);
	bool waitForService(unsigned endpoint, unsigned timeout_us);
	unsigned char send(unsigned endpoint, unsigned char data, bool fastCgb);
	bool check(unsigned endpoint, unsigned char out, unsigned char& in, bool& fastCgb);
	void snapshot(NetplayLocalSerialStats& stats);

private:
	NetplayLocalSerialBus(const NetplayLocalSerialBus&);
	NetplayLocalSerialBus& operator=(const NetplayLocalSerialBus&);

	pthread_mutex_t mutex_;
	pthread_cond_t changed_;
	bool stopped_;
	bool request_pending_;
	bool response_pending_;
	bool active_[2];
	unsigned owner_;
	unsigned char request_data_;
	unsigned char response_data_;
	bool request_fast_;
	uint64_t generation_;
	uint64_t exchanges_;
	uint64_t send_timeouts_;
	uint64_t poll_timeouts_;
	uint64_t simultaneous_sends_;
	uint64_t total_wait_us_;
	uint32_t longest_wait_us_;
};

class NetplayLocalSerialEndpoint : public gambatte::SerialIO {
public:
	NetplayLocalSerialEndpoint(NetplayLocalSerialBus& bus, unsigned endpoint)
		: bus_(bus), endpoint_(endpoint) {}

	virtual bool check(unsigned char out, unsigned char& in, bool& fastCgb) {
		return bus_.check(endpoint_, out, in, fastCgb);
	}

	virtual unsigned char send(unsigned char data, bool fastCgb) {
		return bus_.send(endpoint_, data, fastCgb);
	}

private:
	NetplayLocalSerialBus& bus_;
	unsigned endpoint_;
};

#endif
