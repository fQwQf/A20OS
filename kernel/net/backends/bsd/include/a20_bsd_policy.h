#ifndef A20_BSD_POLICY_H
#define A20_BSD_POLICY_H

#ifdef A20_BSD_ROUTER_POLICY
struct in_addr;
struct in6_addr;
struct ip6_pktopts;
struct nhop_object;

unsigned port_policy_fib4(const struct in_addr *, const struct in_addr *,
                           unsigned base_fib, uint32_t flowid);
unsigned port_policy_fib6(const struct in6_addr *, const struct in6_addr *,
                           unsigned base_fib, uint32_t flowid);
bool port_policy_explicit_if6(const struct ip6_pktopts *);
bool port_policy_cache_in_fib(const struct nhop_object *, unsigned fib);
#endif
#endif
