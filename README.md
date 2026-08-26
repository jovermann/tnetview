# tnetview

`tnetview` is a colorful terminal application for inspecting local-network and
Internet connectivity. It starts on the Network Status page and remains
responsive while checks, device discovery, name resolution, and port inspection
run in background workers.

## Features

- Network diagnostics for interfaces, DHCP, the default route, gateway ARP,
  configured DNS servers, forward and reverse DNS, ICMP reachability, HTTPS,
  captive portals, traceroute, and possible DNS hijacking.
- Fast `/24` local-device discovery using parallel ICMP probes and the ARP cache.
- Asynchronous reverse-name lookup and inspection of ports 22, 80, and 443.
- Persistent known devices with editable user names, stored by MAC address in
  `~/.tnetview-known.tsv`.
- Interface and external-tool prerequisite pages.
- Always-on ANSI foreground and background colors.
- A non-interactive status report for scripts and diagnostics.

Page headers report `BUSY`, `SCANNING`, `INSPECTING`, or `IDLE`. Status tests
show `...` until their individual results arrive. `q` and Ctrl-C remain
responsive during network operations and restore the terminal before exiting.

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

## Runtime prerequisites

`tnetview` does not require a GUI toolkit or third-party C++ library. It uses
POSIX networking APIs and several standard operating-system utilities.

On macOS, the following tools are expected:

- `ping`
- `arp`
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

Local-device discovery assumes an IPv4 `/24` network. ICMP filtering may hide
devices that do not answer ping, although populated ARP entries are also used.

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
| `4` | Interfaces |
| `5` | Prerequisites |
| Up/Down or `k`/`j` | Select a device row |
| Space | Toggle whether the selected local device is known |
| `n` | Edit the selected known device's user name |
| `d` | Delete the selected known device |
| `r` | Refresh the current page |
| `t` | Show or hide the ANSI color matrix |
| `q` or Ctrl-C | Exit |
