#include "local_serial.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace {

static uint64_t now_us() {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static struct timespec deadline_ms(unsigned ms) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	return ts;
}

static struct timespec deadline_us(unsigned us) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += us / 1000000;
	ts.tv_nsec += (long)(us % 1000000) * 1000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	return ts;
}

}

NetplayLocalSerialBus::NetplayLocalSerialBus()
	: stopped_(false)
	, request_pending_(false)
	, response_pending_(false)
	, owner_(0)
	, request_data_(0xFF)
	, response_data_(0xFF)
	, request_fast_(false)
	, generation_(0)
	, exchanges_(0)
	, send_timeouts_(0)
	, poll_timeouts_(0)
	, simultaneous_sends_(0)
	, total_wait_us_(0)
	, longest_wait_us_(0) {
	pthread_mutex_init(&mutex_, 0);
	pthread_cond_init(&changed_, 0);
	active_[0] = active_[1] = false;
}

NetplayLocalSerialBus::~NetplayLocalSerialBus() {
	pthread_mutex_lock(&mutex_);
	stopped_ = true;
	pthread_cond_broadcast(&changed_);
	pthread_mutex_unlock(&mutex_);
	pthread_cond_destroy(&changed_);
	pthread_mutex_destroy(&mutex_);
}

void NetplayLocalSerialBus::reset() {
	pthread_mutex_lock(&mutex_);
	request_pending_ = false;
	response_pending_ = false;
	active_[0] = active_[1] = false;
	request_data_ = response_data_ = 0xFF;
	request_fast_ = false;
	++generation_;
	exchanges_ = send_timeouts_ = poll_timeouts_ = simultaneous_sends_ = 0;
	total_wait_us_ = 0;
	longest_wait_us_ = 0;
	pthread_cond_broadcast(&changed_);
	pthread_mutex_unlock(&mutex_);
}

void NetplayLocalSerialBus::setActive(unsigned endpoint, bool active) {
	if (endpoint > 1) return;
	pthread_mutex_lock(&mutex_);
	active_[endpoint] = active;
	pthread_cond_broadcast(&changed_);
	pthread_mutex_unlock(&mutex_);
}

bool NetplayLocalSerialBus::peerActive(unsigned endpoint) {
	if (endpoint > 1) return false;
	pthread_mutex_lock(&mutex_);
	const bool active = active_[endpoint ^ 1];
	pthread_mutex_unlock(&mutex_);
	return active;
}

bool NetplayLocalSerialBus::waitForService(unsigned endpoint, unsigned timeout_us) {
	if (endpoint > 1) return false;
	const struct timespec deadline = deadline_us(timeout_us);
	pthread_mutex_lock(&mutex_);
	while (!stopped_ && active_[endpoint ^ 1] &&
	       (!request_pending_ || owner_ == endpoint || response_pending_)) {
		if (pthread_cond_timedwait(&changed_, &mutex_, &deadline) == ETIMEDOUT)
			break;
	}
	const bool service = !stopped_ && request_pending_ &&
		owner_ != endpoint && !response_pending_;
	pthread_mutex_unlock(&mutex_);
	return service;
}

void NetplayLocalSerialBus::snapshot(NetplayLocalSerialStats& stats) {
	pthread_mutex_lock(&mutex_);
	stats.exchanges = exchanges_;
	stats.send_timeouts = send_timeouts_;
	stats.poll_timeouts = poll_timeouts_;
	stats.simultaneous_sends = simultaneous_sends_;
	stats.total_wait_us = total_wait_us_;
	stats.longest_wait_us = longest_wait_us_;
	pthread_mutex_unlock(&mutex_);
}

unsigned char NetplayLocalSerialBus::send(unsigned endpoint, unsigned char data, bool fastCgb) {
	const uint64_t started = now_us();
	const struct timespec deadline = deadline_ms(50);
	pthread_mutex_lock(&mutex_);

	/* Identical local instances can assert their internal clocks at the same
	 * emulated cycle. Treat the first claimant as clock owner and the second
	 * send as its peer's response. On a physical cable only one clock should
	 * win; this deterministic arbitration exchanges the same two data bytes
	 * without making both synchronous callers wait for a check() that cannot
	 * occur. */
	if (request_pending_ && owner_ != endpoint && !response_pending_) {
		const unsigned char incoming = request_data_;
		response_data_ = data;
		response_pending_ = true;
		++simultaneous_sends_;
		pthread_cond_broadcast(&changed_);
		pthread_mutex_unlock(&mutex_);
		return incoming;
	}

	while (!stopped_ && request_pending_) {
		if (pthread_cond_timedwait(&changed_, &mutex_, &deadline) == ETIMEDOUT) {
			++send_timeouts_;
			pthread_mutex_unlock(&mutex_);
			return 0xFF;
		}
	}
	if (stopped_) {
		pthread_mutex_unlock(&mutex_);
		return 0xFF;
	}

	owner_ = endpoint;
	request_data_ = data;
	request_fast_ = fastCgb;
	request_pending_ = true;
	response_pending_ = false;
	const uint64_t generation = generation_;
	pthread_cond_broadcast(&changed_);

	while (!stopped_ && generation == generation_ && !response_pending_) {
		if (pthread_cond_timedwait(&changed_, &mutex_, &deadline) == ETIMEDOUT) {
			request_pending_ = false;
			++send_timeouts_;
			pthread_cond_broadcast(&changed_);
			pthread_mutex_unlock(&mutex_);
			return 0xFF;
		}
	}

	unsigned char response = response_pending_ ? response_data_ : 0xFF;
	request_pending_ = false;
	response_pending_ = false;
	if (generation == generation_ && !stopped_) {
		uint64_t waited = now_us() - started;
		++exchanges_;
		total_wait_us_ += waited;
		if (waited > longest_wait_us_)
			longest_wait_us_ = waited > UINT32_MAX ? UINT32_MAX : (uint32_t)waited;
	}
	pthread_cond_broadcast(&changed_);
	pthread_mutex_unlock(&mutex_);
	return response;
}

bool NetplayLocalSerialBus::check(unsigned endpoint, unsigned char out,
		unsigned char& in, bool& fastCgb) {
	pthread_mutex_lock(&mutex_);
	/* The polling endpoint may check again before the sender has consumed the
	 * first response. Never let that later poll overwrite the paired byte. */
	if (response_pending_) {
		pthread_mutex_unlock(&mutex_);
		return false;
	}
	/* checkSerial() runs many times inside Gambatte's CPU loop. Keep it a true
	 * poll; scheduling waits belong at the wrapper's frame boundary. */
	if (stopped_ || !request_pending_ || owner_ == endpoint) {
		++poll_timeouts_;
		pthread_mutex_unlock(&mutex_);
		return false;
	}

	in = request_data_;
	fastCgb = request_fast_;
	response_data_ = out;
	response_pending_ = true;
	pthread_cond_broadcast(&changed_);
	pthread_mutex_unlock(&mutex_);
	return true;
}
