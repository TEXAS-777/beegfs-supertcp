#pragma once

#include <common/Common.h>
#include <common/toolkit/serialization/Serialization.h>

#include "IPAddress.h"

#include <cstdint>
#include <net/if.h>

enum NicAddrType {
   NICADDRTYPE_STANDARD = 0,
   // removed: NICADDRTYPE_SDP = 1,
   NICADDRTYPE_RDMA = 2,
   NICADDRTYPE_SUPERTCP = 3
};

int findNicPosition(const StringList& preferences, const NicAddress& nic);

/**
 * Returns true if a and b represent the same value.
 */
static inline bool operator==(const struct in_addr a, const struct in_addr b)
{
   return a.s_addr == b.s_addr;
}

/**
 * Returns true if a's value is numerically less-than b's value.
 */
static inline bool operator<(const struct in_addr a, const struct in_addr b)
{
   return a.s_addr < b.s_addr;
}

/**
 * Hash functor for in_addr.
 */
class InAddrHash
{
   public:
      std::size_t operator() (const struct in_addr a) const
      {
         return std::hash<uint32_t>()(a.s_addr);
      }
};

/**
 * Note: Make sure this struct can be copied with the assignment operator.
 */
struct NicAddress
{
   IPAddress      ipAddr;
   NicAddrType    nicType = NicAddrType::NICADDRTYPE_STANDARD;
   char           name[IFNAMSIZ] = "";
   uint8_t        protocol = 4;

   static void serialize(const NicAddress* obj, Serializer& ser)
   {
      ser % obj->protocol;

      if (obj->protocol == 4) {
         in_addr_t ipv4 = obj->ipAddr.toIPv4InAddrT();
         ser.putBlock(&ipv4, sizeof(ipv4));
      } else {
         auto ipv6 = obj->ipAddr.data();
         ser.putBlock(&ipv6, sizeof(ipv6));
      }

      ser.putBlock(obj->name, BEEGFS_MIN(sizeof(obj->name), SERIALIZATION_NICLISTELEM_NAME_SIZE) );
      ser % serdes::as<char>(obj->nicType);
      ser.skip(2); // PADDING
   }

   static void serialize(NicAddress* obj, Deserializer& des)
   {
      static const unsigned minNameSize =
         BEEGFS_MIN(sizeof(obj->name), SERIALIZATION_NICLISTELEM_NAME_SIZE);

      des % obj->protocol;
      if (obj->protocol == 4) {
         // Ipv4 address
         in_addr_t ipv4 {0};
         des.getBlock(&ipv4, sizeof(ipv4));
         obj->ipAddr = IPAddress(ipv4);
      } else {
         std::array<uint8_t, 16> ipv6 {0};
         des.getBlock(ipv6.data(), sizeof(ipv6));
         obj->ipAddr = IPAddress(ipv6);
      }

      des.getBlock(obj->name, minNameSize);
      obj->name[minNameSize - 1] = 0;

      des % serdes::as<char>(obj->nicType);
      des.skip(2); // PADDING
   }

   bool operator==(const NicAddress& o) const
   {
      return ipAddr == o.ipAddr && nicType == o.nicType
         && !strncmp(name, o.name, sizeof(name));
   }
};

typedef struct NicListCapabilities
{
   bool supportsRDMA;
} NicListCapabilities;


class NicAddressList : public std::list<NicAddress>
{

};

typedef NicAddressList::iterator NicAddressListIter;

// Specialization of NicAddressList serializer, forwarding to the generic list serializer
inline serdes::BackedPtrSer<NicAddressList> serdesNicAddressList(NicAddressList* const& ptr, const NicAddressList& backing) {
   return serdes::backedPtr(ptr, backing);
}

struct BackedNicAddressListDes {
   NicAddressList& backing;
   NicAddressList*& ptr;

   BackedNicAddressListDes(NicAddressList*& ptr, NicAddressList& backing) : backing(backing), ptr(ptr) {}

   friend Deserializer& operator%(Deserializer& des, BackedNicAddressListDes value)
   {
      value.backing.clear();

      uint32_t nicListLength;
      des % nicListLength;

      uint32_t nicListCount;
      des % nicListCount;

      for (uint32_t i = 0; i < nicListCount; i++) {
         NicAddress nic;

         des % nic;

         value.backing.push_back(nic);
      }

      value.ptr = &value.backing;
      return des;
   }
};

// Specialization of NicAddressList deserializer, allows skipping ipv6 interfaces while not supported.
inline BackedNicAddressListDes serdesNicAddressList(NicAddressList*& ptr, NicAddressList& backing) {
   BackedNicAddressListDes backed(ptr, backing);
   return backed;
}

// used for debugging
static inline std::string NicAddressList_str(const NicAddressList& nicList)
{
   std::string r;
   char buf[128];
   for (NicAddressList::const_iterator it = nicList.begin(); it != nicList.end(); ++it)
   {
      snprintf(buf, sizeof(buf), "name=%s type=%d addr=%s, ", it->name, it->nicType, it->ipAddr.toString().c_str());
      r += std::string(buf);
   }
   return r;
}

int findNicPosition(const StringList& preferences, const NicAddress& nic);

class NetworkInterfaceCard
{
   public:
      static bool findAll(const StringList* allowedInterfacesList, bool useRDMA, bool disableIPv6, NicAddressList* outList);

      static const char* nicTypeToString(NicAddrType nicType);
      static std::string nicAddrToString(NicAddress* nicAddr);

      static bool supportsRDMA(NicAddressList* nicList);
      static void supportedCapabilities(NicAddressList* nicList,
         NicListCapabilities* outCapabilities);

      static bool checkAndAddRdmaCapability(const StringList& allowedInterfacesList, NicAddressList& nicList);
      static bool checkAndAddSuperTcpCapability(const StringList& allowedInterfacesList, NicAddressList& nicList);

      struct NicAddrComp {
         const StringList* preferences = nullptr;
         explicit NicAddrComp(const StringList* preferences = nullptr) : preferences(preferences) {}
         bool operator()(const NicAddress& lhs, const NicAddress& rhs) const;
      };

   private:
      NetworkInterfaceCard() {}

      static bool fillNicAddress(NicAddrType nicType, struct ifaddrs* ifa,
         NicAddress* outAddr);
      static bool findAllInterfaces(NicAddrType nicType,
         const StringList* allowedInterfacesList, bool disableIPv6, NicAddressList* outList);
      static void filterInterfacesForRDMA(const StringList& allowedInterfacesList, NicAddressList* list,
         NicAddressList* outList);
};
