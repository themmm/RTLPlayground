/*
 * test_igmp.c - the IGMP snooping handler against the simulated table engine.
 *
 * IGMP frames go in the way the switch traps them to the CPU: destination
 * and source MAC, the 8-byte CPU tag carrying the ingress port, then the IPv4
 * packet. What comes out is checked only from the outside: which ports the
 * table holds for a group afterwards, and whether the handler leaves a frame
 * to send. The module is compiled unmodified.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_igmp.h"
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

#define IGMP_MEMBERSHIP_QUERY	0x11
#define IGMP_V3_IS_EXCLUDE	2
#define IGMP_V3_ALLOW_NEW	5
#define IP_OPT_ROUTER_ALERT	0x94
#define IP_HDR			22	/* MACs, CPU tag, EtherType */

static const uint8_t grp_a[4]    = { 239, 255, 255, 250 };	/* SSDP */
static const uint8_t grp_b[4]    = { 239, 1, 2, 3 };
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

/* An IGMPv3 report with one group record and no sources */
static void trap_v3(uint8_t port, uint8_t rtype, const uint8_t group[4])
{
	uint8_t m[16] = { IGMP_V3_REPORT, 0, 0, 0, 0, 0, 0, 1, rtype, 0, 0, 0 };
	memcpy(m + 12, group, 4);
	trap(port, v3_routers, false, m, sizeof(m));
}

/* ---- running the handler ---- */

static uint8_t handled;

static void handle(void)
{
	handled = igmp_packet_handler();
}

/* Ports the table holds for a group, 0 when it has no entry */
static uint16_t members(const uint8_t group[4])
{
	const struct hw_l2 *e = hw_ipmc_find(group);
	return e ? hw_ipmc_pmask(e) : 0;
}

#define P(n) ((uint16_t)1 << (n))

static void reset(void)
{
	hw_reset(); out_reset();
	igmpEnabled = 1;
}

/* ---- the cases ---- */

static void t_v2_join_leave(void)
{
	printf("[test] IGMPv2 report and leave\n");
	reset();
	trap_igmp(3, IGMP_V2_REPORT, grp_a, grp_a, false);
	handle();
	CHECK(handled == 1, "the handler takes the report");
	CHECK(uip_len == 0, "and sends nothing");
	CHECK(members(grp_a) == P(3), "the group holds the ingress port");
	CHECK(hw_l2_count() == 1, "in one entry");

	trap_igmp(3, IGMP_V2_LEAVE, grp_a, v2_routers, false);
	handle();
	CHECK(handled == 1 && uip_len == 0, "the leave is taken, nothing sent");
	CHECK(hw_ipmc_find(grp_a) == 0, "the last leave deletes the entry");
	CHECK(hw_l2_count() == 0, "the table is empty again");
}

static void t_v1_join(void)
{
	printf("[test] IGMPv1 report, IP header without options\n");
	reset();
	trap_igmp(1, IGMP_V1_REPORT, grp_b, grp_b, true);
	handle();
	CHECK(handled == 1 && uip_len == 0, "taken, nothing sent");
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
	CHECK(uip_len == 0, "nothing sent");

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
	CHECK(handled == 1 && uip_len == 0, "taken, nothing sent");
	CHECK(hw_l2_count() == 0, "no entry is made");
}

static void t_v3(void)
{
	printf("[test] IGMPv3 change-to-exclude and change-to-include\n");
	reset();
	trap_v3(4, IGMP_V3_TO_EXCLUDE, grp_a);
	handle();
	CHECK(handled == 1 && uip_len == 0, "taken, nothing sent");
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
	CHECK(handled == 1 && uip_len == 0 && hw_l2_count() == 0, "other record types are taken and ignored");
}

static void t_local_net(void)
{
	printf("[test] groups in 224.0.0.0/24 stay out of the table\n");
	reset();
	trap_igmp(3, IGMP_V2_REPORT, grp_mdns, grp_mdns, false);
	handle();
	CHECK(handled == 1 && uip_len == 0, "a v2 report for 224.0.0.251 is taken, nothing sent");
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
	trap_igmp(8, IGMP_MEMBERSHIP_QUERY, (const uint8_t[4]){ 0, 0, 0, 0 }, all_hosts, false);
	handle();
	CHECK(handled == 1, "the handler takes it");
	CHECK(uip_len == 0, "and sends nothing");
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

int main(void)
{
	printf("== rtl837x_igmp.c handler tests ==\n");
	t_v2_join_leave();
	t_v1_join();
	t_second_port();
	t_groups_apart();
	t_leave_unknown();
	t_v3();
	t_local_net();
	t_query();
	t_not_igmp();
	t_existing_entry();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
