#include <stdio.h>

#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {

constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);

bool syscall_failed(std::uint64_t result)
{
    return result >= static_cast<std::uint64_t>(-4095);
}

int syscall_errno(std::uint64_t result)
{
    return static_cast<int>(-static_cast<std::int64_t>(result));
}

std::uint64_t net_syscall(std::Syscall number, std::uint64_t arg1 = 0, std::uint64_t arg2 = 0)
{
    return _syscall_impl(static_cast<std::uint64_t>(number), arg1, arg2);
}

void write_str(const char* s)
{
    std::serial_write(s, std::strlen(s));
}

bool handle_request(std::Handle queue, const std::IPCMessage& message)
{
    std::services::graphics_compositor::HelloReply reply = {};
    reply.header.version = std::services::network_manager::VERSION;
    reply.header.opcode = 1;
    reply.status = std::services::STATUS_OK;
    std::strncpy(reply.service_name, std::services::network_manager::NAME, sizeof(reply.service_name) - 1);

    return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
}

// Replace `path` with `contents` in one step: readers see the old file or the
// new one, never half of it.
void write_file(const char* path, const char* contents)
{
    char temp[64];
    snprintf(temp, sizeof(temp), "%s.new", path);
    constexpr std::uint64_t flags = 0x1 | 0100 | 01000;   // O_WRONLY | O_CREAT | O_TRUNC
    const std::FileHandle fd = std::open(temp, flags, 0644);
    if (fd == fail) {
        printf("[network.manager] cannot write %s\n", temp);
        return;
    }
    std::write(fd, contents, std::strlen(contents));
    std::close(fd);
    if (syscall_failed(_syscall_impl(static_cast<std::uint64_t>(std::Syscall::Rename),
            reinterpret_cast<std::uint64_t>(temp), reinterpret_cast<std::uint64_t>(path)))) {
        printf("[network.manager] cannot rename %s\n", temp);
    }
}

void format_ip(char* out, std::size_t size, std::uint32_t network_order)
{
    const auto* b = reinterpret_cast<const unsigned char*>(&network_order);
    snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

// /etc/resolv.conf and /etc/hosts follow the interface: the name servers and
// domain from the lease, and the host name on the leased address.
void apply_config(const std::NetConfig& config)
{
    char resolv[256] = {};
    std::size_t used = 0;
    if (config.domain[0] != '\0') {
        used += snprintf(resolv + used, sizeof(resolv) - used, "search %s\n", config.domain);
    }
    for (std::uint32_t i = 0; i < config.dnsCount && i < std::NET_CONFIG_MAX_DNS; ++i) {
        char ip[16];
        format_ip(ip, sizeof(ip), config.dns[i]);
        used += snprintf(resolv + used, sizeof(resolv) - used, "nameserver %s\n", ip);
    }
    write_file("/etc/resolv.conf", resolv);

    char hosts[128];
    if ((config.flags & std::NetConfigUp) != 0) {
        char ip[16];
        format_ip(ip, sizeof(ip), config.address);
        snprintf(hosts, sizeof(hosts), "127.0.0.1\tlocalhost\n%s\tinstantos\n", ip);
    } else {
        snprintf(hosts, sizeof(hosts), "127.0.0.1\tlocalhost\n127.0.1.1\tinstantos\n");
    }
    write_file("/etc/hosts", hosts);

    if ((config.flags & std::NetConfigUp) != 0) {
        char ip[16], gw[16];
        format_ip(ip, sizeof(ip), config.address);
        format_ip(gw, sizeof(gw), config.gateway);
        printf("[network.manager] %s address %s gateway %s, %u name server(s); resolv.conf written\n",
            (config.flags & std::NetConfigDhcp) != 0 ? "dhcp" : "static", ip, gw,
            static_cast<unsigned>(config.dnsCount));
    } else {
        printf("[network.manager] interface down\n");
    }
}

// Sets the clock once from NTP (/bin/sntp, which picks the DHCP server or
// pool.ntp.org) the first time the interface comes up. TLS certificate
// checks need the right date; the RTC is usually close, this makes it exact.
void sync_clock(void*)
{
    std::Stat st {};
    if (syscall_failed(std::stat("/bin/sntp", &st))) {
        return;
    }
    const char* const argv[] = { "sntp", "-s", "-q", "-W", "2", "-r", "2", nullptr };
    const char* const envp[] = { "PATH=/bin", nullptr };
    const std::uint64_t pid = std::spawn("/bin/sntp", argv, envp);
    if (pid == fail || pid == 0) {
        printf("[network.manager] cannot start sntp\n");
        return;
    }
    int status = 0;
    std::wait(pid, &status);
    if (status == 0) {
        printf("[network.manager] clock set from NTP\n");
    } else {
        printf("[network.manager] NTP clock sync failed (status %d)\n", (status >> 8) & 0xff);
    }
}

// Follows the kernel's interface configuration for as long as we run.
void watch_config(void*)
{
    bool clock_synced = false;
    std::uint64_t generation = 0;
    for (;;) {
        std::NetConfig config = {};
        const std::uint64_t result = _syscall_impl(static_cast<std::uint64_t>(std::Syscall::NetGetConfig),
            reinterpret_cast<std::uint64_t>(&config), generation, static_cast<std::uint64_t>(-1));
        if (syscall_failed(result)) {
            _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), 1000);
            continue;
        }
        if (config.generation != generation) {
            generation = config.generation;
            apply_config(config);
            if (!clock_synced && (config.flags & std::NetConfigUp) != 0) {
                clock_synced = true;
                if (std::thread_create(sync_clock) == fail) {
                    write_str("[network.manager] thread_create failed\n");
                }
            }
        }
    }
}

void print_device_status()
{
    unsigned char mac[6] {};
    const std::uint64_t mac_result = net_syscall(std::Syscall::NetGetMAC, reinterpret_cast<std::uint64_t>(mac));
    if (syscall_failed(mac_result)) {
        printf("[network.manager] no virtio-net errno=%d\n", syscall_errno(mac_result));
        return;
    }

    const std::uint64_t link = net_syscall(std::Syscall::NetLinkStatus);
    printf("[network.manager] ready mac=%02x:%02x:%02x:%02x:%02x:%02x link=%llu\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], static_cast<unsigned long long>(link));
}

}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_str("[network.manager] queue_create failed\n");
        return 1;
    }

    if (std::service_register(std::services::network_manager::NAME, queue) == fail) {
        write_str("[network.manager] service_register failed\n");
        std::close(queue);
        return 1;
    }

    print_device_status();
    if (std::thread_create(watch_config) == fail) {
        write_str("[network.manager] thread_create failed\n");
    }

    // The kernel processes received packets and runs the TCP timers itself,
    // so all that is left here is answering service requests.
    for (;;) {
        std::IPCMessage message = {};
        if (std::queue_receive(queue, &message, true) == fail) {
            _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), 100);
            continue;
        }
        if ((message.flags & std::IPC_MESSAGE_REQUEST) != 0 && !handle_request(queue, message)) {
            write_str("[network.manager] queue_reply failed\n");
        }
    }

    std::close(queue);
    return 0;
}
