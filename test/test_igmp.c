/*
 * test_igmp.c - the IGMP snooping handler against the simulated table engine.
 *
 * IGMP frames go in the way the switch traps them to the CPU: destination
 * and source MAC, the 8-byte CPU tag carrying the ingress port, then the IPv4
 * packet. What comes out is checked only from the outside: which ports the
 * table holds for a group afterwards, and the frame the handler leaves to
 * send (its CPU tag and to which ports). The module is compiled unmodified.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_igmp.h"
#include "rtl837x_port.h"
#include "uip.h"
#include <test/support.h>
#include <test/hw_mock.h>

/* ---- what the handler links against besides the table environment ---- */
uint8_t  uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
uint8_t  igmpEnabled;
uint8_t  cpuPort = CPU_PORT;
void print_sfr_data(void) { }

/* ---- trapped frames, laid out byte by byte ---- */

#define IGMP_V3_ALLOW_NEW	5
#define IP_OPT_ROUTER_ALERT	0x94
#define IP_HDR			22	/* MACs, CPU tag, EtherType */

static const uint8_t grp_a[4]    = { 239, 255, 255, 250 };	/* SSDP */
static const uint8_t grp_b[4]    = { 239, 1, 2, 3 };
static const uint8_t grp_c[4]    = { 239, 9, 9, 9 };
static const uint8_t grp_d[4]    = { 239, 8, 8, 8 };
static const uint8_t grp_mdns[4] = { 224, 0, 0, 251 };
static const uint8_t all_hosts[4] = { 224, 0, 0, 1 };
static const uint8_t v3_routers[4] = { 224, 0, 0, 22 };
static const uint8_t v2_routers[4] = { 224, 0, 0, 2 };

/* Puts the frame into uip_buf as the switch traps it on `port`: Ethernet
 * header, CPU tag (the ingress port in the low four bits of its last word),
 * an IPv4 header to `dst` with the Router Alert option unless `plain`, and
 * the IGMP message. Checksums stay 0; the handler does not look at them. */
static void trap(uint8_t port, const uint8_t dst[4], bool plain, const uint8_t *msg, uint16_t msg_len)
{
	uint8_t *p = uip_buf;
	uint8_t ihl = plain ? 5 : 6;
	uint16_t ip_len = ihl * 4 + msg_len;

	memset(uip_buf, 0xee, sizeof(uip_buf));
	*p++ = 0x01; *p++ = 0x00; *p++ = 0x5e;
	*p++ = dst[1] & 0x7f; *p++ = dst[2]; *p++ = dst[3];
	*p++ = 0x02; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x42;
	*p++ = 0x88; *p++ = 0x99;		/* CPU tag */
	*p++ = 0x04; *p++ = 0x00;
	*p++ = 0x00; *p++ = 0x00;
	*p++ = 0x00; *p++ = port;
	*p++ = 0x08; *p++ = 0x00;		/* IPv4 */
	*p++ = 0x40 | ihl; *p++ = 0xc0;
	*p++ = ip_len >> 8; *p++ = ip_len;
	*p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
	*p++ = 1; *p++ = 2;			/* TTL 1, protocol IGMP */
	*p++ = 0x00; *p++ = 0x00;
	*p++ = 192; *p++ = 168; *p++ = 10; *p++ = 100 + port;
	memcpy(p, dst, 4); p += 4;
	if (!plain) {
		*p++ = IP_OPT_ROUTER_ALERT; *p++ = 4; *p++ = 0; *p++ = 0;
	}
	memcpy(p, msg, msg_len); p += msg_len;
	uip_len = p - uip_buf;
}

/* An IGMPv1/v2 report or leave, or a query: type, max response time, checksum, group */
static void trap_igmp(uint8_t port, uint8_t type, const uint8_t group[4], const uint8_t dst[4], bool plain)
{
	uint8_t m[8] = { type, 0, 0, 0 };
	memcpy(m + 4, group, 4);
	trap(port, dst, plain, m, sizeof(m));
}

/* IGMPv3 reports, built up record by record: v3_report() starts one that
 * announces `records` records, v3_record() adds one, trap_v3_report() traps it */
static uint8_t  v3_msg[UIP_CONF_BUFFER_ALLOC_SIZE];
static uint16_t v3_len;

static void v3_report(uint16_t records)
{
	memset(v3_msg, 0, sizeof(v3_msg));
	v3_msg[0] = IGMP_V3_REPORT;
	v3_msg[6] = records >> 8; v3_msg[7] = records;
	v3_len = 8;
}

/* A group record with `nsrc` sources and `aux` words of auxiliary data. These
 * words take turns at looking like a record header for MODE_IS_EXCLUDE and
 * like grp_b, so a handler that takes them for a record learns grp_b. */
static void v3_record(uint8_t rtype, const uint8_t group[4], uint16_t nsrc, uint8_t aux)
{
	static const uint8_t fill[8] = { IGMP_V3_IS_EXCLUDE, 0, 0, 0, 239, 1, 2, 3 };
	uint8_t *r = v3_msg + v3_len;
	r[0] = rtype; r[1] = aux; r[2] = nsrc >> 8; r[3] = nsrc;
	memcpy(r + 4, group, 4);
	v3_len += 8;
	for (uint16_t i = 0; i < nsrc + aux; i++, v3_len += 4)
		memcpy(v3_msg + v3_len, fill + (i & 1) * 4, 4);
}

static void trap_v3_report(uint8_t port)
{
	trap(port, v3_routers, false, v3_msg, v3_len);
}

/* An IGMPv3 report with one group record and no sources */
static void trap_v3(uint8_t port, uint8_t rtype, const uint8_t group[4])
{
	v3_report(1);
	v3_record(rtype, group, 0, 0);
	trap_v3_report(port);
}

/* ---- running the handler ---- */

static uint8_t  handled;
static uint8_t  in_frame[UIP_CONF_BUFFER_ALLOC_SIZE];
static uint16_t in_len;

static void handle(void)
{
	memcpy(in_frame, uip_buf, sizeof(in_frame));
	in_len = uip_len;
	handled = igmp_packet_handler();
}

/* The ports the handler sends the trapped frame on to, 0 when it leaves
 * nothing to send. A frame to send sits behind the TX descriptor: the
 * trapped frame unchanged except for its CPU tag, which now names the
 * destination ports and disables learning, so the switch does not learn the
 * host's MAC on the CPU port. */
#define OUT (uip_buf + RTL_FRAME_DESC_SIZE)
static uint16_t sent_to(void)
{
	if (!uip_len)
		return 0;
	CHECK(uip_len == in_len, "the frame keeps its length");
	CHECK(!memcmp(OUT, in_frame, 12), "and its MAC addresses");
	CHECK(OUT[12] == 0x88 && OUT[13] == 0x99 && OUT[14] == 0x04 && OUT[15] == 0x00,
	      "it carries a CPU tag, version 4");
	CHECK(OUT[16] == 0x00 && OUT[17] == 0x20, "with learning disabled");
	CHECK(!memcmp(OUT + 20, in_frame + 20, in_len - 20), "the packet behind the tag is unchanged");
	return (uint16_t)OUT[18] << 8 | OUT[19];
}

/* Ports the table holds for a group, 0 when it has no entry */
static uint16_t members(const uint8_t group[4])
{
	const struct hw_l2 *e = hw_ipmc_find(group);
	return e ? hw_ipmc_pmask(e) : 0;
}

#define P(n) ((uint16_t)1 << (n))

/* VLAN 1: ports 0-5 and 8, port 7 tagged; VLAN 20: port 6, port 7 tagged
 * (a trunk to an access point). PVIDs as the ports' untagged VLANs. */
static const uint16_t vlan1 = P(0) | P(1) | P(2) | P(3) | P(4) | P(5) | P(7) | P(8);
static const uint16_t vlan20 = P(6) | P(7);

static void vlan(uint16_t vid, uint16_t ports, uint16_t tagged)
{
	vlan_settings.vlan = vid;
	vlan_settings.members = ports;
	vlan_settings.tagged = tagged;
	vlan_create();
	for (uint8_t i = 0; i < 9; i++)
		if (ports & ~tagged & P(i))
			port_pvid_set(i, vid);
}

static void reset(void)
{
	hw_reset(); out_reset();
	vlan(1, vlan1, P(7));
	vlan(20, vlan20, P(7));
	igmp_setup();
	igmp_enable();
}

/* A general query from the querier on `port` */
static void query_on(uint8_t port)
{
	trap_igmp(port, IGMP_QUERY, (const uint8_t[4]){ 0, 0, 0, 0 }, all_hosts, false);
	handle();
}

/* ---- the cases ---- */

static void t_v2_join_leave(void)
{
	printf("[test] IGMPv2 report and leave\n");
	reset();
	trap_igmp(3, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(handled == 1, "the handler takes the report");
	CHECK(members(grp_a) == P(3), "the group holds the ingress port");
	CHECK(hw_l2_count() == 1, "in one entry");

	trap_igmp(3, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(handled == 1, "the leave is taken");
	CHECK(hw_ipmc_find(grp_a) == 0, "the last leave deletes the entry");
	CHECK(hw_l2_count() == 0, "the table is empty again");
}

static void t_v1_join(void)
{
	printf("[test] IGMPv1 report, IP header without options\n");
	reset();
	trap_igmp(1, IGMP_V1_REPORT, grp_b, grp_b, true);
	handle();
	CHECK(handled == 1, "taken");
	CHECK(members(grp_b) == P(1), "the group holds the ingress port");
}

static void t_second_port(void)
{
	printf("[test] a second port joins, then the first leaves\n");
	reset();
	trap_igmp(1, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	trap_igmp(6, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(members(grp_a) == (P(1) | P(6)), "both ports, one in each half of the mask");
	CHECK(hw_l2_count() == 1, "still one entry");

	trap_igmp(6, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(members(grp_a) == (P(1) | P(6)), "a repeated report changes nothing");

	trap_igmp(1, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(members(grp_a) == P(6), "the leave takes only its own port out");

	trap_igmp(6, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(hw_ipmc_find(grp_a) == 0, "the last leave deletes the entry");
}

static void t_groups_apart(void)
{
	printf("[test] two groups are kept apart\n");
	reset();
	trap_igmp(2, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	trap_igmp(4, IGMP_V2_REPORT, grp_b, grp_b, false);
	handle();
	CHECK(members(grp_a) == P(2) && members(grp_b) == P(4), "each group holds its own port");
	trap_igmp(2, IGMP_V2_LEAVE, grp_b, v2_routers, false);
	handle();
	CHECK(members(grp_b) == P(4), "a leave from a port not in the group changes nothing");
	trap_igmp(2, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(hw_ipmc_find(grp_a) == 0 && members(grp_b) == P(4), "leaving one group leaves the other alone");
}

static void t_leave_unknown(void)
{
	printf("[test] a leave for a group the table does not hold\n");
	reset();
	trap_igmp(5, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(handled == 1, "taken");
	CHECK(hw_l2_count() == 0, "no entry is made");
}

static void t_v3(void)
{
	printf("[test] IGMPv3 change-to-exclude and change-to-include\n");
	reset();
	trap_v3(4, IGMP_V3_TO_EXCLUDE, grp_a);
	handle();
	CHECK(handled == 1, "taken");
	CHECK(members(grp_a) == P(4), "CHANGE_TO_EXCLUDE puts the ingress port in");
	trap_v3(7, IGMP_V3_TO_EXCLUDE, grp_a);
	handle();
	CHECK(members(grp_a) == (P(4) | P(7)), "a second port comes in");
	trap_v3(4, IGMP_V3_TO_INCLUDE, grp_a);
	handle();
	CHECK(members(grp_a) == P(7), "CHANGE_TO_INCLUDE takes it out");
	trap_v3(7, IGMP_V3_TO_INCLUDE, grp_a);
	handle();
	CHECK(hw_ipmc_find(grp_a) == 0, "the last one deletes the entry");

	trap_v3(4, IGMP_V3_ALLOW_NEW, grp_b);
	handle();
	CHECK(handled == 1 && hw_l2_count() == 0, "other record types are taken and ignored");
}

static void t_v3_records(void)
{
	printf("[test] an IGMPv3 answer to a query: several records, mDNS first\n");
	reset();
	v3_report(3);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_mdns, 0, 0);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 0, 0);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_c, 0, 0);
	trap_v3_report(3);
	handle();
	CHECK(handled == 1, "taken");
	CHECK(members(grp_a) == P(3), "MODE_IS_EXCLUDE in the second record puts the ingress port in");
	CHECK(members(grp_c) == P(3), "the third record too");
	CHECK(hw_l2_count() == 2, "224.0.0.251 stays out");
	CHECK(sent_to() == (vlan1 & ~P(3)), "the report is sent on once, unchanged");

	v3_report(2);
	v3_record(IGMP_V3_TO_EXCLUDE, grp_mdns, 0, 0);
	v3_record(IGMP_V3_TO_INCLUDE, grp_a, 0, 0);
	trap_v3_report(3);
	handle();
	CHECK(hw_ipmc_find(grp_a) == 0, "CHANGE_TO_INCLUDE in a later record takes it out");
	CHECK(members(grp_c) == P(3), "and leaves the other group alone");
}

static void t_v3_sources(void)
{
	printf("[test] IGMPv3 records with sources and auxiliary data are passed over whole\n");
	reset();
	v3_report(4);
	v3_record(IGMP_V3_ALLOW_NEW, grp_mdns, 2, 1);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 3, 0);
	v3_record(IGMP_V3_TO_INCLUDE, grp_c, 0, 2);
	v3_record(IGMP_V3_TO_EXCLUDE, grp_d, 1, 1);
	trap_v3_report(5);
	handle();
	CHECK(members(grp_a) == P(5), "the record behind sources and aux data is read");
	CHECK(members(grp_d) == P(5), "the last one too");
	CHECK(hw_ipmc_find(grp_b) == 0, "no source or aux word is taken for a record");
	CHECK(hw_l2_count() == 2, "nothing else is learned");
	CHECK(sent_to() == (vlan1 & ~P(5)), "the report is sent on unchanged");

	reset();
	v3_report(2);
	v3_record(IGMP_V3_ALLOW_NEW, grp_mdns, 60, 0);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 0, 0);
	trap_v3_report(5);	/* the second record starts at byte 302 of the frame */
	handle();
	CHECK(members(grp_a) == P(5) && hw_l2_count() == 1, "a record beyond byte 255 of the frame is learned");
}

static void t_v3_truncated(void)
{
	printf("[test] an IGMPv3 report whose records run past the end of the packet\n");
	reset();
	v3_report(2);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_mdns, 0, 0);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 2, 0);
	trap_v3_report(2);
	uip_len -= 4;
	handle();
	CHECK(handled == 1, "taken");
	CHECK(hw_l2_count() == 0, "a record whose sources are cut off makes no entry");
	CHECK(sent_to() == (vlan1 & ~P(2)), "the report is still sent on");

	reset();
	v3_report(2);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 0, 0);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_d, 0, 0);
	trap_v3_report(2);
	uip_len -= 1;
	handle();
	CHECK(members(grp_a) == P(2), "the records before the cut are learned");
	CHECK(hw_l2_count() == 1, "a record missing the last byte of its header is not");

	reset();
	v3_report(1);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 1, 2);
	trap_v3_report(2);
	uip_len -= 4;
	handle();
	CHECK(hw_l2_count() == 0, "a record whose aux data is cut off makes no entry");

	/* A report that announces two records but carries one; the frame is
	 * padded behind the IP packet with bytes that look like the second. */
	reset();
	v3_report(2);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 0, 0);
	trap_v3_report(2);
	memcpy(uip_buf + uip_len, (const uint8_t[8]){ IGMP_V3_IS_EXCLUDE, 0, 0, 0, 239, 9, 9, 9 }, 8);
	uip_len += 10;
	handle();
	CHECK(members(grp_a) == P(2), "the record within the IP packet is learned");
	CHECK(hw_ipmc_find(grp_c) == 0, "the Ethernet padding behind it is not read as a record");

	/* The first record claims 375 sources, which would end 2 bytes behind
	 * uip_buf (Ethernet and CPU tag, IP header with Router Alert, report
	 * header, record header, sources); the bytes after its header look like
	 * a record for grp_a. */
	/* 65535 sources and one aux word would wrap a 16-bit count to 0 */
	reset();
	v3_report(2);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_c, 0, 1);
	v3_msg[8 + 2] = 0xff; v3_msg[8 + 3] = 0xff;
	trap_v3_report(2);
	handle();
	CHECK(hw_l2_count() == 0, "a record with 65535 sources ends the walk");

	_Static_assert(IP_HDR + 24 + 8 + 8 + 375 * 4 == sizeof(uip_buf) + 2, "375 sources end behind uip_buf");
	reset();
	v3_report(2);
	v3_record(IGMP_V3_ALLOW_NEW, grp_mdns, 0, 0);
	v3_record(IGMP_V3_IS_EXCLUDE, grp_a, 0, 0);
	v3_msg[8 + 2] = 375 >> 8; v3_msg[8 + 3] = 375 & 0xff;
	trap_v3_report(2);
	handle();
	CHECK(handled == 1 && hw_l2_count() == 0, "a record count of sources beyond the packet ends the walk");
}

static void t_local_net(void)
{
	printf("[test] groups in 224.0.0.0/24 stay out of the table\n");
	reset();
	trap_igmp(3, IGMP_V2_REPORT, grp_mdns, grp_mdns, false);
	handle();
	CHECK(handled == 1, "a v2 report for 224.0.0.251 is taken");
	CHECK(hw_l2_count() == 0, "and makes no entry");
	trap_v3(3, IGMP_V3_TO_EXCLUDE, grp_mdns);
	handle();
	CHECK(hw_l2_count() == 0, "a v3 record for it neither");
}

static void t_query(void)
{
	printf("[test] a query\n");
	reset();
	trap_igmp(2, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	query_on(8);
	CHECK(handled == 1, "the handler takes it");
	CHECK(members(grp_a) == P(2) && hw_l2_count() == 1, "the table is unchanged");
}

static void t_not_igmp(void)
{
	printf("[test] multicast that is not IGMP\n");
	reset();
	uint8_t udp[8] = { 0x07, 0x6c, 0x07, 0x6c, 0x00, 0x08, 0x00, 0x00 };
	trap(3, grp_a, true, udp, sizeof(udp));
	uip_buf[IP_HDR + 9] = 17;	/* protocol UDP */
	uint16_t len = uip_len;
	handle();
	CHECK(handled == 0, "a UDP datagram to a group is left to the caller");
	CHECK(uip_len == len, "untouched");
	CHECK(hw_l2_count() == 0, "and makes no entry");
}

static void t_existing_entry(void)
{
	printf("[test] a group the table already holds\n");
	reset();
	hw_ipmc_put(0x123, grp_b, P(3) | P(5));
	trap_igmp(8, IGMP_V2_REPORT, grp_b, grp_b, false);
	handle();
	CHECK(members(grp_b) == (P(3) | P(5) | P(8)), "a report adds its port, the top bit of the mask included");
	CHECK(hw_l2_count() == 1, "to the same entry");
	trap_igmp(3, IGMP_V2_LEAVE, grp_b, v2_routers, false);
	handle();
	CHECK(members(grp_b) == (P(5) | P(8)), "a leave takes its port out and keeps the others");
}

static void t_query_forward(void)
{
	printf("[test] a query goes to every port of its VLAN\n");
	reset();
	query_on(8);
	CHECK(sent_to() == (vlan1 & ~P(8)), "to all members of VLAN 1 but the ingress port, the CPU port neither");
}

static void t_report_forward(void)
{
	printf("[test] reports and leaves go to the querier once it is known\n");
	reset();
	trap_igmp(2, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(sent_to() == (vlan1 & ~P(2)), "before any query a report goes to every other port of the VLAN");
	trap_igmp(2, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(sent_to() == (vlan1 & ~P(2)), "a leave too");

	query_on(8);
	trap_igmp(2, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(sent_to() == P(8), "after a query on port 8 a report goes there only");
	CHECK(members(grp_a) == P(2), "and is still learned");
	trap_v3(3, IGMP_V3_TO_EXCLUDE, grp_b);
	handle();
	CHECK(sent_to() == P(8), "a v3 report too");
	trap_igmp(2, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(sent_to() == P(8), "a leave too");
	CHECK(hw_ipmc_find(grp_a) == 0, "and is still learned");

	trap_igmp(8, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(handled == 1 && sent_to() == 0, "a report from the querier's port is not sent to the hosts");

	query_on(1);
	trap_igmp(2, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(sent_to() == (P(1) | P(8)), "a query on a second port adds it, the first one stays");
	query_on(1);
	CHECK(sent_to() == (vlan1 & ~P(1)), "a query from a router port still goes to all others");
}

static void t_vlans_apart(void)
{
	printf("[test] IGMP stays in the VLAN of its ingress port\n");
	reset();
	query_on(8);
	CHECK(!(sent_to() & P(6)), "a query in VLAN 1 does not reach port 6 (VLAN 20)");
	trap_igmp(6, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(sent_to() == P(7), "a report in VLAN 20 goes to VLAN 20's ports, not to VLAN 1's router port");
	query_on(6);
	CHECK(sent_to() == P(7), "a query in VLAN 20 goes to VLAN 20's ports only");
	trap_igmp(3, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(sent_to() == P(8), "a report in VLAN 1 goes to VLAN 1's router port, not to VLAN 20's");
}

static void t_long_frame(void)
{
	printf("[test] a frame too long to get the TX descriptor in front\n");
	uint8_t big[UIP_CONF_BUFFER_SIZE - RTL_FRAME_DESC_SIZE - 46] = { IGMP_V2_REPORT, 0, 0, 0, 239, 1, 2, 3 };
	reset();
	trap(2, grp_b, false, big, sizeof(big));
	CHECK(uip_len == UIP_CONF_BUFFER_SIZE - RTL_FRAME_DESC_SIZE, "the longest frame that still fits");
	handle();
	CHECK(sent_to() == (vlan1 & ~P(2)), "is sent on");
	CHECK(members(grp_b) == P(2), "and learned");

	reset();
	trap(2, grp_b, false, big, sizeof(big));
	uip_len = UIP_CONF_BUFFER_SIZE;
	handle();
	CHECK(handled == 1 && uip_len == 0, "a frame of the full buffer size is not sent on");
	CHECK(members(grp_b) == P(2), "but learned");
}

int main(void)
{
	printf("== rtl837x_igmp.c handler tests ==\n");
	t_v2_join_leave();
	t_v1_join();
	t_second_port();
	t_groups_apart();
	t_leave_unknown();
	t_v3();
	t_v3_records();
	t_v3_sources();
	t_v3_truncated();
	t_local_net();
	t_query();
	t_not_igmp();
	t_existing_entry();
	t_query_forward();
	t_report_forward();
	t_vlans_apart();
	t_long_frame();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
