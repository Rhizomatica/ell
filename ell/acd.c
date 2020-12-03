/*
 *  Embedded Linux library
 *
 *  Copyright (C) 2020  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <sys/socket.h>
#include <netpacket/packet.h>
#include <netinet/if_ether.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

#include "private.h"
#include "acd.h"
#include "util.h"
#include "io.h"
#include "net.h"
#include "timeout.h"
#include "random.h"

/* IPv4 Address Conflict Detection (RFC 5227) */
#define PROBE_WAIT		1
#define PROBE_NUM		3
#define PROBE_MIN		1
#define PROBE_MAX		2

#define ANNOUNCE_WAIT		2
#define ANNOUNCE_NUM		2
#define ANNOUNCE_INTERVAL	2

#define DEFEND_INTERVAL		10

#define MAC "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_STR(a) a[0], a[1], a[2], a[3], a[4], a[5]

#define IP_STR(uint_ip) \
({ \
	struct in_addr _in; \
	char *_out; \
	_in.s_addr = uint_ip; \
	_out = inet_ntoa(_in); \
	_out; \
})

#define ACD_DEBUG(fmt, args...)					\
	l_util_debug(acd->debug_handler, acd->debug_data,		\
			"%s:%i " fmt, __func__, __LINE__, ## args)

enum acd_state {
	ACD_STATE_PROBE,
	ACD_STATE_ANNOUNCED,
	ACD_STATE_DEFEND,
};

struct l_acd {
	int ifindex;

	uint32_t ip;
	uint8_t mac[ETH_ALEN];

	enum acd_state state;

	struct l_io *io;
	struct l_timeout *timeout;
	unsigned int retries;

	l_acd_event_func_t event_func;
	l_acd_destroy_func_t destroy;
	void *user_data;

	l_acd_debug_cb_t debug_handler;
	l_acd_destroy_func_t debug_destroy;
	void *debug_data;
};

static int acd_open_socket(int ifindex)
{
	struct sockaddr_ll dest;
	int fd;

	fd = socket(PF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	memset(&dest, 0, sizeof(dest));

	dest.sll_family = AF_PACKET;
	dest.sll_protocol = htons(ETH_P_ARP);
	dest.sll_ifindex = ifindex;
	dest.sll_halen = ETH_ALEN;
	memset(dest.sll_addr, 0xFF, ETH_ALEN);

	if (bind(fd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		int err = errno;
		close(fd);
		return -err;
	}

	return fd;
}

static int acd_send_packet(struct l_acd *acd, uint32_t source_ip)
{
	struct sockaddr_ll dest;
	struct ether_arp p;
	uint32_t ip_source;
	uint32_t ip_target;
	int n;
	int fd = l_io_get_fd(acd->io);

	memset(&dest, 0, sizeof(dest));

	dest.sll_family = AF_PACKET;
	dest.sll_protocol = htons(ETH_P_ARP);
	dest.sll_ifindex = acd->ifindex;
	dest.sll_halen = ETH_ALEN;
	memset(dest.sll_addr, 0xFF, ETH_ALEN);

	ip_source = htonl(source_ip);
	ip_target = htonl(acd->ip);
	p.arp_hrd = htons(ARPHRD_ETHER);
	p.arp_pro = htons(ETHERTYPE_IP);
	p.arp_hln = ETH_ALEN;
	p.arp_pln = 4;
	p.arp_op = htons(ARPOP_REQUEST);

	ACD_DEBUG("sending packet with target IP %s", IP_STR(ip_target));

	memcpy(&p.arp_sha, acd->mac, ETH_ALEN);
	memcpy(&p.arp_spa, &ip_source, sizeof(p.arp_spa));
	memcpy(&p.arp_tpa, &ip_target, sizeof(p.arp_tpa));

	n = sendto(fd, &p, sizeof(p), 0,
			(struct sockaddr*) &dest, sizeof(dest));
	if (n < 0)
		n = -errno;

	return n;
}

static uint32_t acd_random_delay_ms(uint32_t max_sec)
{
	uint32_t rand;

	l_getrandom(&rand, sizeof(rand));

	return rand % (max_sec * 1000);
}

static void announce_wait_timeout(struct l_timeout *timeout, void *user_data)
{
	struct l_acd *acd = user_data;

	l_timeout_remove(acd->timeout);
	acd->timeout = NULL;

	if (acd->state == ACD_STATE_PROBE) {
		ACD_DEBUG("No conflicts found for %s, announcing address",
				IP_STR(htonl(acd->ip)));

		acd->state = ACD_STATE_ANNOUNCED;

		/*
		 * RFC 5227 - Section 2.3
		 *
		 * "The host may begin legitimately using the IP address
		 *  immediately after sending the first of the two ARP
		 *  Announcements"
		 */

		if (acd->event_func)
			acd->event_func(L_ACD_EVENT_AVAILABLE, acd->user_data);
	}

	if (acd->retries != ANNOUNCE_NUM) {
		acd->retries++;

		if (acd_send_packet(acd, acd->ip) < 0) {
			ACD_DEBUG("Failed to send ACD announcement");
			return;
		}

		acd->timeout = l_timeout_create(ANNOUNCE_INTERVAL,
						announce_wait_timeout,
						acd, NULL);

		return;
	}

	ACD_DEBUG("Done announcing");
}

static void probe_wait_timeout(struct l_timeout *timeout, void *user_data)
{
	struct l_acd *acd = user_data;
	uint32_t delay;

	ACD_DEBUG("Sending ACD Probe");

	l_timeout_remove(acd->timeout);
	acd->timeout = NULL;

	if (acd_send_packet(acd, 0) < 0) {
		ACD_DEBUG("Failed to send ACD probe");
		return;
	}

	acd->retries++;

	if (acd->retries < PROBE_NUM) {
		/*
		 * RFC 5227 - Section 2.1.1
		 *
		 * "... and should then send PROBE_NUM probe packets, each of
		 * these probe packets spaced randomly and uniformly, PROBE_MIN
		 * to PROBE_MAX seconds apart."
		 */
		delay = acd_random_delay_ms(PROBE_MAX - PROBE_MIN);
		delay += PROBE_MIN * 1000;
		acd->timeout = l_timeout_create_ms(delay, probe_wait_timeout,
							acd, NULL);
	} else {
		/*
		 * Wait for ANNOUNCE_WAIT seconds after probe period before
		 * announcing address.
		 */
		ACD_DEBUG("Done probing");

		acd->retries = 1;

		acd->timeout = l_timeout_create(ANNOUNCE_WAIT,
							announce_wait_timeout,
							acd, NULL);
	}
}

static void defend_wait_timeout(struct l_timeout *timeout, void *user_data)
{
	struct l_acd *acd = user_data;

	l_timeout_remove(acd->timeout);
	acd->timeout = NULL;

	/* Successfully defended address */
	acd->state = ACD_STATE_ANNOUNCED;
}

static bool acd_read_handler(struct l_io *io, void *user_data)
{
	struct l_acd *acd = user_data;
	struct ether_arp arp;
	ssize_t len;
	int source_conflict;
	int target_conflict;
	bool probe;
	uint32_t ip;

	memset(&arp, 0, sizeof(arp));
	len = read(l_io_get_fd(acd->io), &arp, sizeof(arp));
	if (len < 0)
		return false;

	if (len != sizeof(arp))
		return true;

	if (arp.arp_op != htons(ARPOP_REPLY) &&
			arp.arp_op != htons(ARPOP_REQUEST))
		return true;

	if (memcmp(arp.arp_sha, acd->mac, ETH_ALEN) == 0)
		return true;

	ip = htonl(acd->ip);
	source_conflict = !memcmp(arp.arp_spa, &ip, sizeof(uint32_t));
	probe = l_memeqzero(arp.arp_spa, sizeof(uint32_t));
	target_conflict = probe &&
		!memcmp(arp.arp_tpa, &ip, sizeof(uint32_t));

	if (!source_conflict && !target_conflict) {
		ACD_DEBUG("No target or source conflict detected for %s",
				IP_STR(ip));
		return true;
	}

	switch (acd->state) {
	case ACD_STATE_PROBE:
		/* No reason to continue probing */
		ACD_DEBUG("%s conflict detected for %s",
				target_conflict ? "Target" : "Source",
				IP_STR(ip));

		if (acd->event_func)
			acd->event_func(L_ACD_EVENT_CONFLICT, acd->user_data);

		l_acd_stop(acd);

		break;
	case ACD_STATE_ANNOUNCED:
		/* Only defend packets with a source conflict */
		if (!source_conflict)
			return true;

		/*
		 * RFC 5227 - Section 2.4 (b)
		 * [If the host] "has not seen any other conflicting ARP packets
		 * within the last DEFEND_INTERVAL seconds, MAY elect to attempt
		 * to defend its address by recording the time that the
		 * conflicting ARP packet was received, and then broadcasting
		 * one single ARP Announcement"
		 */
		acd->state = ACD_STATE_DEFEND;

		/*
		 * We still have an initial announcement to send, but rather
		 * than wait for that (potentially 2 seconds) we can remove
		 * the timeout, send annouce now, and still transition to the
		 * defending state.
		 */
		if (acd->timeout)
			l_timeout_remove(acd->timeout);

		acd_send_packet(acd, acd->ip);

		ACD_DEBUG("Defending address");

		acd->timeout = l_timeout_create(DEFEND_INTERVAL,
						defend_wait_timeout, acd, NULL);

		break;
	case ACD_STATE_DEFEND:
		if (!source_conflict)
			return true;

		l_timeout_remove(acd->timeout);
		acd->timeout = NULL;

		ACD_DEBUG("Lost address");
		/*
		* RFC 5227 Section 2.4(b)
		* "if this is not the first conflicting ARP packet the host has seen,
		* and the time recorded for the previous conflicting ARP packet is
		* recent, within DEFEND_INTERVAL seconds, then the host MUST
		* immediately cease using this address and signal an error to the
		* configuring agent"
		*/
		if (acd->event_func)
			acd->event_func(L_ACD_EVENT_LOST, acd->user_data);

		l_acd_stop(acd);

		break;
	}

	return true;
}

LIB_EXPORT struct l_acd *l_acd_new(int ifindex)
{
	struct l_acd *acd = l_new(struct l_acd, 1);

	acd->ifindex = ifindex;

	return acd;
}

LIB_EXPORT bool l_acd_start(struct l_acd *acd, const char *ip)
{
	struct in_addr ia;
	int fd;
	uint32_t delay;

	if (unlikely(!acd || !ip))
		return false;

	if (inet_pton(AF_INET, ip, &ia) != 1)
		return false;

	fd = acd_open_socket(acd->ifindex);
	if (fd < 0)
		return false;

	if (l_memeqzero(acd->mac, ETH_ALEN) &&
			!l_net_get_mac_address(acd->ifindex, acd->mac)) {
		close(fd);
		return false;
	}

	acd->io = l_io_new(fd);
	l_io_set_close_on_destroy(acd->io, true);
	l_io_set_read_handler(acd->io, acd_read_handler, acd, NULL);

	acd->ip = ntohl(ia.s_addr);
	acd->state = ACD_STATE_PROBE;

	delay = acd_random_delay_ms(PROBE_WAIT);

	ACD_DEBUG("Waiting %ums to send probe", delay);

	/*
	 * RFC 5227 - Section 2.1.1
	 * "When ready to begin probing, the host should then wait for a random
	 *  time interval selected uniformly in the range zero to PROBE_WAIT
	 *  seconds..."
	 */
	acd->timeout = l_timeout_create_ms(delay, probe_wait_timeout,
							acd, NULL);

	return true;
}

LIB_EXPORT bool l_acd_set_event_handler(struct l_acd *acd,
					l_acd_event_func_t cb,
					void *user_data,
					l_acd_destroy_func_t destroy)
{
	if (unlikely(!acd))
		return false;

	acd->event_func = cb;
	acd->destroy = destroy;
	acd->user_data = user_data;

	return true;
}

LIB_EXPORT bool l_acd_stop(struct l_acd *acd)
{
	if (unlikely(!acd))
		return false;

	if (acd->timeout) {
		l_timeout_remove(acd->timeout);
		acd->timeout = NULL;
	}

	if (acd->io) {
		l_io_destroy(acd->io);
		acd->io = NULL;
	}

	return true;
}

LIB_EXPORT void l_acd_destroy(struct l_acd *acd)
{
	if (unlikely(!acd))
		return;

	if (acd->destroy)
		acd->destroy(acd->user_data);

	l_free(acd);
}

LIB_EXPORT bool l_acd_set_debug(struct l_acd *acd,
				l_acd_debug_cb_t function,
				void *user_data, l_acd_destroy_func_t destory)
{
	if (unlikely(!acd))
		return false;

	acd->debug_handler = function;
	acd->debug_data = user_data;
	acd->debug_destroy = destory;

	return true;
}