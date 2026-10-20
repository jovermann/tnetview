# tnetview

`tnetview` is a colorful terminal application for inspecting local-network and
Internet connectivity. It starts on the Network Status page and remains
responsive while checks, device discovery, name resolution, and port inspection
run in background workers.

## Features

- Network diagnostics for interfaces, DHCP, the default route, gateway ARP,
  configured DNS servers, forward and reverse DNS, ICMP reachability, HTTPS,
  captive portals, traceroute, and possible DNS hijacking.
- Local-device discovery that shows cached ARP neighbors first, probes the
  selected IPv4 `/24` with parallel ICMP, and reads ARP again after the sweep.
  Captured ARP packets are included when packet capture is available.
- IPv6 neighbor-cache and captured Neighbor Advertisement correlation by MAC
  address, including IPv6-only peers, and Bonjour service discovery.
- Asynchronous host-name lookup through ARP, system DNS, local caches, mDNS,
  Avahi, and NetBIOS (where available), plus inspection of common TCP ports.
- MAC vendor lookup using IEEE MA-L, MA-M, and MA-S assignments from
  `ouidb.txt`.
- Persistent known devices with editable user names, stored by MAC address or
  by IP when no MAC is available, in `~/.tnetview-known.tsv`.
- Interface and external-tool prerequisite pages.
- Always-on ANSI foreground and background colors.
- A non-interactive status report for scripts and diagnostics.

Page headers report `BUSY`, `SCANNING`, `INSPECTING`, or `IDLE`. Status tests
show `...` until their individual results arrive. `q` and Ctrl-C remain
responsive during network operations and restore the terminal before exiting.
An ARP access line appears immediately below each page's table when ARP cannot
be read or has no resolved neighbors. It suggests restarting with
`sudo ./tnetview` when access fails; press `6` for details.
If the operating system does not expose its ARP table to the process, the
Gateway ARP check shows `N/A` and is excluded from the pass count. The ARP page
(`6`) shows resolved entries and MAC vendors, marks multicast addresses, and
hides incomplete probes. It also shows the result of the IPv4 route-table probe
when ARP is unavailable. Use
`./tnetview --arp -vv` for a one-shot report with raw ARP diagnostics on stderr;
`-v` prints a summary. In the interactive interface, redirect stderr to a file
when using `-vv` to keep the display readable.

The Local Devices and Unknown Devices pages show capture availability and, for
the selected device, IPv6 addresses, discovery evidence, open TCP ports, and
advertised Bonjour services. Unknown Devices contains observed peers that are
not marked known; Space marks a selected peer known, including one with no MAC.
Local, Known, and Unknown Devices are sorted by numeric IP address.
`./tnetview --discover -v` prints a one-shot discovery report. A
missing permission or utility reduces available observations without stopping
the scan. Host names are looked up after discovery; common TCP ports are
inspected last.

## Build prerequisites

Building requires:

- A POSIX development environment (currently macOS or Linux).
- A compiler and standard library supporting C++23 or later.
- GNU Make or a compatible `make` implementation.

## Building

Build the optimized binary:

```sh
make
```

Build with debug information and without optimization:

```sh
make BUILD=debug
```

Run the smoke tests or remove generated files:

```sh
make test
make clean
```

The resulting executable is `./tnetview`.

Keep `ouidb.txt` beside the executable (or in the working directory) for MAC
vendor lookup. Set `TNETVIEW_OUI_DB` to use a database at another path.

## Runtime prerequisites

`tnetview` does not require a GUI toolkit or third-party C++ library. It uses
POSIX networking APIs and several standard operating-system utilities.

On macOS, the following tools are expected:

- `ping`
- `arp`
- `tcpdump` (optional packet capture; requires privilege)
- `ndp` (optional IPv6 neighbor data)
- `dns-sd` (optional Bonjour discovery)
- `dscacheutil`
- `scutil`
- `route`
- `ipconfig`
- `traceroute`
- `nslookup`

These are normally included with macOS.

On Linux, the following tools are expected:

- `ping`
- `arp`
- `getent`
- `ip`
- `traceroute`
- `nslookup`

On Debian or Ubuntu they can be installed with:

```sh
sudo apt update
sudo apt install iputils-ping net-tools libc-bin iproute2 traceroute dnsutils
```

Missing tools do not prevent the application from starting. Their associated
checks may fail or be unavailable, and the Prerequisites page shows which tools
were found and whether their harmless probe command succeeded.

The `--discover` report optionally includes neighbors reported by a running
`lldpd` daemon. Switches that emit no discovery information cannot be
identified from this computer alone.

The active ICMP sweep assumes an IPv4 `/24` network. All resolved entries in
the ARP cache are shown, including neighbors outside that range. ICMP filtering
may hide devices that are absent from the cache and do not answer ping.

## Running

Start the interactive interface:

```sh
./tnetview
```

Useful commands:

```sh
./tnetview --help
./tnetview --once
./tnetview --timeout=200 --retries=3
./tnetview --interval=5
./tnetview --colors
```

`--once` prints one colored Network Status report without entering the
full-screen interface. `--colors` prints all supported ANSI foreground and
background combinations.

## Keyboard controls

| Key | Action |
|---|---|
| `1` | Network Status |
| `2` | Local Devices |
| `3` | Known Devices |
| `4` | Unknown Devices |
| `5` | Interfaces |
| `6` | Resolved ARP entries, MAC vendors, and access diagnosis |
| `7` | Prerequisites and tools |
| Up/Down or `k`/`j` | Select a device row |
| Up/Down or `k`/`j` on ARP | Scroll the ARP table |
| Space | Toggle whether the selected local or unknown device is known |
| `n` | Edit the selected known device's user name |
| `d` | Delete the selected known device |
| `r` | Refresh the current page |
| `t` | Show or hide the ANSI color matrix |
| `q` or Ctrl-C | Exit |

### ARP access on macOS 27

On this Mac, `./tnetview --arp -vv` returns an empty IPv4 ARP table as a normal
user, including when launched from Terminal. Running it with `sudo` exposes the
entries:

```sh
sudo ./tnetview --arp -vv
```

To use MAC addresses in the interactive Local Devices and ARP pages,
run `sudo ./tnetview`. This also makes the Known Devices database use root's
home directory instead of your normal `~/.tnetview-known.tsv`. The program does
not request elevated privileges automatically. Without readable ARP data, the
Gateway ARP check is `N/A` and MAC-based device discovery is incomplete. The
new packet capture source also requires privilege on macOS; the scan continues
using its other sources without it. To inspect all available local sources at
once, run `sudo ./tnetview --discover -v`.

Active discovery remains limited to the local link and the IPv4 `/24` chosen by the
current scanner. A packet trace on this Mac cannot reveal every conversation
between devices elsewhere on a switched network. Router DHCP leases, Wi-Fi
client lists, and managed-switch forwarding tables require access to those
devices and a device-specific integration; they are not collected yet.
