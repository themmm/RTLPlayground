# RTLPlayground for the keepLiNK KP-9000-9XHPML-X V1.2

This fork is [RTLPlayground](https://github.com/logicog/RTLPlayground) plus our commits for one
switch: the keepLiNK KP-9000-9XHPML-X V1.2 (stock web UI: `KP-9000-9XHPML-X-AC`), board
`2M-PCB51-ML-V1.2` with an RTL8238C PoE controller, machine `KEEPLINK_KP_9000_9XHPML_V1_2`
([device page](doc/devices/2M-PCB51-ML-V1.2.md)). Not to be confused with the KP-9000-9XHPML-X V3.1
(two RTL8238B), which keepLiNK sells under the same name: check the silkscreen.

- `keeplink` is upstream `main` with our commits on top, rebased onto upstream `main` on every
  sync. This is the branch to build.
- `main` mirrors upstream `main` and gets no commits of its own.

The other machines still build from `keeplink`, but our commits are tested on this switch only.

## What it adds to upstream

- **PoE**: the PoE infrastructure of upstream PR
  [#712](https://github.com/logicog/RTLPlayground/pull/712), copied as it is while the PR is not
  merged, and a driver for the RTL8238C (`poe/rtl8238c.c`). At boot the switch uploads the PSE
  image to the controller and turns all ports on; `poe show`, `poe port <n> on|off`,
  `poe global on|off` and `poe load` on the console.
- **IGMP snooping** ([doc/igmp.md](doc/igmp.md)): queries are forwarded to all ports of the VLAN,
  reports and leaves to the ports with a querier, as RFC 4541 recommends; every group record of an
  IGMPv3 report is learnt; IGMP with an 802.1Q tag is handled within its VLAN.
- **Less internal RAM**: small commits move locals and arguments out of the 8051's internal RAM.
  The stack has 220 B instead of 144 B (see [doc/health.md](doc/health.md) for the stack check).
- **Fixes**, e.g. the OEM installer sums the whole first payload segment.

Each commit says what it costs in code and memory.

## Versions

A tested build of `keeplink` gets a version `v<VERSION>-kl.<N>` as an annotated tag, e.g.
`v0.1.0-kl.1`. `VERSION` is upstream's (the `VERSION=` line of the Makefile), `N` counts our
versions. The tag's message names the upstream `main` commit the version is based on, the number of
our commits and the SHA-256 of the tested image:

```
git tag -n20 -l 'v*-kl.*'
```

The firmware shows upstream's version string `v<VERSION>-<hash>` (`version` on the console, the web
UI); `git tag --points-at <hash>` finds its tag. Since `keeplink` is rebased on every sync, the
commits of an older version stay reachable through its tag only.

## Building

As described in the [README](README.md), with Docker or the toolchain:

```
make MACHINE=KEEPLINK_KP_9000_9XHPML_V1_2
make machine_check
make -C test
```

PoE needs the PSE image, which is extracted from keepLiNK's firmware at build time; no vendor code
is in this repository. keepLiNK publishes no file for this model, but its V100.9.25 update for the
KP-9000-9XHML-X is the firmware of this board byte for byte. Download it from the
[download page](https://en.keeplink.com.cn/down.html)
(`https://en.keeplink.com.cn/upload/20260817143041171242.bin`, sha256
`c26e7394362c788114012ecd374547940dcbb97dc91c9250a53026127c4b0913`) and put it, unchanged, at
`stock_fw/KEEPLINK_KP_9000_9XHPML_V1_2.bin` (see [stock_fw/README.md](stock_fw/README.md)). Without
it the build stops; `NO_POE=1` builds without PoE, and the PoE ports then stay dark.

## Installing

The same ways as upstream: from the stock firmware with the OEM installer (`installer/`, built for
this machine), then updates on RTLPlayground's own update page. Both were done on this switch.
Upstream's warnings apply unchanged: make a backup of the flash with a SOIC-8 clamp before the
first install, and keep it. The OEM installer replaces the stock boot loader.

## Problems

Upstream is not responsible for this fork. Please do not report problems with our commits there;
if something also happens on upstream `main`, report it upstream as usual.
