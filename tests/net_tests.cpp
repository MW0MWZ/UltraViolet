// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The user-mode network's own names: the guest's name answered in any
// domain, its address answered back, a name asked of DHCP learnt, and
// every other query left for the host's resolver.
#include <algorithm>
#include <string>
#include <vector>

#include "net/guest_names.h"
#include "test.h"

using namespace uv;

namespace {

constexpr u32 kGuest = 0x0A00020F, kDns = 0x0A000203;

void put16(std::vector<u8>& v, unsigned x) { v.insert(v.end(), {static_cast<u8>(x >> 8), static_cast<u8>(x)}); }
void put32(std::vector<u8>& v, u32 x) { put16(v, x >> 16), put16(v, x & 0xFFFF); }
unsigned be16(const std::vector<u8>& v, std::size_t at) { return unsigned{v[at]} << 8 | v[at + 1]; }

// An Ethernet frame carrying UDP from `src` to `dst`.
std::vector<u8> udp_frame(u32 src, u32 dst, unsigned sport, unsigned dport, const std::vector<u8>& payload) {
    std::vector<u8> f = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02, 0x08, 0x00, 0x20, 0x55, 0x56, 0x01, 0x08, 0x00};
    f.insert(f.end(), {0x45, 0x00});
    put16(f, 28 + payload.size());
    f.insert(f.end(), {0x12, 0x34, 0x00, 0x00, 0xFF, 17, 0x00, 0x00});
    put32(f, src);
    put32(f, dst);
    put16(f, sport);
    put16(f, dport);
    put16(f, 8 + payload.size());
    put16(f, 0);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<u8> query(const std::string& name, unsigned type, u32 from = kGuest, u32 to = kDns) {
    std::vector<u8> q;
    put16(q, 0xBEEF);
    put16(q, 0x0100);  // recursion desired
    put16(q, 1), put16(q, 0), put16(q, 0), put16(q, 0);
    std::size_t from_at = name.empty() ? 1 : 0;  // (the root: no labels)
    while (from_at <= name.size()) {
        const std::size_t dot = std::min(name.find('.', from_at), name.size());
        q.push_back(static_cast<u8>(dot - from_at));
        q.insert(q.end(), name.begin() + static_cast<long>(from_at), name.begin() + static_cast<long>(dot));
        from_at = dot + 1;
    }
    q.push_back(0);
    put16(q, type);
    put16(q, 1);
    return udp_frame(from, to, 32768, 53, q);
}

// A DHCP request asking for `name` (option 12).
std::vector<u8> dhcp_request(const std::string& name) {
    std::vector<u8> b(236, 0);
    b[0] = 1, b[1] = 1, b[2] = 6;
    put32(b, 0x63825363);
    b.insert(b.end(), {53, 1, 3, 12, static_cast<u8>(name.size())});
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(255);
    return udp_frame(0, 0xFFFFFFFF, 68, 67, b);
}

u16 sum(const std::vector<u8>& v, std::size_t at, std::size_t n, u32 s = 0) {
    for (std::size_t i = 0; i + 1 < n; i += 2) s += be16(v, at + i);
    if (n & 1) s += unsigned{v[at + n - 1]} << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return static_cast<u16>(s);
}

// The answer's record data (after the question), or empty if none.
std::vector<u8> answer_data(const std::vector<u8>& r) {
    const std::size_t dns = 14 + 20 + 8;
    if (be16(r, dns + 6) == 0) return {};
    std::size_t at = dns + 12;
    while (r[at]) at += r[at] + 1u;
    at += 5 + 2 + 2 + 2 + 4;  // the question's end, the pointer, type, class, TTL
    const std::size_t n = be16(r, at);
    return {r.begin() + static_cast<long>(at + 2), r.begin() + static_cast<long>(at + 2 + n)};
}

}  // namespace

TEST(guest_names_answers_the_guests_name_in_any_domain) {
    net::GuestNames names("UltraViolet", "home.arpa", kDns);
    std::vector<u8> r;
    const auto q = query("UltraViolet.oakfield.invalid", 1);
    CHECK(names.answer(q.data(), q.size(), r));
    // Back to the sender, from the address it asked; checksums good.
    CHECK(std::equal(r.begin(), r.begin() + 6, q.begin() + 6));
    CHECK(std::equal(r.begin() + 6, r.begin() + 12, q.begin()));
    CHECK_EQ(be16(r, 14 + 12), 0x0A00u);
    CHECK_EQ(be16(r, 14 + 18), 0x020Fu);
    CHECK_EQ(sum(r, 14, 20), 0xFFFFu);
    const std::size_t udp_len = be16(r, 34 + 4);
    CHECK_EQ(sum(r, 34, udp_len, 0x0A00 + 0x0203 + 0x0A00 + 0x020F + 17 + static_cast<u32>(udp_len)), 0xFFFFu);
    CHECK_EQ(be16(r, 34), 53u);
    CHECK_EQ(be16(r, 36), 32768u);
    // ID kept, an authoritative answer, recursion as asked, one A record.
    CHECK_EQ(be16(r, 42), 0xBEEFu);
    CHECK_EQ(be16(r, 44), 0x8580u);
    CHECK_EQ(be16(r, 48), 1u);
    CHECK((answer_data(r) == std::vector<u8>{10, 0, 2, 15}));
    // Bare, in other letters, and in DHCP's domain too.
    for (const char* name : {"ultraviolet", "ULTRAVIOLET.home.arpa", "UltraViolet.example.com"}) {
        const auto b = query(name, 1);
        CHECK(names.answer(b.data(), b.size(), r));
        CHECK((answer_data(r) == std::vector<u8>{10, 0, 2, 15}));
    }
    // No IPv6 address: an answer with no record, at once.
    const auto aaaa = query("UltraViolet.oakfield.invalid", 28);
    CHECK(names.answer(aaaa.data(), aaaa.size(), r));
    CHECK_EQ(be16(r, 44) & 15, 0u);
    CHECK_EQ(be16(r, 48), 0u);
}

TEST(guest_names_answers_a_host_that_has_no_name_yet) {
    // Solaris's installer checks its name server by looking up its own name
    // before it has set the one DHCP gave: an address query for the root.
    net::GuestNames names("UltraViolet", "home.arpa", kDns);
    std::vector<u8> r;
    const auto root = query("", 1);
    CHECK(names.answer(root.data(), root.size(), r));
    CHECK((answer_data(r) == std::vector<u8>{10, 0, 2, 15}));
    // Other questions about the root are the resolver's.
    const auto ns = query("", 2);
    CHECK(!names.answer(ns.data(), ns.size(), r));
}

TEST(guest_names_answers_the_guests_address_with_its_name) {
    net::GuestNames names("UltraViolet", "home.arpa", kDns);
    std::vector<u8> r;
    const auto ptr = query("15.2.0.10.in-addr.arpa", 12);
    CHECK(names.answer(ptr.data(), ptr.size(), r));
    const std::vector<u8> home = {11, 'U', 'l', 't', 'r', 'a', 'V', 'i', 'o', 'l', 'e', 't', 4, 'h', 'o', 'm', 'e', 4, 'a', 'r', 'p', 'a', 0};
    CHECK(answer_data(r) == home);
    // In the domain it last asked its name in.
    const auto a = query("UltraViolet.oakfield.invalid", 1);
    CHECK(names.answer(a.data(), a.size(), r));
    CHECK(names.answer(ptr.data(), ptr.size(), r));
    const auto d = answer_data(r);
    CHECK_EQ(d.size(), 30u);
    CHECK_EQ(d[12], 8u);  // "oakfield"
    // Another address is not ours.
    const auto other = query("16.2.0.10.in-addr.arpa", 12);
    CHECK(!names.answer(other.data(), other.size(), r));
}

TEST(guest_names_learns_the_name_asked_of_dhcp) {
    net::GuestNames names("UltraViolet", "home.arpa", kDns);
    std::vector<u8> r;
    const auto dhcp = dhcp_request("sparcbox");
    CHECK(!names.answer(dhcp.data(), dhcp.size(), r));  // (it goes on to DHCP)
    const auto a = query("sparcbox.example.com", 1, 0x0A000214);
    CHECK(names.answer(a.data(), a.size(), r));
    CHECK((answer_data(r) == std::vector<u8>{10, 0, 2, 20}));  // the address it asks from
    const auto ptr = query("20.2.0.10.in-addr.arpa", 12, 0x0A000214);
    CHECK(names.answer(ptr.data(), ptr.size(), r));
    CHECK_EQ(answer_data(r)[0], 8u);  // "sparcbox"
    CHECK_EQ(answer_data(r)[1], u8{'s'});
}

TEST(guest_names_leaves_other_queries_to_the_resolver) {
    net::GuestNames names("UltraViolet", "home.arpa", kDns);
    std::vector<u8> r;
    for (const auto& f : {query("www.example.com", 1), query("UltraViolet.example.com", 1, kGuest, 0x08080808),
                          query("example.com", 12)}) {
        CHECK(!names.answer(f.data(), f.size(), r));
    }
    // A response, or a short frame, is not a query.
    auto resp = query("UltraViolet", 1);
    resp[14 + 20 + 8 + 2] |= 0x80;
    CHECK(!names.answer(resp.data(), resp.size(), r));
    const auto q = query("UltraViolet", 1);
    CHECK(!names.answer(q.data(), 40, r));
}
