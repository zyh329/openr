#include "NetlinkTypes.h"
#include "NetlinkException.h"

namespace openr {
namespace fbnl {

Route RouteBuilder::build() const {
  return Route(*this);
}

RouteBuilder& RouteBuilder::setDestination(const folly::CIDRNetwork& dst) {
  dst_ = dst;
  return *this;
}

const folly::CIDRNetwork& RouteBuilder::getDestination() const {
  return dst_;
}

RouteBuilder& RouteBuilder::setType(uint8_t type) {
  type_ = type;
  return *this;
}

uint8_t RouteBuilder::getType() const {
  return type_;
}

RouteBuilder& RouteBuilder::setRouteTable(uint8_t routeTable) {
  routeTable_ = routeTable;
  return *this;
}

uint8_t RouteBuilder::getRouteTable() const {
  return routeTable_;
}

RouteBuilder& RouteBuilder::setProtocolId(uint8_t protocolId) {
  protocolId_ = protocolId;
  return *this;
}

uint8_t RouteBuilder::getProtocolId() const {
  return protocolId_;
}

RouteBuilder& RouteBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

uint8_t RouteBuilder::getScope() const {
  return scope_;
}

 // Optional parameters set after object is constructed
RouteBuilder& RouteBuilder::setFlags(uint32_t flags) {
  flags_ = flags;
  return *this;
}

folly::Optional<uint32_t> RouteBuilder::getFlags() const {
  return flags_;
}

RouteBuilder& RouteBuilder::setPriority(uint32_t priority) {
  priority_ = priority;
  return *this;
}

folly::Optional<uint32_t> RouteBuilder::getPriority() const {
  return priority_;
}

RouteBuilder& RouteBuilder::setTos(uint8_t tos) {
  tos_ = tos;
  return *this;
}

folly::Optional<uint8_t> RouteBuilder::getTos() const {
  return tos_;
}

RouteBuilder& RouteBuilder::addNextHop(const NextHop& nextHop) {
  nextHops_.push_back(nextHop);
  return *this;
}

const std::vector<NextHop>&
RouteBuilder::getNextHops() const {
  return nextHops_;
}

Route::Route(const RouteBuilder& builder)
  : type_(builder.getType()),
    routeTable_(builder.getRouteTable()),
    protocolId_(builder.getProtocolId()),
    scope_(builder.getScope()),
    flags_(builder.getFlags()),
    priority_(builder.getPriority()),
    tos_(builder.getTos()),
    nextHops_(builder.getNextHops()),
    dst_ (builder.getDestination()) {
  init();
}

Route::~Route() {
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
}

Route::Route(Route&& other) noexcept
  : type_(other.type_),
    routeTable_(other.routeTable_),
    protocolId_(other.protocolId_),
    scope_(other.scope_),
    flags_(other.flags_),
    priority_(other.priority_),
    tos_(other.tos_),
    nextHops_(other.nextHops_),
    dst_ (other.dst_) {
  if (other.route_) {
    // prevent double release
    route_ = other.route_;
    other.route_ = nullptr;
  }
}

Route& Route::operator=(Route&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  type_ = other.type_;
  routeTable_ = other.routeTable_;
  protocolId_ = other.protocolId_;
  scope_ = other.scope_;
  flags_ = other.flags_;
  priority_ = other.priority_;
  tos_ = other.tos_;
  nextHops_ = other.nextHops_;
  dst_ = other.dst_;
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
  if (other.route_) {
    route_ = other.route_;
    other.route_ = nullptr;
  }
  return *this;
}

uint8_t Route::getFamily() const {
  return dst_.first.family();
}

uint8_t Route::getType() const {
  return type_;
}

const folly::CIDRNetwork& Route::getDestination() const {
  return dst_;
}

folly::Optional<uint8_t> Route::getTos() const {
  return tos_;
}

uint8_t Route::getRouteTable() const {
  return routeTable_;
}

uint8_t Route::getProtocolId() const {
  return protocolId_;
}

uint8_t Route::getScope() const {
  return scope_;
}

folly::Optional<uint32_t> Route::getFlags() const {
  return flags_;
}

folly::Optional<uint32_t> Route::getPriority() const {
  return priority_;
}

const std::vector<NextHop>&
Route::getNextHops() const {
  return nextHops_;
}

struct rtnl_route* Route::fromNetlinkRoute() const {
  return route_;
}

void Route::init() {
  VLOG(4) << "Creating route object";

  // Only build object once
  if (route_) {
    return;
  }
  route_ = rtnl_route_alloc();
  if (route_ == nullptr) {
    throw NetlinkException("Cannot allocate route object");
  }

  SCOPE_FAIL {
    rtnl_route_put(route_);
    route_ = nullptr;
  };

  rtnl_route_set_scope(route_, scope_);
  rtnl_route_set_type(route_, type_);
  rtnl_route_set_family(route_, dst_.first.family());
  rtnl_route_set_table(route_, routeTable_);
  rtnl_route_set_protocol(route_, protocolId_);

  if (priority_.hasValue()) {
    rtnl_route_set_priority(route_, priority_.value());
  }

  if (flags_.hasValue()) {
    rtnl_route_set_flags(route_, flags_.value());
  }

  if (tos_.hasValue()) {
    rtnl_route_set_tos(route_, tos_.value());
  }

  // Set destination
  struct nl_addr* nlAddr = buildAddrObject(dst_);
  // route object takes a ref if dst is successfully set
  // so we should always drop our ref, success or failure
  SCOPE_EXIT {
    nl_addr_put(nlAddr);
  };
  int err = rtnl_route_set_dst(route_, nlAddr);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to set dst for route {} : {}",
        folly::IPAddress::networkToString(dst_),
        nl_geterror(err)));
  }

  if (nextHops_.empty()) {
    return;
  }
  // Add next hops
  // 1. check dst and nexthop's family
  for (const auto& nextHop : nextHops_) {
    auto gateway = nextHop.getGateway();
    if (gateway.hasValue()
     && gateway.value().family() != dst_.first.family()) {
      throw NetlinkException(
        "Different address family for destination and Nexthop gateway");
    }
  }
  // 2. build nexthop and add it to route
  for (auto nextHop : nextHops_) {
    struct rtnl_nexthop* nh = nextHop.fromNetlinkNextHop();
    rtnl_route_add_nexthop(route_, nh);
  }
}

struct nl_addr* Route::buildAddrObject(const folly::CIDRNetwork& addr) {
  struct nl_addr* nlAddr_ = nl_addr_build(
      addr.first.family(),
      (void*)(addr.first.bytes()),
      addr.first.byteCount());
  if (nlAddr_ == nullptr) {
    throw NetlinkException("Failed to create nl addr");
  }
  nl_addr_set_prefixlen(nlAddr_, addr.second);
  return nlAddr_;
}

/*=================================NextHop====================================*/

NextHop NextHopBuilder::build() const {
  return NextHop(*this);
}

void NextHopBuilder::reset() {
  ifIndex_.clear();
  weight_.clear();
  gateway_.clear();
}

NextHopBuilder& NextHopBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

NextHopBuilder&
NextHopBuilder::setGateway(const folly::IPAddress& gateway) {
  gateway_ = gateway;
  return *this;
}

NextHopBuilder& NextHopBuilder::setWeight(uint8_t weight) {
  weight_ = weight;
  return *this;
}

folly::Optional<int> NextHopBuilder::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::IPAddress> NextHopBuilder::getGateway() const {
  return gateway_;
}

folly::Optional<uint8_t> NextHopBuilder::getWeight() const {
  return weight_;
}

NextHop::NextHop(const NextHopBuilder& builder)
  : ifIndex_(builder.getIfIndex()),
    gateway_(builder.getGateway()),
    weight_(builder.getWeight()) {
  init();
}

folly::Optional<int> NextHop::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::IPAddress> NextHop::getGateway() const {
  return gateway_;
}

folly::Optional<uint8_t> NextHop::getWeight() const {
  return weight_;
}

void NextHop::init() {
  if (nextHop_) {
    return;
  }
  if (ifIndex_.hasValue() && gateway_.hasValue()) {
    nextHop_ = buildNextHopInternal(ifIndex_.value(), gateway_.value());
  } else if (ifIndex_.hasValue()) {
    nextHop_ = buildNextHopInternal(ifIndex_.value());
  } else if (gateway_.hasValue()) {
    nextHop_ = buildNextHopInternal(gateway_.value());
  }
}

struct rtnl_nexthop* NextHop::fromNetlinkNextHop() const {
  return nextHop_;
}

void NextHop::release() {
  if (nextHop_) {
    rtnl_route_nh_free(nextHop_);
    nextHop_ = nullptr;
  }
}

struct rtnl_nexthop* NextHop::buildNextHopInternal(const int ifIdx) {
  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw NetlinkException("Failed to create nextHop");
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }
  rtnl_route_nh_set_ifindex(nextHop, ifIdx);
  return nextHop;
}

struct rtnl_nexthop* NextHop::buildNextHopInternal(
  int ifIdx, const folly::IPAddress& gateway) {
  struct nl_addr* nlGateway = nl_addr_build(
      gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());

  if (nlGateway == nullptr) {
    throw NetlinkException("Failed to create nl addr for gateway");
  }

  // nextHop object takes a ref if gateway is successfully set
  // Either way, success or failure, we drop our ref
  SCOPE_EXIT {
    nl_addr_put(nlGateway);
  };

  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw NetlinkException("Failed to create nextHop");
  }

  if (gateway.isV4()) {
    rtnl_route_nh_set_flags(nextHop, RTNH_F_ONLINK);
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }

  rtnl_route_nh_set_ifindex(nextHop, ifIdx);
  rtnl_route_nh_set_gateway(nextHop, nlGateway);
  return nextHop;
}

// build nexthop with nexthop = global ip addresses
struct rtnl_nexthop* NextHop::buildNextHopInternal(
  const folly::IPAddress& gateway) {
  if (gateway.isLinkLocal()) {
    throw NetlinkException(folly::sformat(
        "Failed to resolve interface name for link local address {}",
        gateway.str()));
  }

  struct nl_addr* nlGateway = nl_addr_build(
      gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());

  if (nlGateway == nullptr) {
    throw NetlinkException("Failed to create nl addr for gateway");
  }

  // nextHop object takes a ref if gateway is successfully set
  // Either way, success or failure, we drop our ref
  SCOPE_EXIT {
    nl_addr_put(nlGateway);
  };

  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw NetlinkException("Failed to create nextHop");
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }
  rtnl_route_nh_set_gateway(nextHop, nlGateway);
  return nextHop;
}

/*================================IfAddress===================================*/

IfAddress IfAddressBuilder::build() {
  return IfAddress(*this);
}

IfAddressBuilder& IfAddressBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int IfAddressBuilder::getIfIndex() const {
  return ifIndex_;
}

IfAddressBuilder&
IfAddressBuilder::setPrefix(const folly::CIDRNetwork& prefix) {
  prefix_ = prefix;
  return *this;
}

folly::Optional<folly::CIDRNetwork> IfAddressBuilder::getPrefix() const {
  return prefix_;
}

IfAddressBuilder& IfAddressBuilder::setFamily(uint8_t family) {
  family_ = family;
  return *this;
}

// Family will be shadowed if prefix is set
folly::Optional<uint8_t> IfAddressBuilder::getFamily() const {
  return family_;
}

IfAddressBuilder& IfAddressBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

folly::Optional<uint8_t> IfAddressBuilder::getScope() const {
  return scope_;
}

IfAddressBuilder& IfAddressBuilder::setFlags(uint8_t flags) {
  flags_ = flags;
  return *this;
}

folly::Optional<uint8_t> IfAddressBuilder::getFlags() const {
  return flags_;
}

void IfAddressBuilder::reset() {
  ifIndex_ = 0;
  prefix_.clear();
  scope_.clear();
  flags_.clear();
  family_.clear();
}

IfAddress::IfAddress(IfAddressBuilder& builder)
  : prefix_(builder.getPrefix()),
    ifIndex_(builder.getIfIndex()),
    scope_(builder.getScope()),
    flags_(builder.getFlags()),
    family_(builder.getFamily()) {
  init();
}

IfAddress::~IfAddress() {
  if (ifAddr_) {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  }
}

IfAddress::IfAddress(IfAddress&& other) noexcept
  : prefix_(other.prefix_),
    ifIndex_(other.ifIndex_),
    scope_(other.scope_),
    flags_(other.flags_),
    family_(other.family_) {
  if (other.ifAddr_) {
    ifAddr_ = other.ifAddr_;
    other.ifAddr_ = nullptr;
  }
}

IfAddress& IfAddress::operator=(IfAddress&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  prefix_ = other.prefix_;
  ifIndex_ = other.ifIndex_;
  scope_ = other.scope_;
  flags_ = other.flags_;
  family_ = other.family_;
  // release old object
  if (ifAddr_) {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  }
  if (other.ifAddr_) {
    ifAddr_ = other.ifAddr_;
    other.ifAddr_ = nullptr;
  }
  return *this;
}

uint8_t IfAddress::getFamily() const {
  if (prefix_.hasValue()) {
    return prefix_->first.family();
  } else {
    return family_.value();
  }
}

uint8_t IfAddress::getPrefixLen() const {
  if (prefix_.hasValue()) {
    return prefix_->second;
  }
  return 0;
}

int IfAddress::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::CIDRNetwork> IfAddress::getPrefix() const {
  return prefix_;
}

folly::Optional<uint8_t> IfAddress::getScope() const {
  return scope_;
}

folly::Optional<uint8_t> IfAddress::getFlags() const {
  return flags_;
}

// Will construct rtnl_addr object on the first time call, then will return
// the same object pointer
struct rtnl_addr* IfAddress::fromIfAddress() const {
  return ifAddr_;
}

void IfAddress::init() {
  if (ifAddr_) {
    return;
  }

  ifAddr_ = rtnl_addr_alloc();
  if (nullptr == ifAddr_) {
    throw NetlinkException("Failed to create rtnl_addr object");
  }
  rtnl_addr_set_ifindex(ifAddr_, ifIndex_);

  // Get local addr
  struct nl_addr* localAddr = nullptr;
  if (prefix_.hasValue()) {
    localAddr = nl_addr_build(
      prefix_->first.family(),
      (void*)(prefix_->first.bytes()),
      prefix_->first.byteCount());
    if (nullptr == localAddr) {
      throw NetlinkException("Failed to create local addr");
    }
    nl_addr_set_prefixlen(localAddr, prefix_->second);
    // Setting the local address will automatically set the address family
    // and the prefix length to the correct values.
    rtnl_addr_set_local(ifAddr_, localAddr);
  }

  // rtnl_addr_set_local will increase reference for localAddr
  SCOPE_EXIT {
    if (localAddr) {
      nl_addr_put(localAddr);
    }
  };

  SCOPE_FAIL {
    if (localAddr) {
      nl_addr_put(localAddr);
    }
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  };

  if (family_.hasValue()) {
    rtnl_addr_set_family(ifAddr_, family_.value());
  }
  if (scope_.hasValue()) {
    rtnl_addr_set_scope(ifAddr_, scope_.value());
  }
  if (flags_.hasValue()) {
    rtnl_addr_set_flags(ifAddr_, flags_.value());
  }
}

} // namespace fbnl
} // namespace openr
