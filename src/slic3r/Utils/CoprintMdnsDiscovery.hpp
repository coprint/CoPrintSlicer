#ifndef slic3r_CoprintMdnsDiscovery_hpp_
#define slic3r_CoprintMdnsDiscovery_hpp_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

struct CoprintMdnsPrinter
{
    std::string service;
    std::string name;
    std::string ip;
    std::string id;
    std::string model;
    std::string fw;
    // Agent advertisement port from SRV (werkzeug), not Moonraker 7125.
    int         port{7125};

    bool operator==(const CoprintMdnsPrinter &other) const
    {
        return service == other.service && name == other.name && ip == other.ip &&
               id == other.id && model == other.model && fw == other.fw && port == other.port;
    }
    bool operator!=(const CoprintMdnsPrinter &other) const { return !(*this == other); }
};

// Background DNS-SD query for CoPrint agents advertising
// `_coprintagent._tcp.local.`. Callbacks may arrive on a worker thread.
class CoprintMdnsDiscovery
{
public:
    using UpdateFn = std::function<void(const CoprintMdnsPrinter &printer, bool lost)>;

    CoprintMdnsDiscovery() = default;
    CoprintMdnsDiscovery(const CoprintMdnsDiscovery &) = delete;
    CoprintMdnsDiscovery &operator=(const CoprintMdnsDiscovery &) = delete;
    ~CoprintMdnsDiscovery();

    void start(UpdateFn on_update);
    void stop();
    void request_query();

private:
    void worker();

    std::mutex          m_mutex;
    UpdateFn            m_on_update;
    std::thread         m_thread;
    std::atomic<bool>   m_running{false};
    std::atomic<bool>   m_query_now{false};
};

} // namespace Slic3r

#endif
