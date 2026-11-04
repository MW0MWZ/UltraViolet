// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "net/guest_names.h"

#include <algorithm>
#include <cctype>

namespace uv::net {

namespace {

constexpr std::size_t kEthernet = 14;
constexpr u16 kTypeA = 1, kTypePtr = 12, kTypeAny = 255, kClassIn = 1;
constexpr u32 kTtl = 300;

u16 be16(const u8* p) { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
void put16(std::vector<u8>& v, u16 x) { v.insert(v.end(), {static_cast<u8>(x >> 8), static_cast<u8>(x)}); }
void put32(std::vector<u8>& v, u32 x) {
    v.insert(v.end(), {static_cast<u8>(x >> 24), static_cast<u8>(x >> 16), static_cast<u8>(x >> 8), static_cast<u8>(x)});
}
void set16(std::vector<u8>& v, std::size_t at, u16 x) { v[at] = static_cast<u8>(x >> 8), v[at + 1] = static_cast<u8>(x); }

bool same(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// A query's name (no compression in a question), from `at`; false if it is
// malformed or runs off the end.
bool read_name(const u8* p, std::size_t n, std::size_t& at, std::vector<std::string>& labels) {
    std::size_t total = 0;
    for (;;) {
        if (at >= n) return false;
        const u8 len = p[at++];
        if (len == 0) return true;
        if (len > 63 || at + len > n || (total += len + 1u) > 255) return false;
        labels.emplace_back(reinterpret_cast<const char*>(p + at), len);
        at += len;
    }
}

void put_name(std::vector<u8>& v, const std::string& dotted) {
    std::size_t from = 0;
    while (from < dotted.size()) {
        const std::size_t dot = std::min(dotted.find('.', from), dotted.size());
        v.push_back(static_cast<u8>(dot - from));
        v.insert(v.end(), dotted.begin() + static_cast<std::ptrdiff_t>(from), dotted.begin() + static_cast<std::ptrdiff_t>(dot));
        from = dot + 1;
    }
    v.push_back(0);
}

// The Internet checksum of `n` bytes, on top of `sum`.
u16 checksum(const u8* p, std::size_t n, u32 sum = 0) {
    for (std::size_t i = 0; i + 1 < n; i += 2) sum += be16(p + i);
    if (n & 1) sum += u32{p[n - 1]} << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}

bool valid_label(const std::string& s) {
    return !s.empty() && s.size() <= 63 && std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
    });
}

}  // namespace

GuestNames::GuestNames(std::string name, std::string domain, u32 nameserver)
    : names_{std::move(name)}, domain_(std::move(domain)), nameserver_(nameserver) {
    for (std::size_t from = 0; from < domain_.size();) {
        const std::size_t dot = std::min(domain_.find('.', from), domain_.size());
        if (dot > from) zone_.push_back(domain_.substr(from, dot - from));
        from = dot + 1;
    }
}

bool GuestNames::is_ours(const std::string& label) const {
    return std::any_of(names_.begin(), names_.end(), [&](const std::string& n) { return same(n, label); });
}

// The name, or one under it, is in DHCP's domain (none if it gives none).
bool GuestNames::in_zone(const std::vector<std::string>& labels) const {
    if (zone_.empty() || labels.size() < zone_.size()) return false;
    return std::equal(zone_.begin(), zone_.end(), labels.end() - static_cast<std::ptrdiff_t>(zone_.size()), same);
}

// A BOOTP request's options (after the 236-byte header and the magic
// cookie): the host name the client asks for (option 12).
void GuestNames::learn_dhcp(const u8* p, std::size_t n) {
    if (n < 240 || p[0] != 1 || be32(p + 236) != 0x63825363) return;
    for (std::size_t at = 240; at < n;) {
        const u8 code = p[at++];
        if (code == 0) continue;
        if (code == 255 || at >= n) return;
        const std::size_t len = p[at++];
        if (at + len > n) return;
        if (code == 12) {
            std::string name(reinterpret_cast<const char*>(p + at), len);
            name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
            if (const std::size_t dot = name.find('.'); dot != std::string::npos) name.resize(dot);
            if (valid_label(name)) {
                asked_ = name;
                if (!is_ours(name)) names_.push_back(name);
            }
        }
        at += len;
    }
}

bool GuestNames::answer(const u8* frame, std::size_t length, std::vector<u8>& reply) {
    if (length < kEthernet + 28 || be16(frame + 12) != 0x0800) return false;
    const u8* ip = frame + kEthernet;
    const std::size_t ihl = (ip[0] & 15u) * 4, total = be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl + 8 || total > length - kEthernet || ip[9] != 17 ||
        (be16(ip + 6) & 0x3FFF) != 0)  // UDP, not fragmented
        return false;
    const u8* udp = ip + ihl;
    const std::size_t udp_len = be16(udp + 4);
    if (udp_len < 8 || udp_len > total - ihl) return false;
    const u8* dns = udp + 8;
    const std::size_t n = udp_len - 8;
    if (be16(udp + 2) == 67) {
        learn_dhcp(dns, n);
        return false;
    }
    if (be16(udp + 2) != 53 || be32(ip + 16) != nameserver_ || n < 12) return false;

    // A standard query for one name (EDNS's record, if any, is not echoed).
    const u16 flags = be16(dns + 2);
    if ((flags & 0x8000) || ((flags >> 11) & 15) != 0 || be16(dns + 4) != 1 || be16(dns + 6) != 0 ||
        be16(dns + 8) != 0 || be16(dns + 10) > 1)
        return false;
    std::size_t at = 12;
    std::vector<std::string> labels;
    if (!read_name(dns, n, at, labels) || at + 4 > n) return false;
    const u16 qtype = be16(dns + at), qclass = be16(dns + at + 2);
    const std::size_t question_end = at + 4;
    if (qclass != kClassIn) return false;

    const u32 guest = be32(ip + 12);
    std::vector<u8> rdata;
    u16 rtype = 0, rcode = 0;
    std::string asked;
    for (std::size_t i = 0; i < labels.size(); ++i) asked += (i ? "." : "") + labels[i];
    // The guest's name, here or in the domain it asks in; or no name at all,
    // the name of a host that does not know its own yet (Solaris's installer
    // checks its name server so, with the name DHCP gave it not yet set).
    if (labels.empty() ? qtype == kTypeA : is_ours(labels[0])) {
        if (labels.size() > 1) {
            domain_.clear();
            for (std::size_t i = 1; i < labels.size(); ++i) domain_ += (i > 1 ? "." : "") + labels[i];
        }
        if (qtype == kTypeA || qtype == kTypeAny) {
            rtype = kTypeA;
            put32(rdata, guest);
        }
    } else if (!labels.empty() && same(labels[0], "localhost") && (labels.size() == 1 || (labels.size() == zone_.size() + 1 && in_zone(labels)))) {
        // localhost, bare or in DHCP's domain: the loopback address (RFC 6761).
        if (qtype == kTypeA || qtype == kTypeAny) {
            rtype = kTypeA;
            put32(rdata, 0x7F000001);
        }
    } else if (in_zone(labels)) {
        // DHCP's domain is this network's own, as a home router's is: it
        // has no other names, which is said at once (the host's resolver
        // has no say in it, and may be slow to say so or never answer).
        if (labels.size() > zone_.size()) rcode = 3;  // no such name
    } else if (same(asked, "1.0.0.127.in-addr.arpa")) {
        if (qtype == kTypePtr || qtype == kTypeAny) {
            rtype = kTypePtr;
            put_name(rdata, "localhost");
        }
    } else {
        // Its own address, backwards under in-addr.arpa.
        const std::string reverse = std::to_string(guest & 0xFF) + "." + std::to_string(guest >> 8 & 0xFF) + "." +
                                    std::to_string(guest >> 16 & 0xFF) + "." + std::to_string(guest >> 24);
        if (!same(asked, reverse + ".in-addr.arpa")) return false;
        if (qtype == kTypePtr || qtype == kTypeAny) {
            rtype = kTypePtr;
            const std::string& name = asked_.empty() ? names_.front() : asked_;
            put_name(rdata, domain_.empty() ? name : name + "." + domain_);
        }
    }

    // The answer (or none, for a type the name does not have, or a name
    // that is not there), from the
    // address and port asked, to where it came from.
    reply.clear();
    reply.insert(reply.end(), frame + 6, frame + 12);  // to the sender's address
    reply.insert(reply.end(), frame, frame + 6);       // from the one it sent to
    put16(reply, 0x0800);
    const std::size_t ip_at = reply.size();
    reply.insert(reply.end(), {0x45, 0x00, 0, 0});
    put16(reply, ip_id_++);
    reply.insert(reply.end(), {0x00, 0x00, 64, 17, 0, 0});
    reply.insert(reply.end(), ip + 16, ip + 20);
    reply.insert(reply.end(), ip + 12, ip + 16);
    const std::size_t udp_at = reply.size();
    put16(reply, 53);
    reply.insert(reply.end(), udp, udp + 2);
    reply.insert(reply.end(), {0, 0, 0, 0});
    reply.insert(reply.end(), dns, dns + 2);                           // ID
    put16(reply, static_cast<u16>(0x8480 | (flags & 0x0100) | rcode));  // response, authoritative, RD as asked, RA
    put16(reply, 1);
    put16(reply, rtype ? 1 : 0);
    put16(reply, 0);
    put16(reply, 0);
    reply.insert(reply.end(), dns + 12, dns + question_end);
    if (rtype) {
        put16(reply, 0xC00C);  // the name asked
        put16(reply, rtype);
        put16(reply, kClassIn);
        put32(reply, kTtl);
        put16(reply, static_cast<u16>(rdata.size()));
        reply.insert(reply.end(), rdata.begin(), rdata.end());
    }
    const std::size_t udp_total = reply.size() - udp_at;
    set16(reply, ip_at + 2, static_cast<u16>(reply.size() - ip_at));
    set16(reply, ip_at + 10, checksum(reply.data() + ip_at, 20));
    set16(reply, udp_at + 4, static_cast<u16>(udp_total));
    u32 pseudo = be16(ip + 16) + be16(ip + 18) + be16(ip + 12) + be16(ip + 14) + 17 + static_cast<u32>(udp_total);
    const u16 sum = checksum(reply.data() + udp_at, udp_total, pseudo);
    set16(reply, udp_at + 6, sum ? sum : 0xFFFF);
    return true;
}

}  // namespace uv::net
