// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The user-mode network's own names, as a DHCP server's DNS does for its
// leases: the guest's name (the one DHCP gives it, and any it asks DHCP for)
// resolves to the address it asks from, in whatever domain it asks (bare,
// the domain DHCP gives, or one typed into an installer), and that address
// back to the name; an address query for no name at all (the root), which a
// host that has not yet set the name DHCP gave it sends for its own, is
// answered the same way. DHCP's domain is the network's own: localhost in
// it (or bare) is the loopback address, and it has no other names, so a
// query for any other is answered at once that there is no such name. Every
// other query goes on to the host's resolver.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/types.h"

namespace uv::net {

class GuestNames {
public:
    // `name` is the one DHCP gives, `domain` the domain it gives (empty for
    // none), `nameserver` the DNS server's address (host order).
    GuestNames(std::string name, std::string domain, u32 nameserver);

    // A frame from the guest. A DHCP request's host name (option 12) is
    // learnt; a DNS query for one of the guest's names, its own address,
    // localhost or a name in DHCP's domain is answered in `reply` (true):
    // that frame goes no further.
    bool answer(const u8* frame, std::size_t length, std::vector<u8>& reply);

private:
    bool is_ours(const std::string& label) const;
    bool in_zone(const std::vector<std::string>& labels) const;
    void learn_dhcp(const u8* p, std::size_t n);

    std::vector<std::string> names_;  // the first is DHCP's
    std::string asked_;               // the name the guest last asked DHCP for (it calls itself)
    std::string domain_;              // the domain last asked in (else DHCP's)
    std::vector<std::string> zone_;   // DHCP's domain, its labels
    u32 nameserver_;
    u16 ip_id_ = 0;
};

}  // namespace uv::net
