#include "NetworkInterfaceCard.h"

#include <common/toolkit/ListTk.h>
#include <common/toolkit/StringTk.h>
#include <common/app/log/LogContext.h>
#include <common/system/System.h>
#include "RDMASocket.h"
#include <memory>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#define IFRSIZE(numIfcReqs)      ((int)((numIfcReqs) * sizeof(struct ifreq) ) )

// Finds the position in the nic preference list (e.g. connInterfacesList), returns -1 if not found
// or blacklisted
int findNicPosition(const StringList& preferences, const NicAddress& nic) {
   auto matches = [&](unsigned col, const std::string& value) -> bool {
      switch(col) {
         case 0:
            // Special case of interface being named "*", needs to be escaped: `\*`
            if(value == "\\*" && std::string_view("*") == nic.name)
               return true;
            // Special case of interface being named "!", needs to be escaped: `\!`
            if(value == "\\!" && std::string_view("!") == nic.name)
               return true;
            return value == "*" || value == nic.name;

         case 1:
            return value == "*" || value == nic.ipAddr.toString();

         case 2:
            if(value == "*")
               return true;
            else if(nic.ipAddr.isIPv6() && value == "6")
               return true;
            else if (nic.ipAddr.isIPv4() && value == "4")
               return true;
            else
               return false;

         case 3:
            if(value == "*")
               return true;
            else if(nic.nicType == NICADDRTYPE_RDMA && value == "rdma")
               return true;
            else if (nic.nicType == NICADDRTYPE_STANDARD && value == "tcp")
               return true;
            else if (nic.nicType == NICADDRTYPE_SUPERTCP && value == "supertcp")
               return true;
            else
               return false;
         default:
            // Any potential extra arguments are ignored if '*' but fail on anything else
            return value == "*";
      }
   };

   std::vector<std::string> split;
   int pos = -1;

   for (const auto& row : preferences) {
      pos++;

      split.clear();
      StringTk::explode(row, ' ', &split);

      bool match = true;
      // First parameter being '!' means anything that matches the row is blacklisted
      if(split[0] == "!") {
         for(unsigned col = 1; col < split.size(); col++) {
            if(!matches(col - 1, split[col])) {
               match = false;
               break;
            }
         }
         if(match)
            return -1;
      // Otherwise, anything that matches the row is whitelisted and put into that position
      } else {
         for(unsigned col = 0; col < split.size(); col++) {
            if(!matches(col, split[col])) {
               match = false;
               break;
            }
         }
         if(match)
            return pos;
      }
   }

   return -1;
}

/**
 * find all network interfaces.
 *
 * @param useRDMA - if true, check for RDMA devices
 * @param outList - receiver of IP addresses
 * @return true if any usable standard interface was found
 */
bool NetworkInterfaceCard::findAll(const StringList* allowedInterfacesList, bool useRDMA, bool disableIPv6,
   NicAddressList* outList)
{
   bool retVal = false;

   // find standard IP interfaces
   retVal = findAllInterfaces(NICADDRTYPE_STANDARD, allowedInterfacesList, disableIPv6, outList);

   // find RDMA interfaces (based on IP interfaces query results)
   if(retVal && useRDMA && RDMASocket::rdmaDevicesExist() )
   {
      NicAddressList tcpInterfaces(*outList);
      filterInterfacesForRDMA(*allowedInterfacesList, &tcpInterfaces, outList);
   }

   return retVal;
}

bool NetworkInterfaceCard::findAllInterfaces(NicAddrType nicType,
   const StringList* allowedInterfacesList, bool disableIPv6, NicAddressList* outList)
{

   std::unique_ptr<ifaddrs, void(*)(ifaddrs* p)>
      ifaddrs(nullptr, [](struct ifaddrs* p) {freeifaddrs(p);});

   {
      struct ifaddrs* p = nullptr;
      if (getifaddrs(&p))
      {
         LogContext("NIC query").logErr(
            std::string("getifaddrs failed: ") + System::getErrString());
         return false;
      }

      ifaddrs.reset(p);
   }

   for (struct ifaddrs* ifa = ifaddrs.get(); ifa != NULL; ifa = ifa->ifa_next)
   {
      if (ifa->ifa_addr == NULL || ifa->ifa_flags & IFF_LOOPBACK || !(ifa->ifa_flags & IFF_RUNNING))
         continue;

      if(ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
         continue;

      if(ifa->ifa_addr->sa_family == AF_INET6 && disableIPv6)
         continue;

      NicAddress nicAddr;
         
      if(fillNicAddress(nicType, ifa, &nicAddr) )
      {
         if (nicAddr.ipAddr.isLinkLocal() || nicAddr.ipAddr.isLoopback())
            continue;

         if (!allowedInterfacesList->empty() && findNicPosition(*allowedInterfacesList, nicAddr) == -1)
            continue;
            
         outList->push_back(nicAddr);
      }
   }

   return true;
}

// Compare nics for sorting
bool NetworkInterfaceCard::NicAddrComp::operator()(const NicAddress& lhs, const NicAddress& rhs) const
{
   // If preferences are set, use these first
   if (preferences) {
      int l = findNicPosition(*preferences, lhs);
      int r = findNicPosition(*preferences, rhs);

      if (r == -1 && l != -1)
         return true;
      if (l == -1 && r != -1)
         return false;

      if (l != r)
         return l < r;
   }

   // No preference match, sort by default

   // prefer RDMA NICs
   if( (lhs.nicType == NICADDRTYPE_RDMA) && (rhs.nicType != NICADDRTYPE_RDMA) )
      return true;
   if( (rhs.nicType == NICADDRTYPE_RDMA) && (lhs.nicType != NICADDRTYPE_RDMA) )
      return false;

   // prefer IPv4
   bool rhsv4 = rhs.ipAddr.isIPv4();
   bool lhsv4 = lhs.ipAddr.isIPv4();
   if (rhsv4 != lhsv4) {
      return lhsv4;
   }

   // this is the original IP-order version
   return lhs.ipAddr > rhs.ipAddr;
}

/**
 * Checks a list of IP interfaces for RDMA-capable interfaces. RDMA interfaces are
 * added to outList.
 */
void NetworkInterfaceCard::filterInterfacesForRDMA(const StringList& allowedInterfacesList,
   NicAddressList* list, NicAddressList* outList)
{
   // Note: This works by binding an RDMASocket to each IP of the passed list.

   if (!RDMASocket::isRDMAAvailable())
      return;

   for(NicAddressListIter iter = list->begin(); iter != list->end(); iter++)
   {
      try
      {
         auto rdmaSock = RDMASocket::create();

         rdmaSock->bindToAddr(iter->ipAddr.toSocketAddress(0));

         // interface is RDMA-capable => append to outList

         NicAddress nicAddr = *iter;
         nicAddr.nicType = NICADDRTYPE_RDMA;

         if (!allowedInterfacesList.empty() && findNicPosition(allowedInterfacesList, nicAddr) == -1)
            continue;

         outList->push_back(nicAddr);
      }
      catch(SocketException& e)
      {
         // interface is not RDMA-capable in this case
      }
   }
}

/*
 * Checks for RDMA-capable interfaces in a list of IP interfaces and adds the devices as
 * new RDMA devices to the list
 *
 * @param nicList a reference to a list of TCP interfaces; RDMA interfaces will be added to the list
 *
 * @return true, if at least one RDMA-capable interface was found
 */
bool NetworkInterfaceCard::checkAndAddRdmaCapability(const StringList& allowedInterfacesList, NicAddressList& nicList)
{
   // Note: This works by binding an RDMASocket to each IP of the passed list.

   NicAddressList rdmaInterfaces;

   if (RDMASocket::isRDMAAvailable())
   {
      for(NicAddressListIter iter = nicList.begin(); iter != nicList.end(); iter++)
      {
         try
         {
            if (iter->nicType == NICADDRTYPE_STANDARD)
            {
               auto rdmaSock = RDMASocket::create();

               rdmaSock->bindToAddr(iter->ipAddr.toSocketAddress(0));

               // interface is RDMA-capable => append to outList

               NicAddress nicAddr = *iter;
               nicAddr.nicType = NICADDRTYPE_RDMA;

               if (!allowedInterfacesList.empty() && findNicPosition(allowedInterfacesList, nicAddr) == -1)
                  continue;

               rdmaInterfaces.push_back(nicAddr);
            }
         }
         catch(SocketException& e)
         {
            // interface is not RDMA-capable in this case
         }
      }
   }

   const bool foundRdmaInterfaces = !rdmaInterfaces.empty();
   nicList.splice(nicList.end(), rdmaInterfaces);

   return foundRdmaInterfaces;
}


/**
 * @param ifa interface name and IP must be set in ifa when this method is called
 */
bool NetworkInterfaceCard::fillNicAddress(NicAddrType nicType, struct ifaddrs* ifa,
   NicAddress* outAddr)
{
   // IP address
   outAddr->ipAddr.setAddr(ifa->ifa_addr);

   // name
   // note: must be done at the beginning because following ioctl-calls will overwrite the data
   memcpy(outAddr->name, ifa->ifa_name, sizeof(outAddr->name));

   // copy nicType
   outAddr->nicType = nicType;

   if (ifa->ifa_addr->sa_family == AF_INET6) {
      outAddr->protocol = 6;
   } else {
      outAddr->protocol = 4;
   }

   return true;
}

/**
 * @return static string (not alloc'ed, so don't free it).
 */
const char* NetworkInterfaceCard::nicTypeToString(NicAddrType nicType)
{
   switch(nicType)
   {
      case NICADDRTYPE_RDMA: return "RDMA";
      case NICADDRTYPE_STANDARD: return "TCP";
      case NICADDRTYPE_SUPERTCP: return "SuperTCP";

      default: return "<unknown>";
   }
}

std::string NetworkInterfaceCard::nicAddrToString(NicAddress* nicAddr)
{
   std::string resultStr;

   resultStr += nicAddr->name;
   resultStr += "[";

   resultStr += std::string("ip addr: ") + nicAddr->ipAddr.toString() + "; ";
   resultStr += std::string("type: ") + nicTypeToString(nicAddr->nicType);

   resultStr += "]";

   return resultStr;
}

bool NetworkInterfaceCard::supportsRDMA(NicAddressList* nicList)
{
   for(NicAddressListIter iter = nicList->begin(); iter != nicList->end(); iter++)
   {
      if(iter->nicType == NICADDRTYPE_RDMA)
         return true;
   }

   return false;
}

void NetworkInterfaceCard::supportedCapabilities(NicAddressList* nicList,
         NicListCapabilities* outCapabilities)
{
   outCapabilities->supportsRDMA = supportsRDMA(nicList);
}

