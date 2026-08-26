// tnetview - Colorful terminal network monitor.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "CommandLineParser.hpp"
#include "Tui.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <future>
#include <ifaddrs.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <regex>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

/// Monotonic clock used for deadlines and refresh scheduling.
using Clock = std::chrono::steady_clock;

/// Program version reported by --version.
static constexpr const char* Version = "0.1.0";

/// Result of one network status check.
struct Result
{
    bool        ok {};
    std::string test;
    std::string target;
    std::string timing;
    std::string detail;
    bool        ready { true };
};

/// IPv4 interface information displayed on the Interfaces page.
struct Interface
{
    std::string name;
    std::string address;
    std::string mask;
    unsigned    flags {};
};

/// Availability and executable path of one external runtime tool.
struct Prerequisite
{
    bool        ok {};
    std::string tool;
    std::string path;
};

/// Discovered local-network device and asynchronously inspected properties.
struct Device
{
    std::string ip;
    std::string name;
    std::string mac;
    std::string vendor;
    std::string ports;
    bool        known {};
};

/// User-maintained device identity persisted by normalized MAC address.
struct KnownDevice
{
    std::string mac;
    std::string userName;
    std::string dns;
    std::string ip;
    std::string vendor;
};

/// Shared snapshots and progress counters published by background workers.
struct AppState
{
    std::mutex                mutex;
    std::vector<Result>       status;
    std::vector<Device>       devices;
    std::vector<Prerequisite> prereqs;
    std::vector<Interface>    ifaces;
    std::string               gatewayAddress;
    std::vector<std::string>  dnsAddresses;
    bool                      statusBusy {}, devicesBusy {}, prereqBusy {};
    bool                      devicesEnrichBusy {};
    bool                      interfaceBusy {};
    size_t                    statusDone {}, statusTotal { 17 };
    unsigned                  devicesDone {}, devicesTotal { 254 };
    unsigned                  devicesEnrichDone {}, devicesEnrichTotal {};
    uint64_t                  statusGeneration {}, devicesGeneration {}, prereqGeneration {};
    uint64_t                  interfaceGeneration {};
};

/// Set by the signal handler and polled by the interactive event loop.
static volatile sig_atomic_t interrupted {};

/// Request a clean exit after SIGINT or SIGTERM.
static void onSignal(int)
{
    interrupted = 1;
}

static std::string gateway();
static Result      connectCheck(std::string test, std::string host, int port, int timeoutMs);

/// Process environment passed through to directly spawned utilities.
extern char** environ;

/// Run one directly spawned ICMP probe with a strict parent-enforced deadline.
static bool scanPing(const std::string& ip)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid {};
#ifdef __APPLE__
    std::string wait   = "10";
    char*       argv[] = { const_cast<char*>("ping"), const_cast<char*>("-n"), const_cast<char*>("-c"),
              const_cast<char*>("1"), const_cast<char*>("-W"), wait.data(), const_cast<char*>(ip.c_str()), nullptr };
#else
    std::string wait   = "1";
    char*       argv[] = { const_cast<char*>("ping"), const_cast<char*>("-n"), const_cast<char*>("-c"),
              const_cast<char*>("1"), const_cast<char*>("-W"), wait.data(), const_cast<char*>(ip.c_str()), nullptr };
#endif
    int created = posix_spawnp(&pid, "ping", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (created != 0)
        return false;
    int  status {};
    auto deadline = Clock::now() + std::chrono::milliseconds(100);
    while (Clock::now() < deadline)
    {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    return false;
}

/// Enumerate IPv4 addresses and netmasks for all local interfaces.
static std::vector<Interface> interfaces()
{
    std::vector<Interface> result;
    ifaddrs*               head {};
    if (getifaddrs(&head) != 0)
        return result;
    for (auto* p = head; p; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        char  address[INET_ADDRSTRLEN] {}, mask[INET_ADDRSTRLEN] {};
        auto* a = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        auto* m = reinterpret_cast<sockaddr_in*>(p->ifa_netmask);
        inet_ntop(AF_INET, &a->sin_addr, address, sizeof(address));
        if (m)
            inet_ntop(AF_INET, &m->sin_addr, mask, sizeof(mask));
        result.push_back({ p->ifa_name, address, mask, p->ifa_flags });
    }
    freeifaddrs(head);
    return result;
}

/// Run a fixed internal shell command and return its standard output.
static std::string commandOutput(const char* command)
{
    std::string out;
    FILE*       pipe = popen(command, "r");
    if (!pipe)
        return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
    return out;
}

/// Remove leading and trailing ASCII whitespace.
static std::string trim(std::string s)
{
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

/// Resolve host through the system resolver and return its first IPv4 address.
static std::string resolveAddress(const std::string& host)
{
    addrinfo hints {};
    hints.ai_family = AF_INET;
    addrinfo*   info {};
    std::string result;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &info) == 0)
    {
        char  ip[INET_ADDRSTRLEN] {};
        auto* a = reinterpret_cast<sockaddr_in*>(info->ai_addr);
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        result = ip;
        freeaddrinfo(info);
    }
    return result;
}

/// Ping host with retries and return a status-table result including latency.
static Result pingCheck(std::string test, const std::string& host, int timeoutMs, int retries)
{
    Result r { false, std::move(test), {}, {}, host + " timeout" };
    int    failed = 0;
    for (int attempt = 0; attempt < retries; ++attempt)
    {
#ifdef __APPLE__
        std::string cmd = "ping -n -c 1 -W " + std::to_string(timeoutMs) + " " + host + " 2>&1";
#else
        std::string cmd = "ping -n -c 1 -W 1 " + host + " 2>&1";
#endif
        auto        out = commandOutput(cmd.c_str());
        std::smatch m;
        if (std::regex_search(out, m, std::regex("time[=<]\\s*([0-9.,]+)\\s*ms")))
        {
            r.ok     = true;
            r.timing = m[1].str() + " ms";
            std::smatch ip;
            if (std::regex_search(out, ip, std::regex("PING [^ ]+ \\(([^)]+)\\)")))
                r.target = ip[1];
            break;
        }
        ++failed;
    }
    if (r.target.empty() && std::regex_match(host, std::regex("[0-9.]+")))
        r.target = host;
    if (r.ok)
        r.detail = failed ? "had " + std::to_string(failed) + "/" + std::to_string(retries) + " failures" : "";
    else
        r.detail += " failed " + std::to_string(failed) + "/" + std::to_string(retries);
    return r;
}

/// Resolve an IPv4 address to a DNS name without falling back to numeric text.
static std::string reverseName(const std::string& ip)
{
    sockaddr_in a {};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    char host[NI_MAXHOST] {};
    return getnameinfo(reinterpret_cast<sockaddr*>(&a), sizeof(a), host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0
        ? host
        : "";
}

/// Resolve the diagnostic hostname through one explicitly selected DNS server.
static std::string explicitDns(const std::string& server)
{
    if (server.empty())
        return {};
    auto               out = commandOutput(("nslookup heise.de " + server + " 2>/dev/null").c_str());
    std::istringstream in(out);
    std::string        line, result;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.rfind("Address:", 0) == 0)
        {
            auto value = trim(line.substr(8));
            if (value != server && value.find('#') == std::string::npos)
                result = value;
        }
    }
    return result;
}

/// Normalize a MAC address to six uppercase, zero-padded hexadecimal octets.
static std::string formatMac(std::string mac)
{
    std::replace(mac.begin(), mac.end(), '-', ':');
    std::istringstream in(mac);
    std::string        part, result;
    size_t             count = 0;
    while (std::getline(in, part, ':'))
    {
        if (part.empty() || part.size() > 2 || count++ >= 6)
            return {};
        unsigned           value {};
        std::istringstream hex(part);
        hex >> std::hex >> value;
        if (hex.fail() || !hex.eof() || value > 255)
            return {};
        if (!result.empty())
            result += ':';
        std::ostringstream out;
        out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << value;
        result += out.str();
    }
    return count == 6 ? result : "";
}

/// Return the default gateway's normalized MAC address from the ARP cache.
static std::string gatewayMac(const std::string& gw)
{
    if (gw.empty())
        return {};
    auto        out = commandOutput("arp -an 2>/dev/null");
    std::smatch m;
    if (std::regex_search(out, m, std::regex("\\(" + gw + "\\) at ([0-9a-fA-F:]+)")))
        return formatMac(m[1]);
    return {};
}

/// Parse the operating system's ARP table into IP-to-(name, MAC) entries.
static std::map<std::string, std::pair<std::string, std::string>> arpTable()
{
    std::map<std::string, std::pair<std::string, std::string>> result;
    auto                                                       out = commandOutput("arp -an 2>/dev/null");
    std::istringstream                                         in(out);
    std::string                                                line;
    std::regex                                                 pattern("^(.*?) ?\\(([^)]+)\\) at ([0-9a-fA-F:]+)");
    std::smatch                                                m;
    while (std::getline(in, line))
        if (std::regex_search(line, m, pattern))
            result[m[2]] = { trim(m[1]), formatMac(m[3]) };
    return result;
}

/// Provide a fallback classification when no external OUI database is present.
static std::string vendorForMac(const std::string& mac)
{
    if (mac.size() < 2)
        return {};
    unsigned first {};
    std::istringstream(mac.substr(0, 2)) >> std::hex >> first;
    return first & 2 ? "locally administered" : (first & 1 ? "multicast" : "unknown vendor");
}

/// Return whether a short TCP connection to ip:port succeeds.
static bool portOpen(const std::string& ip, int port)
{
    return connectCheck("", ip, port, 200).ok;
}

/// Return the last octet of an IPv4 address for natural subnet ordering.
static unsigned ipLast(const std::string& ip)
{
    auto p = ip.rfind('.');
    try
    {
        return static_cast<unsigned>(std::stoul(ip.substr(p + 1)));
    }
    catch (...)
    {
        return 0;
    }
}

/// Discover the local /24 concurrently and publish incremental progress updates.
static std::vector<Device> scanDevices(const std::function<void(const Device&, unsigned, unsigned)>& update = {})
{
    std::string local, localName;
    for (const auto& i: interfaces())
        if (i.address != "127.0.0.1")
        {
            local = i.address;
            break;
        }
    if (local.empty())
        return {};
    char hostname[256] {};
    gethostname(hostname, sizeof(hostname));
    localName = hostname;
    auto dot  = local.rfind('.');
    if (dot == std::string::npos)
        return {};
    auto                          base = local.substr(0, dot + 1);
    std::atomic<unsigned>         next { 1 }, scanned { 0 };
    std::mutex                    lock;
    std::map<std::string, Device> found;
    found[local] = { local, localName, {}, {}, {} };
    if (update)
        update(found[local], 0, 254);
    auto gw = gateway();
    if (!gw.empty())
    {
        found[gw] = { gw, {}, {}, {}, {} };
        if (update)
            update(found[gw], 0, 254);
    }
    std::vector<std::future<void>> workers;
    for (unsigned worker = 0; worker < 128; ++worker)
        workers.push_back(std::async(std::launch::async,
            [&]
            {
                for (;;)
                {
                    auto n = next.fetch_add(1);
                    if (n > 254)
                        break;
                    auto ip = base + std::to_string(n);
                    if (ip == local)
                        continue;
                    bool alive = scanPing(ip);
                    auto count = scanned.fetch_add(1) + 1;
                    if (alive)
                    {
                        Device copy { ip, {}, {}, {}, {} };
                        {
                            std::lock_guard guard(lock);
                            found.try_emplace(ip, copy);
                        }
                        if (update)
                            update(copy, count, 254);
                    }
                    else if (update && count % 8 == 0)
                        update({}, count, 254);
                }
            }));
    for (auto& w: workers)
        w.get();
    for (const auto& [ip, identity]: arpTable())
    {
        if (ip.rfind(base, 0) == 0 && ipLast(ip) > 0 && ipLast(ip) < 255)
        {
            auto& d = found[ip];
            d.ip    = ip;
            if (d.name.empty())
                d.name = identity.first;
            d.mac = identity.second;
            if (update)
                update(d, 254, 254);
        }
    }
    std::vector<Device> out;
    for (auto& [ip, d]: found)
        out.push_back(std::move(d));
    std::sort(out.begin(), out.end(),
        [](const auto& a, const auto& b)
        {
            return ipLast(a.ip) < ipLast(b.ip);
        });
    return out;
}

/// Return the per-user known-device database path.
static std::string knownPath()
{
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.tnetview-known.tsv" : ".tnetview-known.tsv";
}
/// Load valid known-device records from the tab-separated database.
static std::vector<KnownDevice> loadKnown()
{
    std::vector<KnownDevice> out;
    std::ifstream            in(knownPath());
    std::string              line;
    while (std::getline(in, line))
    {
        std::vector<std::string> f;
        std::istringstream       row(line);
        std::string              v;
        while (std::getline(row, v, '\t'))
            f.push_back(v);
        if (f.size() >= 5)
        {
            auto mac = formatMac(f[0]);
            if (!mac.empty())
                out.push_back({ mac, f[1], f[2], f[3], f[4] });
        }
    }
    return out;
}
/// Rewrite the known-device database from the current in-memory records.
static void saveKnown(const std::vector<KnownDevice>& rows)
{
    std::ofstream out(knownPath(), std::ios::trunc);
    for (const auto& r: rows)
        out << r.mac << '\t' << r.userName << '\t' << r.dns << '\t' << r.ip << '\t' << r.vendor << '\n';
}
/// Merge persisted names and current discovery data by MAC address.
static void applyKnown(std::vector<Device>& devices, std::vector<KnownDevice>& known)
{
    for (auto& d: devices)
        for (auto& k: known)
            if (!d.mac.empty() && d.mac == k.mac)
            {
                d.known  = true;
                k.ip     = d.ip;
                k.dns    = d.name;
                k.vendor = d.vendor;
                if (!k.userName.empty())
                    d.name = k.userName;
            }
}

/// Return human-readable DHCP lease information when supported by the OS.
static std::string dhcpLease(const std::string& iface)
{
#ifdef __APPLE__
    if (iface.empty())
        return "Not available";
    auto               out = commandOutput(("ipconfig getpacket " + iface + " 2>/dev/null").c_str());
    std::istringstream in(out);
    std::string        line;
    while (std::getline(in, line))
        if (line.find("lease_time") != std::string::npos || line.find("lease_expiration") != std::string::npos)
            return trim(line);
    return "Not available";
#else
    (void)iface;
    return "Not supported";
#endif
}

/// Query the operating system for the default IPv4 gateway.
static std::string gateway()
{
#ifdef __APPLE__
    auto out = commandOutput("route -n get default 2>/dev/null");
    auto pos = out.find("gateway:");
    if (pos != std::string::npos)
    {
        pos += 8;
        auto end = out.find('\n', pos);
        out      = out.substr(pos, end - pos);
    }
#else
    auto out = commandOutput("ip route show default 2>/dev/null");
    auto pos = out.find(" via ");
    if (pos != std::string::npos)
    {
        pos += 5;
        auto end = out.find(' ', pos);
        out      = out.substr(pos, end - pos);
    }
#endif
    auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    auto last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

/// Query up to two unique DNS servers configured by the operating system.
static std::vector<std::string> dnsServers()
{
    std::vector<std::string> result;
#ifdef __APPLE__
    std::istringstream in(commandOutput("scutil --dns 2>/dev/null"));
    std::string        line;
    while (std::getline(in, line))
    {
        auto p = line.find("nameserver[");
        auto c = line.find(':');
        if (p != std::string::npos && c != std::string::npos)
            result.push_back(line.substr(c + 2));
    }
#else
    FILE* f = fopen("/etc/resolv.conf", "r");
    char  line[256];
    while (f && fgets(line, sizeof(line), f))
    {
        std::istringstream in(line);
        std::string        key, value;
        in >> key >> value;
        if (key == "nameserver")
            result.push_back(value);
    }
    if (f)
        fclose(f);
#endif
    std::vector<std::string> unique;
    for (const auto& s: result)
    {
        bool seen {};
        for (const auto& u: unique)
            seen |= u == s;
        if (!seen && unique.size() < 2)
            unique.push_back(s);
    }
    return unique;
}

/// Run the system-resolver status check.
static Result resolveCheck(std::string host)
{
    Result   r { false, "DNS system resolve", {}, {}, "resolve failed" };
    auto     start = Clock::now();
    addrinfo hints {};
    hints.ai_family = AF_INET;
    addrinfo* info {};
    if (getaddrinfo(host.c_str(), nullptr, &hints, &info) == 0)
    {
        char  ip[INET_ADDRSTRLEN] {};
        auto* a = reinterpret_cast<sockaddr_in*>(info->ai_addr);
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        r.ok     = true;
        r.target = ip;
        r.detail.clear();
        freeaddrinfo(info);
    }
    r.timing
        = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()) + " ms";
    return r;
}

/// Attempt a nonblocking TCP connection within timeoutMs.
static Result connectCheck(std::string test, std::string host, int port, int timeoutMs)
{
    Result   r { false, std::move(test), host, {}, "connection failed" };
    auto     start = Clock::now();
    addrinfo hints {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info {};
    if (getaddrinfo(host.c_str(), nullptr, &hints, &info) != 0)
        return r;
    char  ip[INET_ADDRSTRLEN] {};
    auto* sin = reinterpret_cast<sockaddr_in*>(info->ai_addr);
    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
    r.target = ip;
    int fd   = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    sin->sin_port = htons(static_cast<uint16_t>(port));
    int status    = connect(fd, info->ai_addr, info->ai_addrlen);
    if (status != 0 && errno == EINPROGRESS)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        status = select(fd + 1, nullptr, &set, nullptr, &tv);
    }
    if (status > 0 || status == 0)
    {
        int       error {};
        socklen_t len = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
        r.ok = error == 0;
    }
    close(fd);
    freeaddrinfo(info);
    r.detail = r.ok ? "connected" : "timeout/refused";
    r.timing
        = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()) + " ms";
    return r;
}

/// Verify Google's connectivity endpoint returns HTTP 204 without redirection.
static Result portalCheck(int timeoutMs)
{
    auto r = connectCheck("HTTP 204 check", "connectivitycheck.gstatic.com", 80, timeoutMs);
    if (!r.ok)
    {
        r.detail = "connectivity endpoint unavailable";
        return r;
    }
    addrinfo hints {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info {};
    if (getaddrinfo("connectivitycheck.gstatic.com", "80", &hints, &info) != 0)
        return r;
    int     fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, info->ai_addr, info->ai_addrlen) == 0)
    {
        const char req[]
            = "GET /generate_204 HTTP/1.0\r\nHost: connectivitycheck.gstatic.com\r\nConnection: close\r\n\r\n";
        send(fd, req, sizeof(req) - 1, 0);
        char buf[256] {};
        auto n   = recv(fd, buf, sizeof(buf) - 1, 0);
        r.ok     = n > 0 && std::string(buf).find(" 204 ") != std::string::npos;
        r.detail = r.ok ? "HTTP 204 — no portal" : "unexpected HTTP response";
    }
    close(fd);
    freeaddrinfo(info);
    return r;
}

/// Verify Apple's captive-network endpoint returns HTTP 200.
static Result appleCheck(int timeoutMs)
{
    auto r = connectCheck("Apple captive check", "captive.apple.com", 80, timeoutMs);
    if (!r.ok)
    {
        r.detail = "HTTP error";
        return r;
    }
    addrinfo hints {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info {};
    if (getaddrinfo("captive.apple.com", "80", &hints, &info) != 0)
        return r;
    int     fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, info->ai_addr, info->ai_addrlen) == 0)
    {
        const char req[] = "GET /hotspot-detect.html HTTP/1.0\r\nHost: captive.apple.com\r\nConnection: close\r\n\r\n";
        send(fd, req, sizeof(req) - 1, 0);
        char buf[256] {};
        auto n   = recv(fd, buf, sizeof(buf) - 1, 0);
        r.ok     = n > 0 && std::string(buf).find(" 200 ") != std::string::npos;
        r.detail = r.ok ? "" : "HTTP error";
    }
    close(fd);
    freeaddrinfo(info);
    return r;
}

/// Run a bounded traceroute and report the number of visible hops.
static Result tracerouteCheck()
{
    auto               out = commandOutput("traceroute -n -m 5 -w 1 8.8.8.8 2>/dev/null");
    std::istringstream in(out);
    std::string        line;
    int                hops = 0;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0])))
            ++hops;
    }
    return { hops > 0, "Traceroute 8.8.8.8", "8.8.8.8", {}, hops ? std::to_string(hops) + " hops" : "no hops" };
}

/// Return whether ip belongs to a private, loopback, or link-local IPv4 range.
static bool privateIpv4(const std::string& ip)
{
    in_addr a {};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1)
        return true;
    uint32_t v = ntohl(a.s_addr);
    return (v >> 24) == 10 || (v >> 20) == 0xac1 || (v >> 16) == 0xc0a8 || (v >> 24) == 127 || (v >> 16) == 0xa9fe;
}

/// Run all status checks concurrently and publish each completed result.
static std::vector<Result> runChecks(
    int timeoutMs, int retries, const std::function<void(const Result&, size_t, size_t)>& update = {})
{
    auto        ifs    = interfaces();
    bool        online = false;
    std::string local, iface, mask;
    for (const auto& i: ifs)
        if (i.address != "127.0.0.1")
        {
            online = true;
            if (local.empty())
            {
                local = i.address;
                iface = i.name;
                mask  = i.mask;
            }
        }
    auto       gw           = gateway();
    auto       dns          = dnsServers();
    const bool hasSecondDns = dns.size() > 1;
    if (dns.empty())
        dns.emplace_back();
    std::vector<std::future<Result>> jobs;
    jobs.push_back(std::async(std::launch::async,
        [=]
        {
            return gw.empty() ? Result { false, "Local gateway", {}, {}, "Not found" }
                              : pingCheck("Local gateway", gw, timeoutMs, retries);
        }));
    for (size_t i = 0; i < dns.size(); ++i)
        jobs.push_back(std::async(std::launch::async,
            [=]
            {
                auto ip = explicitDns(dns[i]);
                return Result { !ip.empty(), "DNS server " + std::to_string(i + 1), ip.empty() ? dns[i] : ip, {},
                    ip.empty() ? "resolve failed" : "" };
            }));
    jobs.push_back(std::async(std::launch::async,
        [=]
        {
            return Result { online, "Interface status", local, {},
                online ? iface + " " + mask : "interface down or no IP" };
        }));
    jobs.push_back(std::async(std::launch::async,
        [=]
        {
            auto lease = dhcpLease(iface);
            return Result { lease != "Not supported" && lease != "Not available", "DHCP lease", local, {}, lease };
        }));
    jobs.push_back(std::async(std::launch::async,
        [=]
        {
            auto mac = gatewayMac(gw);
            return Result { !mac.empty(), "Gateway ARP", gw, {}, mac.empty() ? "not found" : mac };
        }));
    jobs.push_back(std::async(std::launch::async,
        [=]
        {
            return Result { !gw.empty(), "Default route", gw, {}, gw.empty() ? "Not found" : gw };
        }));
    jobs.push_back(std::async(std::launch::async, resolveCheck, "heise.de"));
    jobs.push_back(std::async(std::launch::async, pingCheck, "Ping 8.8.8.8", "8.8.8.8", timeoutMs, retries));
    jobs.push_back(std::async(std::launch::async, pingCheck, "Ping 1.1.1.1", "1.1.1.1", timeoutMs, retries));
    jobs.push_back(std::async(std::launch::async, pingCheck, "Ping heise.de", "heise.de", timeoutMs, retries));
    jobs.push_back(std::async(std::launch::async,
        [=]
        {
            auto r = connectCheck("TCP 443 heise.de", "heise.de", 443, 1500);
            if (!r.ok)
                r.detail = "connect failed";
            else
                r.detail = "";
            return r;
        }));
    jobs.push_back(std::async(std::launch::async, portalCheck, 2000));
    jobs.push_back(std::async(std::launch::async, appleCheck, 2000));
    jobs.push_back(std::async(std::launch::async, tracerouteCheck));
    for (const auto& ip: { std::string("1.1.1.1"), std::string("8.8.8.8") })
        jobs.push_back(std::async(std::launch::async,
            [ip]
            {
                auto name = reverseName(ip);
                return Result { !name.empty(), "Reverse lookup " + ip, ip, {}, name.empty() ? "reverse failed" : name };
            }));
    jobs.push_back(std::async(std::launch::async,
        []
        {
            auto ip = resolveAddress("heise.de");
            bool ok = !ip.empty() && !privateIpv4(ip);
            return Result { ok, "DNS hijack check", ip, {}, ok ? "" : "private IP" };
        }));
    std::map<std::string, Result> completed;
    std::vector<bool>             collected(jobs.size());
    size_t                        done = 0;
    while (done < jobs.size())
    {
        for (size_t i = 0; i < jobs.size(); ++i)
            if (!collected[i] && jobs[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                auto r            = jobs[i].get();
                completed[r.test] = r;
                collected[i]      = true;
                ++done;
                if (update)
                    update(r, done, jobs.size());
            }
        if (done < jobs.size())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<std::string> order = { "Interface status", "DHCP lease", "Local gateway", "Gateway ARP",
        "Default route", "DNS system resolve", "DNS server 1" };
    if (hasSecondDns)
        order.push_back("DNS server 2");
    const std::vector<std::string> remainder
        = { "Reverse lookup 1.1.1.1", "Reverse lookup 8.8.8.8", "Ping 8.8.8.8", "Ping 1.1.1.1", "Ping heise.de",
              "TCP 443 heise.de", "HTTP 204 check", "Apple captive check", "Traceroute 8.8.8.8", "DNS hijack check" };
    order.insert(order.end(), remainder.begin(), remainder.end());
    std::vector<Result> out;
    for (const auto& name: order)
        out.push_back(std::move(completed.at(name)));
    return out;
}

/// Execute harmless platform-specific probes for required external tools.
static std::vector<Prerequisite> prerequisites()
{
#ifdef __APPLE__
    const std::vector<std::pair<std::string, std::string>> tools
        = { { "ping", "ping -c 1 127.0.0.1 >/dev/null 2>&1" }, { "arp", "arp -an >/dev/null 2>&1" },
              { "dscacheutil", "dscacheutil -q host -a ip_address 127.0.0.1 >/dev/null 2>&1" },
              { "scutil", "scutil --dns >/dev/null 2>&1" }, { "route", "route -n get default >/dev/null 2>&1" },
              { "ipconfig", "ipconfig ifcount >/dev/null 2>&1" },
              { "traceroute", "traceroute -n -m 1 127.0.0.1 >/dev/null 2>&1" },
              { "nslookup", "nslookup localhost >/dev/null 2>&1" } };
#else
    const std::vector<std::pair<std::string, std::string>> tools
        = { { "ping", "ping -c 1 127.0.0.1 >/dev/null 2>&1" }, { "arp", "arp -a >/dev/null 2>&1" },
              { "getent", "getent hosts localhost >/dev/null 2>&1" }, { "ip", "ip route show default >/dev/null 2>&1" },
              { "traceroute", "traceroute -n -m 1 127.0.0.1 >/dev/null 2>&1" },
              { "nslookup", "nslookup localhost >/dev/null 2>&1" } };
#endif
    std::vector<Prerequisite> out;
    for (const auto& [tool, probe]: tools)
    {
        auto path = trim(commandOutput(("command -v " + tool + " 2>/dev/null").c_str()));
        bool ok   = false;
        if (!path.empty())
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                execl("/bin/sh", "sh", "-c", probe.c_str(), static_cast<char*>(nullptr));
                _exit(127);
            }
            auto until = Clock::now() + std::chrono::seconds(2);
            int  status {};
            while (Clock::now() < until)
            {
                if (waitpid(pid, &status, WNOHANG) == pid)
                {
                    ok  = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                    pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (pid > 0)
            {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
        }
        out.push_back({ ok, tool, path });
    }
    return out;
}

/// Truncate or right-pad a plain string to exactly n terminal columns.
static std::string pad(std::string s, size_t n)
{
    if (s.size() > n)
        s = ut1::tui::fitTerminalLine(s, n);
    if (s.size() < n)
        s += std::string(n - s.size(), ' ');
    return s;
}
/// Render the full-width menu bar and its green keyboard-help line.
static void printMenu(const std::string& text, const std::string& help, size_t width, bool color)
{
    std::cout << "\n"
              << (color ? ut1::tui::ansiWhiteOnBlue : "") << pad(text, width) << (color ? ut1::tui::ansiReset : "")
              << "\n"
              << (color ? ut1::tui::ansiBrightGreen : "") << help << (color ? ut1::tui::ansiReset : "") << "\n"
              << std::flush;
}
/// Render the interactive ANSI foreground/background color matrix.
static void renderColors()
{
    using namespace ut1::tui;
    clearScreen();
    auto width = terminalWidth();
    std::cout << ansiWhiteOnBlue << pad(" tnetview  ANSI COLOR MATRIX", std::max<size_t>(30, width)) << ansiReset
              << '\n';
    printAnsiColorMatrix(std::cout, terminalHeight() > 3 ? terminalHeight() - 3 : 0);
    printMenu(" t Colors: return", "  q Quit", width, true);
}
/// Render the Network Status page from its current immutable snapshot.
static void renderStatus(
    const std::vector<Result>& rows, bool busy, size_t done, size_t total, bool color, bool fullScreen)
{
    using namespace ut1::tui;
    if (fullScreen)
        clearScreen();
    const auto width = terminalWidth();
    auto       c     = [&](const char* x)
    {
        return color ? x : "";
    };
    size_t                      ok = 0;
    std::map<std::string, bool> passed;
    for (const auto& r: rows)
        if (r.ready)
        {
            ok += r.ok;
            passed[r.test] = r.ok;
        }
    std::string summary = busy
        ? "Checking network... (" + std::to_string(done) + "/" + std::to_string(total) + ")"
        : (!passed["Local gateway"] ? "Local network unreachable"
                                    : (!(passed["Ping 8.8.8.8"] || passed["Ping 1.1.1.1"])
                                              ? "Internet unreachable"
                                              : (!passed["Ping heise.de"] ? "DNS error" : "Internet access OK")));
    std::cout << c(ansiWhiteOnBlue)
              << pad(" tnetview  NETWORK STATUS  [" + std::string(busy ? "BUSY" : "IDLE") + "]",
                     std::max<size_t>(30, width))
              << c(ansiReset) << "\n";
    std::cout << c(summary == "Internet access OK" ? ansiBrightGreen : ansiBrightYellow) << "  " << summary << "  —  "
              << ok << "/" << rows.size() << " checks passed" << c(ansiReset) << "\n\n";
    std::cout << c(ansiBold) << "  " << pad("STATUS", 9) << pad("TEST", 25) << pad("IP", 18) << pad("PING", 10)
              << "DETAILS" << c(ansiReset) << "\n";
    for (const auto& r: rows)
    {
        const char* state = !r.ready ? ansiGray : (r.ok ? ansiBrightGreen : ansiBrightRed);
        std::string line  = "  " + pad(!r.ready ? "..." : (r.ok ? "PASS" : "FAIL"), 9) + pad(r.test, 25)
            + pad(r.target, 18) + pad(r.timing, 10) + r.detail;
        std::cout << c(state) << fitTerminalLine(line, width) << c(ansiReset) << "\n";
    }
    if (fullScreen)
        printMenu(" 1 Status   2 Local Devices   3 Known Devices   4 Interfaces   5 Prerequisites",
            "  r Refresh   t Colors   q Quit", width, color);
}
/// Render local interfaces and asynchronously queried resolver/routing data.
static void renderInterfaces(const std::vector<Interface>& rows, const std::string& gatewayAddress,
    const std::vector<std::string>& dnsAddresses, bool busy, bool color)
{
    using namespace ut1::tui;
    clearScreen();
    auto c = [&](const char* x)
    {
        return color ? x : "";
    };
    auto width = terminalWidth();
    std::cout << c(ansiWhiteOnBlue)
              << pad(" tnetview  INTERFACES  [" + std::string(busy ? "BUSY" : "IDLE") + "]",
                     std::max<size_t>(30, width))
              << c(ansiReset) << "\n\n"
              << c(ansiBold) << "  " << pad("INTERFACE", 18) << pad("IPv4 ADDRESS", 20) << "NETMASK" << c(ansiReset)
              << "\n";
    for (const auto& i: rows)
        std::cout << c(i.address == "127.0.0.1" ? ansiGray : ansiBrightGreen) << "  " << pad(i.name, 18)
                  << pad(i.address, 20) << i.mask << c(ansiReset) << "\n";
    std::cout << c(ansiBrightGreen) << "\n  Default gateway: " << gatewayAddress << "\n  DNS servers: ";
    for (const auto& s: dnsAddresses)
        std::cout << s << ' ';
    std::cout << c(ansiReset);
    printMenu(" 1 Status   2 Local Devices   3 Known Devices   4 Interfaces   5 Prerequisites",
        "  r Refresh   t Colors   q Quit", width, color);
}

/// Render external runtime-tool availability and progress.
static void renderPrerequisites(const std::vector<Prerequisite>& rows, bool busy, bool color)
{
    using namespace ut1::tui;
    clearScreen();
    auto c = [&](const char* x)
    {
        return color ? x : "";
    };
    auto   width = terminalWidth();
    size_t ok    = 0;
    for (const auto& r: rows)
        ok += r.ok;
    std::cout << c(ansiWhiteOnBlue)
              << pad(" tnetview  PREREQUISITES  [" + std::string(busy ? "BUSY" : "IDLE") + "]",
                     std::max<size_t>(30, width))
              << c(ansiReset) << "\n"
              << c(ok == rows.size() && !busy ? ansiBrightGreen : ansiBrightYellow) << "  "
              << (busy ? "Checking tools..."
                       : std::to_string(ok) + "/" + std::to_string(rows.size()) + " tools available and working")
              << c(ansiReset) << "\n\n";
    std::cout << c(ansiBold) << "  " << pad("STATUS", 9) << pad("TOOL", 20) << "PATH" << c(ansiReset) << "\n";
    for (const auto& r: rows)
        std::cout << c(r.ok ? ansiBrightGreen : ansiBrightRed)
                  << fitTerminalLine("  " + pad(r.ok ? "PASS" : "FAIL", 9) + pad(r.tool, 20)
                             + (r.path.empty() ? "not found" : r.path),
                         width)
                  << c(ansiReset) << "\n";
    printMenu(" 1 Status   2 Local Devices   3 Known Devices   4 Interfaces   5 Prerequisites",
        "  r Refresh   t Colors   q Quit", width, color);
}

/// Render discovered devices and discovery/inspection progress.
static void renderDevices(const std::vector<Device>& rows, size_t selected, bool busy, unsigned done, unsigned total,
    bool enrichBusy, unsigned enrichDone, unsigned enrichTotal, bool color)
{
    using namespace ut1::tui;
    clearScreen();
    auto c = [&](const char* x)
    {
        return color ? x : "";
    };
    auto        width    = terminalWidth();
    std::string progress = busy
        ? "SCANNING " + std::to_string(done) + "/" + std::to_string(total)
        : (enrichBusy ? "INSPECTING " + std::to_string(enrichDone) + "/" + std::to_string(enrichTotal) : "IDLE");
    std::cout << c(ansiWhiteOnBlue)
              << pad(" tnetview  LOCAL DEVICES (" + std::to_string(rows.size()) + ")  [" + progress + "]",
                     std::max<size_t>(30, width))
              << c(ansiReset) << "\n\n";
    std::cout << c(ansiBold) << "  " << pad("IP ADDRESS", 16) << pad("WEB", 6) << pad("KNOWN", 8) << pad("NAME", 22)
              << pad("MAC", 19) << pad("MAC VENDOR", 22) << "PORTS" << c(ansiReset) << "\n";
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r    = rows[i];
        std::string line = (i == selected ? "> " : "  ") + pad(r.ip, 16)
            + pad((r.ports.find("80") != std::string::npos || r.ports.find("443") != std::string::npos) ? "yes" : "", 6)
            + pad(r.known ? "[x]" : "[ ]", 8) + pad(r.name, 22) + pad(r.mac, 19) + pad(r.vendor, 22) + r.ports;
        std::cout << c(i == selected ? ansiBlackOnCyan : ansiBrightGreen) << fitTerminalLine(line, width)
                  << c(ansiReset) << "\n";
    }
    printMenu(" 1 Status   2 Local Devices   3 Known Devices   4 Interfaces   5 Prerequisites",
        "  Up/Down Select   Space Toggle known   r Rescan   t Colors   q Quit", width, color);
}

/// Render persisted known devices and the current selection.
static void renderKnown(const std::vector<KnownDevice>& rows, size_t selected, bool color)
{
    using namespace ut1::tui;
    clearScreen();
    auto c = [&](const char* x)
    {
        return color ? x : "";
    };
    auto width = terminalWidth();
    std::cout << c(ansiWhiteOnBlue)
              << pad(" tnetview  KNOWN DEVICES (" + std::to_string(rows.size()) + ")  [IDLE]",
                     std::max<size_t>(30, width))
              << c(ansiReset) << "\n\n";
    std::cout << c(ansiBold) << "  " << pad("DEL", 6) << pad("IP", 16) << pad("USER NAME", 22) << pad("DNS", 24)
              << pad("MAC", 19) << "VENDOR" << c(ansiReset) << "\n";
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r    = rows[i];
        std::string line = (i == selected ? "> " : "  ") + pad("[d]", 6) + pad(r.ip, 16) + pad(r.userName, 22)
            + pad(r.dns, 24) + pad(r.mac, 19) + r.vendor;
        std::cout << c(i == selected ? ansiBlackOnCyan : ansiBrightGreen) << fitTerminalLine(line, width)
                  << c(ansiReset) << "\n";
    }
    printMenu(" 1 Status   2 Local Devices   3 Known Devices   4 Interfaces   5 Prerequisites",
        "  Up/Down Select   n Rename   d Delete   t Colors   q Quit", width, color);
}

/// Read an editable user name in raw terminal mode; Escape cancels the edit.
static std::string readName(const std::string& current)
{
    std::string value = current;
    std::cout << "\n  User name: " << value << std::flush;
    for (;;)
    {
        int key = ut1::tui::readStdinByte();
        if (interrupted || key == 3)
            return current;
        if (key == '\r' || key == '\n')
            break;
        if (key == 27)
            return current;
        if ((key == 127 || key == 8) && !value.empty())
        {
            value.pop_back();
            std::cout << "\b \b" << std::flush;
        }
        else if (key >= 32 && key < 127 && key != '\t')
        {
            value.push_back(static_cast<char>(key));
            std::cout << static_cast<char>(key) << std::flush;
        }
    }
    return value;
}

/// Return the stable display order for all possible status checks.
static std::vector<std::string> statusOrder()
{
    return { "Interface status", "DHCP lease", "Local gateway", "Gateway ARP", "Default route", "DNS system resolve",
        "DNS server 1", "DNS server 2", "Reverse lookup 1.1.1.1", "Reverse lookup 8.8.8.8", "Ping 8.8.8.8",
        "Ping 1.1.1.1", "Ping heise.de", "TCP 443 heise.de", "HTTP 204 check", "Apple captive check",
        "Traceroute 8.8.8.8", "DNS hijack check" };
}
/// Return the display rank of a named status check.
static size_t statusRank(const std::string& name)
{
    auto order = statusOrder();
    auto it    = std::find(order.begin(), order.end(), name);
    return static_cast<size_t>(std::distance(order.begin(), it));
}
/// Build initial status rows whose result cells display as pending.
static std::vector<Result> pendingStatus()
{
    std::vector<Result> rows;
    for (const auto& name: statusOrder())
        if (name != "DNS server 2")
        {
            std::string target;
            if (name.find("1.1.1.1") != std::string::npos)
                target = "1.1.1.1";
            else if (name.find("8.8.8.8") != std::string::npos)
                target = "8.8.8.8";
            rows.push_back({ false, name, target, {}, {}, false });
        }
    return rows;
}

/// Start a new nonblocking status generation and ignore stale worker results.
static void startStatus(const std::shared_ptr<AppState>& state, int timeout, int retries)
{
    uint64_t generation;
    {
        std::lock_guard lock(state->mutex);
        generation         = ++state->statusGeneration;
        state->status      = pendingStatus();
        state->statusBusy  = true;
        state->statusDone  = 0;
        state->statusTotal = state->status.size();
    }
    std::thread(
        [state, generation, timeout, retries]
        {
            auto            rows = runChecks(timeout, retries,
                           [state, generation](const Result& r, size_t done, size_t total)
                           {
                    std::lock_guard lock(state->mutex);
                    if (generation != state->statusGeneration)
                        return;
                    auto it = std::find_if(state->status.begin(), state->status.end(),
                                   [&](const auto& old)
                                   {
                            return old.test == r.test;
                        });
                    if (it == state->status.end())
                        state->status.push_back(r);
                    else
                        *it = r;
                    std::sort(state->status.begin(), state->status.end(),
                                   [](const auto& a, const auto& b)
                                   {
                            return statusRank(a.test) < statusRank(b.test);
                        });
                    state->statusDone  = done;
                    state->statusTotal = total;
                });
            std::lock_guard lock(state->mutex);
            if (generation == state->statusGeneration)
            {
                state->status     = std::move(rows);
                state->statusDone = state->statusTotal = state->status.size();
                state->statusBusy                      = false;
            }
        })
        .detach();
}

/// Insert or replace a device snapshot and retain natural IPv4 ordering.
static void mergeDevice(std::vector<Device>& rows, const Device& device)
{
    auto it = std::find_if(rows.begin(), rows.end(),
        [&](const auto& old)
        {
            return old.ip == device.ip;
        });
    if (it == rows.end())
        rows.push_back(device);
    else
        *it = device;
    std::sort(rows.begin(), rows.end(),
        [](const auto& a, const auto& b)
        {
            return ipLast(a.ip) < ipLast(b.ip);
        });
}

/// Start background discovery followed by independent name and port inspection.
static void startDeviceScan(const std::shared_ptr<AppState>& state)
{
    uint64_t generation;
    {
        std::lock_guard lock(state->mutex);
        generation = ++state->devicesGeneration;
        state->devices.clear();
        state->devicesBusy       = true;
        state->devicesEnrichBusy = false;
        state->devicesDone       = 0;
        state->devicesEnrichDone = 0;
    }
    std::thread(
        [state, generation]
        {
            auto rows = scanDevices(
                [state, generation](const Device& device, unsigned done, unsigned total)
                {
                    std::lock_guard lock(state->mutex);
                    if (generation != state->devicesGeneration)
                        return;
                    state->devicesDone  = done;
                    state->devicesTotal = total;
                    if (!device.ip.empty())
                        mergeDevice(state->devices, device);
                });
            {
                std::lock_guard lock(state->mutex);
                if (generation != state->devicesGeneration)
                    return;
                state->devices            = rows;
                state->devicesDone        = state->devicesTotal;
                state->devicesBusy        = false;
                state->devicesEnrichBusy  = !rows.empty();
                state->devicesEnrichDone  = 0;
                state->devicesEnrichTotal = static_cast<unsigned>(rows.size() * 2);
            }
            std::vector<std::future<void>> jobs;
            for (const auto& device: rows)
            {
                jobs.push_back(std::async(std::launch::async,
                    [state, generation, device]
                    {
                        Device update = device;
                        if (update.name.empty())
                            update.name = reverseName(update.ip);
                        std::lock_guard lock(state->mutex);
                        if (generation != state->devicesGeneration)
                            return;
                        auto it = std::find_if(state->devices.begin(), state->devices.end(),
                            [&](const auto& d)
                            {
                                return d.ip == update.ip;
                            });
                        if (it != state->devices.end())
                            it->name = update.name;
                        ++state->devicesEnrichDone;
                    }));
                jobs.push_back(std::async(std::launch::async,
                    [state, generation, device]
                    {
                        Device update = device;
                        for (int port: { 22, 80, 443 })
                            if (portOpen(update.ip, port))
                            {
                                if (!update.ports.empty())
                                    update.ports += ",";
                                update.ports += std::to_string(port);
                            }
                        update.vendor = vendorForMac(update.mac);
                        std::lock_guard lock(state->mutex);
                        if (generation != state->devicesGeneration)
                            return;
                        auto it = std::find_if(state->devices.begin(), state->devices.end(),
                            [&](const auto& d)
                            {
                                return d.ip == update.ip;
                            });
                        if (it != state->devices.end())
                        {
                            it->ports  = update.ports;
                            it->vendor = update.vendor;
                        }
                        ++state->devicesEnrichDone;
                    }));
            }
            for (auto& job: jobs)
                job.get();
            std::lock_guard lock(state->mutex);
            if (generation == state->devicesGeneration)
                state->devicesEnrichBusy = false;
        })
        .detach();
}

/// Start a nonblocking prerequisite check generation.
static void startPrerequisites(const std::shared_ptr<AppState>& state)
{
    uint64_t generation;
    {
        std::lock_guard lock(state->mutex);
        generation = ++state->prereqGeneration;
        state->prereqs.clear();
        state->prereqBusy = true;
    }
    std::thread(
        [state, generation]
        {
            auto            rows = prerequisites();
            std::lock_guard lock(state->mutex);
            if (generation == state->prereqGeneration)
            {
                state->prereqs    = std::move(rows);
                state->prereqBusy = false;
            }
        })
        .detach();
}

/// Refresh interface, gateway, and DNS snapshots on a background worker.
static void startInterfaces(const std::shared_ptr<AppState>& state)
{
    uint64_t generation;
    {
        std::lock_guard lock(state->mutex);
        generation           = ++state->interfaceGeneration;
        state->interfaceBusy = true;
    }
    std::thread(
        [state, generation]
        {
            auto            rows = interfaces();
            auto            gw   = gateway();
            auto            dns  = dnsServers();
            std::lock_guard lock(state->mutex);
            if (generation == state->interfaceGeneration)
            {
                state->ifaces         = std::move(rows);
                state->gatewayAddress = std::move(gw);
                state->dnsAddresses   = std::move(dns);
                state->interfaceBusy  = false;
            }
        })
        .detach();
}

/// Parse options and run either a one-shot report or the interactive event loop.
int main(int argc, char* argv[])
try
{
    ut1::CommandLineParser cl("tnetview", "Usage: tnetview [options]\n\nColorful terminal network status monitor.",
        "\nKeys: 1 status, 2 local devices, 3 known devices, 4 interfaces, 5 prerequisites.\n"
        "      Up/Down or j/k select, Space toggles known, n renames, d deletes.\n"
        "      r refresh, t show colors, q quit.\n",
        Version);
    cl.addOption('t', "timeout", "Connection timeout in milliseconds.", "MS", "1000");
    cl.addOption(0, "retries", "Number of ICMP ping attempts.", "COUNT", "5");
    cl.addOption('i', "interval", "Auto-refresh interval in seconds (0 disables).", "SECONDS", "0");
    cl.addOption(0, "once", "Print the status page once and exit.");
    cl.addOption(0, "colors", "Show all ANSI color combinations and exit.");
    cl.parse(argc, argv);
    auto timeout = cl.getInt("timeout"), interval = cl.getInt("interval"), retries = cl.getInt("retries");
    if (timeout < 1 || timeout > 60000 || interval < 0 || retries < 1 || retries > 20)
    {
        std::cerr << "tnetview: invalid timeout, interval, or retries\n";
        return 2;
    }
    bool color = true;
    if (cl.isSet("colors"))
    {
        ut1::tui::printAnsiColorMatrix(std::cout, 16);
        return 0;
    }
    if (cl.isSet("once") || !isatty(STDIN_FILENO))
    {
        auto rows = runChecks(static_cast<int>(timeout), static_cast<int>(retries));
        renderStatus(rows, false, rows.size(), rows.size(), color, false);
        return 0;
    }
    auto   state    = std::make_shared<AppState>();
    auto   known    = loadKnown();
    size_t selected = 0;
    startStatus(state, static_cast<int>(timeout), static_cast<int>(retries));
    startInterfaces(state);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    ut1::tui::TerminalRawMode raw(STDIN_FILENO);
    ut1::tui::enterAlternateScreen();
    int  page = 1;
    bool done = false, showColors = false;
    auto nextRefresh = Clock::now() + std::chrono::seconds(interval);
    while (!done && !interrupted)
    {
        std::vector<Result>       rows;
        std::vector<Device>       devices;
        std::vector<Prerequisite> prereqs;
        std::vector<Interface>    ifaces;
        std::string               gw;
        std::vector<std::string>  dns;
        bool                      statusBusy, devicesBusy, devicesEnrichBusy, prereqBusy, interfaceBusy;
        size_t                    statusDone, statusTotal;
        unsigned                  devicesDone, devicesTotal, devicesEnrichDone, devicesEnrichTotal;
        {
            std::lock_guard lock(state->mutex);
            rows               = state->status;
            devices            = state->devices;
            prereqs            = state->prereqs;
            ifaces             = state->ifaces;
            gw                 = state->gatewayAddress;
            dns                = state->dnsAddresses;
            statusBusy         = state->statusBusy;
            devicesBusy        = state->devicesBusy;
            devicesEnrichBusy  = state->devicesEnrichBusy;
            prereqBusy         = state->prereqBusy;
            interfaceBusy      = state->interfaceBusy;
            statusDone         = state->statusDone;
            statusTotal        = state->statusTotal;
            devicesDone        = state->devicesDone;
            devicesTotal       = state->devicesTotal;
            devicesEnrichDone  = state->devicesEnrichDone;
            devicesEnrichTotal = state->devicesEnrichTotal;
        }
        applyKnown(devices, known);
        if (showColors)
            renderColors();
        else if (page == 1)
            renderStatus(rows, statusBusy, statusDone, statusTotal, color, true);
        else if (page == 2)
            renderDevices(devices, selected, devicesBusy, devicesDone, devicesTotal, devicesEnrichBusy,
                devicesEnrichDone, devicesEnrichTotal, color);
        else if (page == 3)
            renderKnown(known, selected, color);
        else if (page == 4)
            renderInterfaces(ifaces, gw, dns, interfaceBusy, color);
        else
            renderPrerequisites(prereqs, prereqBusy, color);
        int key = ut1::tui::readKey(100);
        if (key == ut1::tui::keyUp)
            key = 'k';
        if (key == ut1::tui::keyDown)
            key = 'j';
        if (interval && Clock::now() >= nextRefresh)
        {
            if (page == 1)
                startStatus(state, static_cast<int>(timeout), static_cast<int>(retries));
            nextRefresh = Clock::now() + std::chrono::seconds(interval);
        }
        if (interrupted)
            break;
        switch (key)
        {
        case 3:
        case 'q':
        case 'Q':
            done = true;
            break;
        case '1':
            page       = 1;
            selected   = 0;
            showColors = false;
            break;
        case '2':
            page       = 2;
            selected   = 0;
            showColors = false;
            if (devices.empty() && !devicesBusy)
                startDeviceScan(state);
            break;
        case '3':
            page       = 3;
            selected   = 0;
            showColors = false;
            break;
        case '4':
            page       = 4;
            showColors = false;
            if (ifaces.empty() && !interfaceBusy)
                startInterfaces(state);
            break;
        case '5':
            page       = 5;
            showColors = false;
            if (prereqs.empty() && !prereqBusy)
                startPrerequisites(state);
            break;
        case 'j':
            if ((page == 2 && selected + 1 < devices.size()) || (page == 3 && selected + 1 < known.size()))
                ++selected;
            break;
        case 'k':
            if ((page == 2 || page == 3) && selected > 0)
                --selected;
            break;
        case ' ':
            if (page == 2 && selected < devices.size() && !devices[selected].mac.empty())
            {
                auto& d  = devices[selected];
                auto  it = std::find_if(known.begin(), known.end(),
                     [&](const auto& k)
                     {
                        return k.mac == d.mac;
                    });
                if (it == known.end())
                    known.push_back({ d.mac, "", d.name, d.ip, d.vendor });
                else
                    known.erase(it);
                saveKnown(known);
            }
            break;
        case 'n':
        case 'N':
            if (page == 3 && selected < known.size())
            {
                known[selected].userName = readName(known[selected].userName);
                saveKnown(known);
            }
            break;
        case 'd':
        case 'D':
            if (page == 3 && selected < known.size())
            {
                known.erase(known.begin() + static_cast<std::ptrdiff_t>(selected));
                if (selected >= known.size() && selected)
                    --selected;
                saveKnown(known);
            }
            break;
        case 't':
        case 'T':
            showColors = !showColors;
            break;
        case 'r':
        case 'R':
            if (page == 1)
                startStatus(state, static_cast<int>(timeout), static_cast<int>(retries));
            else if (page == 2)
            {
                startDeviceScan(state);
                selected = 0;
            }
            else if (page == 3)
                known = loadKnown();
            else if (page == 4)
                startInterfaces(state);
            else
                startPrerequisites(state);
            break;
        default:
            break;
        }
    }
    ut1::tui::leaveAlternateScreen();
    return 0;
}
catch (const std::exception& e)
{
    ut1::tui::leaveAlternateScreen();
    std::cerr << "tnetview: " << e.what() << '\n';
    return 1;
}
