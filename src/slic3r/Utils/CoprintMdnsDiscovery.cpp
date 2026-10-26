#include "CoprintMdnsDiscovery.hpp"

#include "mdns.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

constexpr const char *kServiceName = "_coprintagent._tcp.local.";
constexpr auto        kQueryPeriod = std::chrono::seconds(3);
constexpr auto        kLostDebounce = std::chrono::seconds(5);
constexpr auto        kStaleAfter = std::chrono::seconds(8);
constexpr int         kDefaultPort = 7125;

std::string from_mdns(mdns_string_t value)
{
    if (value.str == nullptr || value.length == 0)
        return {};
    return std::string(value.str, value.length);
}

std::string to_lower_copy(std::string value)
{
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string strip_trailing_dot(std::string value)
{
    while (!value.empty() && value.back() == '.')
        value.pop_back();
    return value;
}

std::string service_key(std::string name)
{
    return to_lower_copy(strip_trailing_dot(std::move(name)));
}

bool name_is_coprint_service(const std::string &name)
{
    const std::string lower = to_lower_copy(name);
    return lower.find("_coprintagent._tcp") != std::string::npos;
}

std::string instance_label(const std::string &service)
{
    const std::string lower = to_lower_copy(service);
    const auto pos = lower.find("._coprintagent._tcp");
    if (pos != std::string::npos)
        return service.substr(0, pos);
    auto dot = service.find('.');
    if (dot != std::string::npos)
        return service.substr(0, dot);
    return strip_trailing_dot(service);
}

std::string sockaddr_ip(const struct sockaddr *addr)
{
    if (addr == nullptr)
        return {};
    char host[NI_MAXHOST] = {0};
    if (addr->sa_family == AF_INET) {
        const auto *in = reinterpret_cast<const struct sockaddr_in *>(addr);
        if (inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)) != nullptr)
            return host;
    } else if (addr->sa_family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const struct sockaddr_in6 *>(addr);
#ifdef IN6_IS_ADDR_LINKLOCAL
        if (IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr))
            return {};
#endif
        if (inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)) != nullptr)
            return host;
    }
    return {};
}

bool prefer_ipv4(const std::string &current, const std::string &incoming)
{
    if (incoming.empty())
        return false;
    if (current.empty())
        return true;
    const bool current_v6 = current.find(':') != std::string::npos;
    const bool incoming_v6 = incoming.find(':') != std::string::npos;
    return current_v6 && !incoming_v6;
}

bool is_loopback_or_linklocal(const std::string &ip)
{
    return ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0 || ip == "::1";
}

std::unordered_set<std::string> local_ipv4_set()
{
    std::unordered_set<std::string> out;
#ifdef _WIN32
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return out;
    std::vector<unsigned char> buffer(size);
    auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addresses, &size) != NO_ERROR)
        return out;
    for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;
        for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const std::string ip = sockaddr_ip(unicast->Address.lpSockaddr);
            if (!ip.empty() && !is_loopback_or_linklocal(ip))
                out.insert(ip);
        }
    }
#else
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0)
        return out;
    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        const std::string ip = sockaddr_ip(ifa->ifa_addr);
        if (!ip.empty() && !is_loopback_or_linklocal(ip))
            out.insert(ip);
    }
    freeifaddrs(ifaddr);
#endif
    return out;
}

struct PartialPrinter
{
    CoprintMdnsPrinter printer;
    CoprintMdnsPrinter last_published;
    std::string        hostname;
    std::chrono::steady_clock::time_point last_seen{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point lost_since{};
    bool               has_lost_since{false};
    bool               published{false};
    bool               ip_from_record{false};
};

struct QueryContext
{
    std::map<std::string, PartialPrinter> printers;
    std::vector<std::string>              packet_services;
    std::unordered_set<std::string>       local_ips;
    CoprintMdnsDiscovery::UpdateFn        on_update;
    std::string                           sender_ip;
};

bool ip_owned_by_other(const QueryContext *ctx, const std::string &ip, const std::string &self_key)
{
    if (ctx == nullptr || ip.empty())
        return false;
    for (const auto &entry : ctx->printers) {
        if (entry.first == self_key)
            continue;
        if (entry.second.printer.ip == ip)
            return true;
    }
    return false;
}

bool is_clone_hostname(const std::string &hostname)
{
    const std::string host_key = service_key(hostname);
    return host_key == "co-print.local" || host_key == "co-print" || host_key == "coprint.local";
}

bool hostname_shared(const QueryContext *ctx, const std::string &hostname)
{
    if (hostname.empty())
        return false;
    if (is_clone_hostname(hostname))
        return true;
    if (ctx == nullptr)
        return false;
    const std::string host_key = service_key(hostname);
    int count = 0;
    for (const auto &entry : ctx->printers) {
        if (service_key(entry.second.hostname) == host_key) {
            if (++count > 1)
                return true;
        }
    }
    return false;
}

CoprintMdnsPrinter make_complete(const PartialPrinter &partial)
{
    CoprintMdnsPrinter out = partial.printer;
    if (out.port <= 0)
        out.port = kDefaultPort;
    if (out.name.empty())
        out.name = instance_label(out.service);
    if (out.name.empty() && !out.model.empty())
        out.name = out.model;
    return out;
}

void publish_if_ready(QueryContext *ctx, PartialPrinter &partial, bool lost)
{
    if (ctx == nullptr || !ctx->on_update)
        return;
    if (!lost && partial.printer.ip.empty())
        return;
    const CoprintMdnsPrinter snapshot = make_complete(partial);
    if (lost) {
        if (!partial.published)
            return;
        partial.published = false;
        ctx->on_update(snapshot, true);
        return;
    }
    if (partial.published && snapshot == partial.last_published)
        return;
    partial.published = true;
    partial.last_published = snapshot;
    partial.printer = snapshot;
    ctx->on_update(snapshot, false);
}

void apply_ip_to_printer(PartialPrinter &partial, const std::string &ip, bool force)
{
    if (ip.empty() || is_loopback_or_linklocal(ip))
        return;
    if (partial.printer.ip == ip)
        return;
    if (!partial.printer.ip.empty() && !force && !prefer_ipv4(partial.printer.ip, ip))
        return;
    partial.printer.ip = ip;
    partial.ip_from_record = true;
}

void remember_packet_service(QueryContext *ctx, const std::string &key)
{
    if (ctx == nullptr || key.empty())
        return;
    if (std::find(ctx->packet_services.begin(), ctx->packet_services.end(), key) == ctx->packet_services.end())
        ctx->packet_services.push_back(key);
}

bool sender_usable(const QueryContext *ctx)
{
    if (ctx == nullptr || ctx->sender_ip.empty() || is_loopback_or_linklocal(ctx->sender_ip))
        return false;
    return ctx->local_ips.find(ctx->sender_ip) == ctx->local_ips.end();
}

void apply_packet_address(QueryContext *ctx, const std::string &hostname, const std::string &ip)
{
    if (ctx == nullptr || ip.empty() || is_loopback_or_linklocal(ip))
        return;
    if (ctx->local_ips.find(ip) != ctx->local_ips.end())
        return;
    // Cloned images share CO-PRINT.local. An A record for that hostname must
    // never be broadcast to every instance — only the datagram sender / TXT
    // may bind the address.
    if (ctx->packet_services.size() != 1)
        return;
    const std::string &key = ctx->packet_services.front();
    if (ip_owned_by_other(ctx, ip, key))
        return;
    PartialPrinter &partial = ctx->printers[key];
    if (hostname_shared(ctx, partial.hostname) || is_clone_hostname(hostname))
        return;
    apply_ip_to_printer(partial, ip, false);
    publish_if_ready(ctx, partial, false);
}

void finish_packet(QueryContext *ctx)
{
    if (ctx == nullptr || ctx->packet_services.size() != 1 || !sender_usable(ctx))
        return;
    const std::string &key = ctx->packet_services.front();
    if (ip_owned_by_other(ctx, ctx->sender_ip, key))
        return;
    PartialPrinter &partial = ctx->printers[key];
    const bool shared = hostname_shared(ctx, partial.hostname);
    if (!partial.printer.ip.empty() && !shared && partial.printer.ip != ctx->sender_ip)
        return;
    apply_ip_to_printer(partial, ctx->sender_ip, shared || partial.printer.ip.empty());
    publish_if_ready(ctx, partial, false);
}

void touch_printer(QueryContext *ctx, const std::string &service, uint32_t ttl)
{
    if (ctx == nullptr || service.empty() || !name_is_coprint_service(service))
        return;
    const std::string key = service_key(service);
    remember_packet_service(ctx, key);
    PartialPrinter &partial = ctx->printers[key];
    if (partial.printer.service.empty())
        partial.printer.service = strip_trailing_dot(service);
    partial.last_seen = std::chrono::steady_clock::now();
    if (ttl == 0) {
        if (!partial.has_lost_since) {
            partial.lost_since = partial.last_seen;
            partial.has_lost_since = true;
        }
        return;
    }
    partial.has_lost_since = false;
    publish_if_ready(ctx, partial, false);
}

int query_callback(int /*sock*/, const struct sockaddr *from, size_t /*addrlen*/,
                   mdns_entry_type_t entry, uint16_t /*query_id*/, uint16_t rtype,
                   uint16_t /*rclass*/, uint32_t ttl, const void *data, size_t size,
                   size_t name_offset, size_t /*name_length*/, size_t record_offset,
                   size_t record_length, void *user_data)
{
    auto *ctx = static_cast<QueryContext *>(user_data);
    if (ctx == nullptr || entry == MDNS_ENTRYTYPE_QUESTION)
        return 0;

    if (ctx->sender_ip.empty())
        ctx->sender_ip = sockaddr_ip(from);

    char name_buf[256];
    size_t name_ofs = name_offset;
    const std::string record_name = from_mdns(
        mdns_string_extract(data, size, &name_ofs, name_buf, sizeof(name_buf)));

    if (rtype == MDNS_RECORDTYPE_PTR) {
        char ptr_buf[256];
        const std::string instance = from_mdns(
            mdns_record_parse_ptr(data, size, record_offset, record_length, ptr_buf, sizeof(ptr_buf)));
        if (name_is_coprint_service(instance) || name_is_coprint_service(record_name))
            touch_printer(ctx, instance.empty() ? record_name : instance, ttl);
        return 0;
    }

    if (rtype == MDNS_RECORDTYPE_SRV) {
        char srv_buf[256];
        const mdns_record_srv_t srv =
            mdns_record_parse_srv(data, size, record_offset, record_length, srv_buf, sizeof(srv_buf));
        const std::string instance = name_is_coprint_service(record_name) ? record_name : from_mdns(srv.name);
        if (!name_is_coprint_service(instance) && !name_is_coprint_service(record_name))
            return 0;
        const std::string key = service_key(record_name.empty() ? instance : record_name);
        touch_printer(ctx, record_name.empty() ? instance : record_name, ttl);
        PartialPrinter &partial = ctx->printers[key];
        if (srv.port > 0)
            partial.printer.port = srv.port;
        partial.hostname = from_mdns(srv.name);
        if (ttl > 0)
            publish_if_ready(ctx, partial, false);
        return 0;
    }

    if (rtype == MDNS_RECORDTYPE_A) {
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        mdns_record_parse_a(data, size, record_offset, record_length, &addr);
        apply_packet_address(ctx, record_name, sockaddr_ip(reinterpret_cast<struct sockaddr *>(&addr)));
        return 0;
    }

    if (rtype == MDNS_RECORDTYPE_AAAA) {
        struct sockaddr_in6 addr;
        std::memset(&addr, 0, sizeof(addr));
        mdns_record_parse_aaaa(data, size, record_offset, record_length, &addr);
        apply_packet_address(ctx, record_name, sockaddr_ip(reinterpret_cast<struct sockaddr *>(&addr)));
        return 0;
    }

    if (rtype == MDNS_RECORDTYPE_TXT) {
        if (!name_is_coprint_service(record_name))
            return 0;
        mdns_record_txt_t txts[16];
        const size_t parsed = mdns_record_parse_txt(data, size, record_offset, record_length, txts, 16);
        touch_printer(ctx, record_name, ttl);
        const std::string key = service_key(record_name);
        PartialPrinter &partial = ctx->printers[key];
        for (size_t i = 0; i < parsed; ++i) {
            const std::string txt_key = to_lower_copy(from_mdns(txts[i].key));
            const std::string txt_val = from_mdns(txts[i].value);
            if (txt_key == "id")
                partial.printer.id = txt_val;
            else if (txt_key == "model")
                partial.printer.model = txt_val;
            else if (txt_key == "fw" || txt_key == "firmware")
                partial.printer.fw = txt_val;
            else if (txt_key == "name" || txt_key == "device_name")
                partial.printer.name = txt_val;
            else if (txt_key == "ip" || txt_key == "address" || txt_key == "ipv4") {
                if (!ip_owned_by_other(ctx, txt_val, key))
                    apply_ip_to_printer(partial, txt_val, partial.printer.ip.empty());
            }
        }
        if (ttl > 0)
            publish_if_ready(ctx, partial, false);
    }
    return 0;
}

void send_ptr_query(const int *sockets, int count)
{
    if (sockets == nullptr || count <= 0)
        return;
    alignas(8) uint8_t buffer[2048];
    const size_t name_len = std::strlen(kServiceName);
    for (int i = 0; i < count; ++i) {
        if (sockets[i] < 0)
            continue;
        mdns_query_send(sockets[i], MDNS_RECORDTYPE_PTR, kServiceName, name_len, buffer, sizeof(buffer), 0);
    }
}

void sweep_lost(QueryContext *ctx)
{
    if (ctx == nullptr)
        return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = ctx->printers.begin(); it != ctx->printers.end();) {
        PartialPrinter &partial = it->second;
        if (!partial.has_lost_since && now - partial.last_seen >= kStaleAfter) {
            partial.lost_since = now;
            partial.has_lost_since = true;
        }
        if (partial.has_lost_since && now - partial.lost_since >= kLostDebounce) {
            publish_if_ready(ctx, partial, true);
            it = ctx->printers.erase(it);
            continue;
        }
        ++it;
    }
}

#ifdef _WIN32
struct WinsockGuard
{
    bool ok{false};
    WinsockGuard()
    {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard()
    {
        if (ok)
            WSACleanup();
    }
};
#endif

} // namespace

CoprintMdnsDiscovery::~CoprintMdnsDiscovery()
{
    stop();
}

void CoprintMdnsDiscovery::start(UpdateFn on_update)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_on_update = std::move(on_update);
    }
    m_running = true;
    m_query_now = true;
    m_thread = std::thread([this]() { worker(); });
}

void CoprintMdnsDiscovery::stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_update = {};
}

void CoprintMdnsDiscovery::request_query()
{
    m_query_now = true;
}

void CoprintMdnsDiscovery::worker()
{
#ifdef _WIN32
    WinsockGuard winsock;
    if (!winsock.ok) {
        BOOST_LOG_TRIVIAL(warning) << "CoprintMdnsDiscovery: WSAStartup failed";
        return;
    }
#endif

    int sockets[32];
    const int num_sockets = open_client_sockets(sockets, 32, 0);
    if (num_sockets <= 0) {
        BOOST_LOG_TRIVIAL(warning) << "CoprintMdnsDiscovery: no mDNS sockets";
        return;
    }

    QueryContext ctx;
    ctx.local_ips = local_ipv4_set();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ctx.on_update = m_on_update;
    }

    BOOST_LOG_TRIVIAL(info) << "CoprintMdnsDiscovery: querying " << kServiceName
                            << " on " << num_sockets << " socket(s)";

    send_ptr_query(sockets, num_sockets);
    m_query_now = false;
    auto next_query = std::chrono::steady_clock::now() + kQueryPeriod;

    alignas(8) uint8_t recv_buffer[2048];
    while (m_running.load()) {
        if (m_query_now.exchange(false) || std::chrono::steady_clock::now() >= next_query) {
            send_ptr_query(sockets, num_sockets);
            next_query = std::chrono::steady_clock::now() + kQueryPeriod;
        }

        fd_set readset;
        FD_ZERO(&readset);
        int nfds = 0;
        for (int i = 0; i < num_sockets; ++i) {
            if (sockets[i] < 0)
                continue;
            FD_SET(sockets[i], &readset);
#ifndef _WIN32
            nfds = std::max(nfds, sockets[i] + 1);
#endif
        }
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
#ifdef _WIN32
        const int ready = select(0, &readset, nullptr, nullptr, &timeout);
#else
        const int ready = select(nfds, &readset, nullptr, nullptr, &timeout);
#endif
        if (ready > 0) {
            for (int i = 0; i < num_sockets; ++i) {
                if (sockets[i] < 0)
                    continue;
                if (!FD_ISSET(sockets[i], &readset))
                    continue;
                ctx.packet_services.clear();
                ctx.sender_ip.clear();
                mdns_query_recv(sockets[i], recv_buffer, sizeof(recv_buffer), query_callback, &ctx, 0);
                finish_packet(&ctx);
            }
        }
        sweep_lost(&ctx);
    }

    for (int i = 0; i < num_sockets; ++i) {
        if (sockets[i] >= 0)
            mdns_socket_close(sockets[i]);
    }
}

} // namespace Slic3r
