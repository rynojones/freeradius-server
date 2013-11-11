/*
 * arp.c	ARP processing.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright (C) 2013 Network RADIUS SARL <info@networkradius.com>
 */

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/protocol.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/process.h>
#include <freeradius-devel/rad_assert.h>
#include <freeradius-devel/pcap.h>
#include <net/if_arp.h>

extern int check_config;

typedef struct arp_socket_t {
	char const     	*interface;
	fr_pcap_t	*pcap;
	uint64_t	counter;
	RADCLIENT	client;
} arp_socket_t;


/*
 *	ARP for ethernet && IPv4.
 */
struct arphdr_ether_ipv4 {
	uint16_t	ar_hrd;		/* format of hardware address */
	uint16_t	ar_pro;		/* format of protocol address */
	uint8_t		ar_hln;		/* length of hardware address */
	uint8_t		ar_pln;		/* length of protocol address */
	uint8_t		ar_op;		/* one of: */
	uint8_t		ar_sha[ETHER_ADDR_LEN];	/* sender hardware address */
	uint8_t		ar_spa[4];	/* sender protocol address */
	uint8_t		ar_tha[ETHER_ADDR_LEN];	/* target hardware address */
	uint8_t		ar_tpa[4];	/* target protocol address */
};

static int arp_process(REQUEST *request)
{
	size_t size;
	RADIUS_PACKET *packet = request->packet;
	struct ethernet_header const *ether;
	struct arphdr const *arp;
	uint8_t const *p;

	ether = (struct ethernet_header *) request->packet->data;
	arp = (struct arphdr *) (request->packet->data + sizeof(*ether));

	p = (const uint8_t *) arp;
	if (p > (packet->data + packet->data_len)) return 0;

	size = packet->data_len;
	size -= (p - packet->data);

#if 0
	{
		int i;

		for (i = 0; i < size; i++) {
			if ((i & 0x0f) == 0) printf("%04zx: ", i);
			printf("%02x ", p[i]);
			if ((i & 0x0f) == 0x0f) printf("\r\n");
			fflush(stdout);
		}
		printf("\n");
		fflush(stdout);
	}
#endif

	/*
	 *
	 */
	process_post_auth(0, request);

	return 1;
}


/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
 */
static int arp_socket_recv(rad_listen_t *listener)
{
	int rcode;
	arp_socket_t *sock = listener->data;
	pcap_t *handle = sock->pcap->handle;
	const uint8_t *data;
	struct pcap_pkthdr *header;
	struct ethernet_header const *ether;
	struct arphdr_ether_ipv4 const *arp;
	RADIUS_PACKET *packet;

	rcode = pcap_next_ex(handle, &header, &data);
	if (rcode == 0) return 0; /* no packet */

	if (rcode < 0) {
		ERROR("Error requesting next packet, got (%i): %s", rcode, pcap_geterr(handle));
		return 0;
	}

	/*
	 *	Silently ignore it.
	 */
	if (header->caplen < (sizeof(*ether) + sizeof(*arp))) {
		return 0;
	}

	ether = (struct ethernet_header const *) data;
	arp = (struct arphdr_ether_ipv4 const *) (data + sizeof(*ether));

	if (ntohs(arp->ar_hrd) != ARPHRD_ETHER) return 0;

	if (ntohs(arp->ar_pro) != 0x0800) return 0;

	if (arp->ar_hln != ETHER_ADDR_LEN) return 0; /* FIXME: malformed error */

	if (arp->ar_pln != 4) return 0; /* FIXME: malformed packet error */

	packet = talloc_zero(listener, RADIUS_PACKET);
	if (!packet) return 0;

	packet->dst_port = 1;	/* so it's not a "fake" request */
	packet->data_len = header->caplen;
	packet->data = talloc_memdup(packet, data, packet->data_len);

	DEBUG("ARP received on interface %s", sock->interface);

	if (!request_receive(listener, packet, &sock->client, arp_process)) {
		rad_free(&packet);
		return 0;
	}

	return 1;
}

static int arp_socket_send(UNUSED rad_listen_t *listener, UNUSED REQUEST *request)
{
	return 0;
}


static int arp_socket_encode(UNUSED rad_listen_t *listener, UNUSED REQUEST *request)
{
	return 0;
}


typedef struct arp_decode_t {
	char const	*name;
	size_t		len;
} arp_decode_t;

static const arp_decode_t header_names[] = {
	{ "ARP-Hardware-Format",		2 },
	{ "ARP-Protocol-Format",		2 },
	{ "ARP-Hardware-Address-Length",	1 },
	{ "ARP-Protocol-Address-Length",	1 },
	{ "ARP-Operation",			2 },
	{ "ARP-Sender-Hardware-Address",	6 },
	{ "ARP-Sender-Protocol-Address",	4 },
	{ "ARP-Target-Hardware-Address",	6 },
	{ "ARP-Target-Protocol-Address",	4 },

	{ NULL, 0 }
};

static int arp_socket_decode(UNUSED rad_listen_t *listener, UNUSED REQUEST *request)
{
	int i;
	struct ethernet_header const *ether;
	struct arphdr_ether_ipv4 const *arp;
	uint8_t const *p;

	ether = (struct ethernet_header const *) request->packet->data;
	arp = (struct arphdr_ether_ipv4 const *) (request->packet->data + sizeof(*ether));

	/*
	 *	arp_socket_recv() takes care of validating it's really
	 *	our kind of ARP.
	 */
	for (i = 0, p = (uint8_t const *) arp;
	     header_names[i].name != NULL;
	     p += header_names[i].len,
		     i++) {
		ssize_t len;
		DICT_ATTR const *da;
		VALUE_PAIR *vp;

		da = dict_attrbyname(header_names[i].name);
		if (!da) return 0;

		vp = NULL;
		len = data2vp(request->packet, NULL, NULL, da, p,
			      header_names[i].len, header_names[i].len,
			      &vp);
		if (len <= 0) {
			RDEBUG("Failed decoding %s: %s",
			       header_names[i].name, fr_strerror());
			return 0;
		}
		
		debug_pair(vp);
		pairadd(&request->packet->vps, vp);
	}

	return 0;
}


static void arp_socket_free(rad_listen_t *this)
{
	arp_socket_t *sock = this->data;

	talloc_free(sock);
	this->data = NULL;
}


static int arp_socket_parse(CONF_SECTION *cs, rad_listen_t *this)
{
	arp_socket_t *sock = this->data;
	char const *value;
	CONF_PAIR *cp;
	RADCLIENT *client;

	cp = cf_pair_find(cs, "interface");
	if (!cp) {
		cf_log_err_cs(cs, "'interface' is required for arp");
		return -1;
	}

	value = cf_pair_value(cp);
	if (!value) {
		cf_log_err_cs(cs,
			      "No interface name given");
		return -1;
	}
	sock->interface = value;

	sock->pcap = fr_pcap_init(cs, sock->interface, PCAP_INTERFACE_IN);
	if (!sock->pcap) {
		cf_log_err_cs(cs, "Failed creating pcap for interface %s",
			value);
		return -1;
	}

	if (check_config) return 0;

	fr_suid_up();
	if (fr_pcap_open(sock->pcap) < 0) {
		cf_log_err_cs(cs, "Failed opening interface %s: %s",
			      value, fr_strerror());
		return -1;
	}
	fr_suid_down();

	if (fr_pcap_apply_filter(sock->pcap, "arp") < 0) {
		cf_log_err_cs(cs, "Failed setting filter for interface %s: %s",
			      value, fr_strerror());
		return -1;
	}

	this->fd = sock->pcap->fd;
	this->nodup = true;	/* don't check for duplicates */

	/*
	 *	The server core is still RADIUS, and needs a client.
	 *	So we fake one here.
	 */
	client = &sock->client;
	memset(client, 0, sizeof(*client));
	client->ipaddr.af = AF_INET;
	client->ipaddr.ipaddr.ip4addr.s_addr = INADDR_NONE;
	client->prefix = 0;
	client->longname = client->shortname = sock->interface;
	client->secret = client->shortname;
	client->nas_type = talloc_strdup(sock, "none");

	return 0;
}

static int arp_socket_print(const rad_listen_t *this, char *buffer, size_t bufsize)
{
	arp_socket_t *sock = this->data;

	snprintf(buffer, bufsize, "arp interface %s", sock->interface);

	return 1;
}

fr_protocol_t proto_arp = {
	RLM_MODULE_INIT,
	"arp",
	sizeof(arp_socket_t),
	NULL,
	arp_socket_parse, arp_socket_free,
	arp_socket_recv, arp_socket_send,
	arp_socket_print, arp_socket_encode, arp_socket_decode
};