#include "vrst/protocol.hpp"
#include <cstring>
#include <limits>
namespace vrst::wire {
namespace {
void put(std::vector<std::uint8_t> &b, std::uint64_t n, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i)
        b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
}
void number(std::vector<std::uint8_t> &b, double d) {
    std::uint64_t n;
    std::memcpy(&n, &d, 8);
    put(b, n, 8);
}
struct Reader {
    const std::uint8_t *p;
    std::size_t n, at{};
    bool ok{true};
    std::uint64_t get(std::size_t bytes) {
        if (bytes > n - at) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bytes; ++i)
            v |= std::uint64_t(p[at++]) << (8 * i);
        return v;
    }
    double number() {
        auto v = get(8);
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
};
} // namespace
std::vector<std::uint8_t> encode(const Message &m) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "IEEE 754 doubles required");
    std::vector<std::uint8_t> b;
    b.reserve(128);
    put(b, 0x54535256, 4);
    put(b, 1, 2);
    put(b, static_cast<std::uint16_t>(m.type), 2);
    put(b, 0, 4);
    if (m.type == Type::pose) {
        auto &p = m.pose;
        put(b, p.session, 8);
        put(b, p.sequence, 8);
        put(b, p.time_ns, 8);
        put(b, p.flags, 4);
        for (double d : {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w,
                         p.projection.left, p.projection.right, p.projection.up, p.projection.down})
            number(b, d);
    } else {
        put(b, m.nonce, 8);
        put(b, m.client_send, 8);
        if (m.type == Type::sync_reply) {
            put(b, m.server_receive, 8);
            put(b, m.server_send, 8);
            put(b, m.session, 8);
        }
    }
    auto n = static_cast<std::uint32_t>(b.size() - 12);
    for (int i = 0; i < 4; ++i)
        b[8 + i] = static_cast<std::uint8_t>(n >> (8 * i));
    return b;
}
std::optional<Message> decode(const std::uint8_t *data, std::size_t n) {
    if (!data || n < 12 || n > 128)
        return std::nullopt;
    Reader r{data, n};
    if (r.get(4) != 0x54535256 || r.get(2) != 1)
        return std::nullopt;
    auto type = r.get(2);
    if (type < 1 || type > 3 || r.get(4) != n - 12)
        return std::nullopt;
    Message m;
    m.type = static_cast<Type>(type);
    if (m.type == Type::pose) {
        if (n != 104)
            return std::nullopt;
        auto &p = m.pose;
        p.session = r.get(8);
        p.sequence = r.get(8);
        p.time_ns = r.get(8);
        p.flags = static_cast<std::uint32_t>(r.get(4));
        p.orientation = {r.number(), r.number(), r.number(), r.number()};
        p.projection = {r.number(), r.number(), r.number(), r.number()};
        if (!p.session || !p.time_ns || (p.flags & ~7U) || !valid(p.orientation) || !valid(p.projection))
            return std::nullopt;
    } else {
        if (n != (m.type == Type::sync_request ? 28U : 52U))
            return std::nullopt;
        m.nonce = r.get(8);
        m.client_send = r.get(8);
        if (m.type == Type::sync_reply) {
            m.server_receive = r.get(8);
            m.server_send = r.get(8);
            m.session = r.get(8);
            if (!m.session || m.server_send < m.server_receive)
                return std::nullopt;
        }
    }
    return r.ok && r.at == n ? std::optional<Message>(m) : std::nullopt;
}
} // namespace vrst::wire
