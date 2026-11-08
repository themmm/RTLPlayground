# Host unit-test harness

Compile and test individual firmware translation units on the build host with
**gcc + AddressSanitizer + UBSan** — no SDCC, no flashing, no hardware. This is
the fast inner loop for the logic-level bugs in `../../NOTES/08-findings.md`
(parser, line editor, HTTP header, DHCP): edit → `make` → see red/green in
seconds instead of a minutes-long flash-and-reboot cycle.

## Run
```
cd test
make            # build + run all tests (ASan/UBSan on)
make clean
```
Exit status is non-zero if any check fails, a sanitizer trips, or the watchdog
fires — so it drops straight into CI.

## How it works
- **`sdcc_shim.h`** (force-included) erases SDCC 8051 keywords (`__xdata`,
  `__code`, `__banked`, …) so firmware sources compile under host gcc. It changes
  no logic — the code under test is byte-for-byte the firmware source.
- **`hw_mock.c` / `hw_mock.h`** simulate the ASIC edge: a flat register file
  behind `reg_read`/`reg_write` and the `SFR_DATA_*` registers, with a table
  engine that performs a VLAN or L2 operation the moment `TBL_CTRL` is written,
  fetches a MIB counter on `STAT_GET` and drops entries on `L2_TBL_FLUSH_CTRL`.
  Nothing ever reports busy, so a polling loop runs once. The header states the
  VLAN, L2 and IP multicast entry layouts independently of the firmware, so a
  test can hold what `rtl837x_port.c` writes against what `page_impl.c` reads,
  and look up the ports `rtl837x_igmp.c` put into a group.
- **`env_tables.c`** carries the globals and leaf calls those two modules link
  against; **`stub/`** stands in for the two headers only a firmware build
  generates, so the harness needs no SDCC build first.
- **`support.c` / `support.h`** mock the hardware edges: the 16-byte serial ring
  (`sbuf`), the command/history buffers, and the character-output sink
  (`write_char` etc.). Buffers are sized **exactly** as on target, so ASan
  redzones catch the same off-by-one overflows the 8051 hits.
- Builds define **`RTLP_HOST_TEST`**, which hides the firmware's libc-named
  prototypes (`memset`/`strlen`/…) in `rtl837x_common.h` so they don't clash with
  glibc. Argument order matches libc, so on-host callers transparently use the C
  library. This guard is compiled out of normal firmware builds — zero on-target
  effect.
- A **SIGALRM watchdog** (`watchdog_arm`) turns an infinite-loop bug into a fast
  test failure instead of a hung runner.

## Current coverage
| Test binary | TU under test | Findings exercised |
|-------------|---------------|--------------------|
| `test_cmd_editor` | `cmd_editor.c` | **C4** — full-line hang + `cmd_buffer` 1-byte overflow; basic entry & backspace regressions |
| `test_port_tables` | `rtl837x_port.c` | VLAN entry layout and round trip, PVID register sharing, static multicast and management entries, per-port flush, trunk membership and hash seed |
| `test_page_json` | `httpd/page_impl.c` + `rtl837x_port.c` | `/vlan.json`, `/vlanlist`, `/l2.json` (walk, wrap marker, paging inside `outbuf`), 64-bit counters in `/status.json` and `/counters.json` |
| `test_httpd_tx` | `httpd/httpd.c` + `uip/uip.c` | a GET of a static file against a client that moves its receive window: ACK accounting, continuation chunks out of flash, retransmission |
| `test_lacp` | `rtl837x_lacp.c` | state machines against a simulated 802.3ad partner: convergence, mis-cabling, expiry, re-election, two groups, delivery by trap |
| `test_igmp` | `rtl837x_igmp.c` | trapped IGMP frames: v1/v2 reports and leaves, v3 reports with several group records (is-exclude and change-to-exclude join, change-to-include leaves, records behind sources and aux data, records cut off at the end of the packet, Ethernet padding behind it), a second port in a group, the last leave deleting the entry, 224.0.0.x kept out, queries and non-IGMP multicast; frames that came in tagged (802.1Q behind the CPU tag, priority-tagged, tagged non-IGMP multicast left alone); what is sent on: a query to the other ports of its VLAN (the tag's, else the PVID's), with an 802.1Q tag of that VLAN, a report or leave to the ports a query came in on (all others before), Learn-Disable set, nothing into another VLAN, a frame too long for the TX descriptor kept back; the table mock holds IP multicast entries by group |

## Adding a test for another module
1. Write `test_<module>.c` with `main()` driving the module's entry points and
   `CHECK(cond, msg)` assertions.
2. Add any missing mocked symbols to `support.c` (only what the linker asks for).
3. Add a target in the `Makefile` listing `test_<module>.c support.c ../<module>.c`.
4. If the module calls a firmware libc-name (`memcpy`/`strcpy`/…), it will use the
   C library on host (compatible arg order); provide a `fw_*`-style mock only if
   the semantics differ in a way the test cares about.

Good next targets (pure logic, high finding density): `cmd_parser.c` (C6/C8),
the HTTP header parser in `httpd/httpd.c` (S1), `dhcp.c` (S5).
