# IGMP (Internet Group Management Protocol) and MLD (Multicast Listener Discovery)
IGMP (for IPv4) and MLD (for IPv6) are protocols that control the distribution
of Layer-3 Multicast packets on the LAN, which otherwise would be flooded across the
entire network. For this to work, IGMP/MLD messages are sent, in particular
from MC consumers (e.g. the video-player that plays an IP-Multicast stream), but
also Multicast-aware routers to control switching of the IP-MC or underlying
L2-MC packets. The main usage in home networks is IPTV. 

The RTL8372/3 SoC supports managing IPv4-MC using either Destination-IP (the IPv4
multicast group address)/Source-IP (typically 0.0.0.0) matching or via controlling
the switching of the underlying L2-MC packets (i.e. packets in 01:00:5e:xx:yy:zz, where
xx:yy:zz are the LSBs of the IPv4-MC address). The DIP/SIP-based switching is
not VLAN-aware, meaning a stream will be available in all VLANs if subscribed to.
This is not a problem in a typical home network, however. The L2-based method
is VLAN aware, but currently not supported in the software.

Although there is hardware support for IPv6/MLD-based Multicast management (i.e. intelligent
management by the switch), the current software does not implement managing IPv6 Multicast.
Instead, all IPv6 Multicast pakets will be flooded to all ports, just as an unmanaged
switch would do.

The current software support works by trapping IGMP packets (IGMPv1, v2 and v3, the
latter used in the vast majority of today's networks) to the CPU of the switch which will
update the L3 and L2 switching tables to include switch ports in a stream or remove
them. This trapping to the CPU is also called IGMP snooping. While there is support
in the HW to handle IGMP/MLD packets (v3 has only limited support) entirely in hardware
and even send out reports, it is currently not understood
how this works, and instead IGMP is handled entirely in software, which also allows
to fully support IGMPv3 packet which are the standard in present-day networks.

## IP-MC control
The relevant registers for controlling IP-MC switching are:
```
#define RTL837X_IPV4_PORT_MC_LM_ACT	0x4f78
#define RTL837X_IPV6_PORT_MC_LM_ACT	0x4f7c
#define RTL837X_IGMP_PORT_CFG		0x52a0
#define IGMP_MAX_GROUP			0x00ff0000
#define IGMP_PROTOCOL_ENABLE		0x00007c00
#define IGMP_TRAP			0x0000002a
#define IGMP_FLOOD			0x00000015
#define IGMP_ASIC			0x00000000
#define RTL837X_IGMP_ROUTER_PORT	0x529c
#define RTL837X_IPV4_UNKN_MC_FLD_PMSK	0x5368
#define RTL837X_IPV6_UNKN_MC_FLD_PMSK	0x536c
#define RTL837X_IGMP_TRAP_CFG		0x50bc
#define IGMP_TRAP_PRIORITY		0x7
#define IGMP_CPU_PORT			0x00010000
```
`RTL837X_IPV4_PORT_MC_LM_ACT/RTL837X_IPV6_PORT_MC_LM_ACT` control the action when an
IP-MC packet is encountered at a switch port and there is no rule for forwarding in
the forwarding tables. The default action is to flood such Lookup-Miss packets to all
ports. This is the configuration without IGMP/MLD enabled.

When IGMP/MLD is turned on, the Lookup-Miss action will be changed to drop such packets
unless a rule is found in the forwarding tables, which will need to be configured by
IGMP packets.

Switching on IGMP also configures all ports via `RTL837X_IGMP_PORT_CFG` to trap all
incoming IGMP packets to the CPU. `RTL837X_IGMP_TRAP_CFG` then is used to configure
priority and CPU-Port of trapped IGMP/MLD packets. 

Configuration of the IP-MC-forwarding to the listening ports is done by managing the
forwarding tables of the switch, see [L2 learning](l2.md).


## IGMP API
The code currently provides the following functions:
```
void igmp_setup(void) __banked;
void igmp_enable(void) __banked;
void igmp_router_port_set(uint16_t pmask) __banked;
uint8_t igmp_packet_handler(void) __banked;
void igmp_show(void) __banked;
```c
`igmp_setup()` is called at boot-time and configures flooding of all IP-MC packets by
default, as otherwise no IP-MC would be possible in the network. `igmp off` calls it as
well; groups learned while IGMP was on stay in the table, but are no longer looked up.

`igmp_enable()`starts IGMP which cause IGMP packets to be handled by the CPU and forwarding
of IP-MC packets to be limited to only subscribed ports.

`igmp_router_port_set()` writes the chip's static router port register
(`RTL837X_IGMP_ROUTER_PORT`), meant for handling IGMP in the ASIC. Nothing calls it, and the
forwarding by `igmp_packet_handler()` does not use it (see Router ports below).

`igmp_packet_handler()` implements handling of trapped IGMP packets by the CPU. It is
called for frames to 01:00:5e:xx:xx:xx and returns 0 when the frame is not a trapped IGMP
packet, so that it is passed on to the IP stack. IGMPv1/v2 reports (0x12, 0x16) join the
group given in the IGMP header, IGMPv2 leaves (0x17) leave it. In IGMPv3 reports every
group record counts: MODE_IS_EXCLUDE (2, a host's answer to a query) and CHANGE_TO_EXCLUDE
(4) join, CHANGE_TO_INCLUDE (3) leaves, other record types are passed over. The records are
read up to the first one that does not end within the IP packet. Source lists are not
looked at: a join counts for all sources, and a host that joins with INCLUDE records only
(source-specific multicast) is not learned. Trapped IGMP frames arrive without a VLAN tag.
The packet's VLAN is the PVID of its ingress port. After learning, the handler sends the
packet on to the other members of that VLAN: a query to all of them, a report or leave to
the router ports among them, or to all of them while the VLAN has none (see Router ports
below). The CPU tag of the frame names these ports and disables learning, so the switch
does not learn the host's MAC address on the CPU port.
Reports for groups in 224.0.0.0/24 are ignored: the switch floods that block
even with IGMP on, and an entry for a group such as 224.0.0.251 (mDNS) would
otherwise limit it to the ports that reported it, as RFC 4541 warns against.

`igmp_show()` prints out the IGMP configuration on the CLI.


## Router ports
A port a query comes in on becomes a router port: a querier, normally the router, sits
behind it. The handler keeps the router ports as one port mask. Only queries add to it,
`igmp on` clears it, and it does not age. Reports and leaves go to the router ports of
their VLAN only, so the querier learns the members, and IGMPv1/v2 hosts do not see each
other's reports, which would make them hold back their own. A report or leave that comes
in on a router port goes to the other router ports of its VLAN only, so with one querier
nowhere. Other IGMP messages go like reports. While no query has come in on a port of the
VLAN, reports and leaves go to all its ports but the ingress port, so none is lost before
the querier is known.

Snooping depends on a querier in every VLAN with multicast listeners. Hosts report on
their own only when they join; after that, only a query makes them report again. After a
restart of the switch the group table is empty, and a host that stayed in a group gets the
group's traffic again once it has answered the next query (125 s apart by default).
Without a querier it does not get it until it joins anew.

## Differences from RFC 4541
- **No ageing.** Neither the ports of a group nor the router ports age. A port leaves a
  group through a leave, at once (fast leave), or when the switch restarts; `igmp off` and
  `igmp on` keep the groups. A host that goes away without a leave (unplugged, switched
  off, IGMPv1) keeps its port in the group. With several listeners behind one port (an
  access point or an unmanaged switch), the leave of one cuts the group off for the others
  until they answer the querier's group-specific query that follows the leave.
- **No multicast data to router ports.** RFC 4541 sends multicast data to the router
  ports as well, and data of groups without members at least there. Here data of a group
  goes to its member ports only, and data of a group without members is dropped
  (`LOOKUP_MISS_DROP`). A multicast router behind a router port therefore gets only the
  groups it joined itself. If one is needed: the lookup-miss action "to router port" (11,
  see `igmp_setup()`) together with `RTL837X_IGMP_ROUTER_PORT`, untested.

## When ageing is needed
Without ageing, a port that no longer wants a group still gets its traffic. That costs
little while the multicast in the network is small, such as SSDP or other service
discovery; 224.0.0.0/24 (mDNS) is flooded anyway. Ageing is needed as soon as a multicast
stream with bandwidth enters the network, such as IPTV or cameras. A sketch: two port
masks per group (reported in this round, reported in the last one), rounds clocked by
general queries, a port drops out after two rounds without a report (about 250 s with the
default query interval of 125 s), the router ports likewise; estimated 300-400 B code and
about 256 B XRAM.

## IGMP configuration on the Serial Console
For testing the following commands are provided on the serial console:
```
> igmp [on/off]
  Enables or disables IGMP

> igmp show
  Shows information on IGMP
```

## LAG configuration via the Web Interface
Not implemented, yet!

## A Test with IP-MC streaming using vlc
The following is a simple test verifying the IGMP and IP-MC switching capabilities.

You will need 2 Linux/Windows devices with a GUI plus a switch.

Connect the switch to an MC-aware router (e.g. to your home network). Connect the 2 Linux/Windows
devices to the switch. The connection to the router makes sure that Linux/Windows will send
out IGMP messages on the ports connected to the switch, which they will only do if they are aware
that there is a MC-aware router in the network. Make sure the 2 GUI devices are in the home network
(e.g. via DHCP).

Start streaming on one of the Linux/Windows machines:
```
$ vlc your_video.mp4 --sout="#std{access=udp, mux=ts, dst=239.255.0.1:8090}"
```
At this point you should see all switch ports flickering heavily as the MC stream is switched to all
switch ports, including flooding your home network. If you do not see any packets arriving at the switch,
you can force the output interface of vlc by using `--miface=<ifname>`

Enable IGMP on the switch-CLI:
```
> igmp on
```
The flickering should now stop on all ports except the port where the streaming device is connected:
the switch drops all IP-MC packets as there are no listeners.

Now, on the second Linux/Windows device start listening to the stream:
```
$ vlc udp://@239.255.0.1:8090
```
You should see the port-led of the port the displaying machine is connected to, to start flickering
and after some synchronization, the video should start playing.

Stopping vlc should also switching of the IP-MC frames to the listening device, i.e. the port-leds
should stop flickering.
