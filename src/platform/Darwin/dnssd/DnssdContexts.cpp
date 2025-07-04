/*
 *
 *    Copyright (c) 2021-2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
#include "DnssdError.h"
#include "DnssdImpl.h"
#include "DnssdType.h"

#include <lib/support/CHIPMemString.h>
#include <platform/CHIPDeviceLayer.h>

#include <net/if.h>

#include <string>

using namespace chip::Dnssd;
using namespace chip::Dnssd::Internal;

namespace {

constexpr uint8_t kDnssdKeyMaxSize          = 32;
constexpr uint8_t kDnssdTxtRecordMaxEntries = 20;

std::string GetHostNameWithoutDomain(const char * hostnameWithDomain)
{
    std::string hostname(hostnameWithDomain);
    size_t position = hostname.find(".");
    if (position != std::string::npos)
    {
        hostname.erase(position);
    }

    return hostname;
}

void GetTextEntries(DnssdService & service, const unsigned char * data, uint16_t len)
{
    uint16_t recordCount   = TXTRecordGetCount(len, data);
    service.mTextEntrySize = recordCount;
    service.mTextEntries   = static_cast<TextEntry *>(chip::Platform::MemoryCalloc(kDnssdTxtRecordMaxEntries, sizeof(TextEntry)));

    for (uint16_t i = 0; i < recordCount; i++)
    {
        char key[kDnssdKeyMaxSize];
        uint8_t valueLen;
        const void * valuePtr;

        auto err = TXTRecordGetItemAtIndex(len, data, i, kDnssdKeyMaxSize, key, &valueLen, &valuePtr);
        if (kDNSServiceErr_NoError != err)
        {
            // If there is an error with a txt record stop the parsing here.
            service.mTextEntrySize = i;
            break;
        }

        if (valueLen >= chip::Dnssd::kDnssdTextMaxSize)
        {
            // Truncation, but nothing better we can do
            valueLen = chip::Dnssd::kDnssdTextMaxSize - 1;
        }

        char value[chip::Dnssd::kDnssdTextMaxSize];
        memcpy(value, valuePtr, valueLen);
        value[valueLen] = 0;

        auto & textEntry    = service.mTextEntries[i];
        textEntry.mKey      = strdup(key);
        textEntry.mData     = reinterpret_cast<const uint8_t *>(strdup(value));
        textEntry.mDataSize = valueLen;
    }
}

DNSServiceProtocol GetProtocol(const chip::Inet::IPAddressType & addressType)
{
#if INET_CONFIG_ENABLE_IPV4
    if (addressType == chip::Inet::IPAddressType::kIPv4)
    {
        return kDNSServiceProtocol_IPv4;
    }

    if (addressType == chip::Inet::IPAddressType::kIPv6)
    {
        return kDNSServiceProtocol_IPv6;
    }

    return kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6;
#else
    // without IPv4, IPv6 is the only option
    return kDNSServiceProtocol_IPv6;
#endif
}

DnssdService GetService(const char * name, const char * type, DnssdServiceProtocol protocol, uint32_t interfaceId)
{
    DnssdService service = {};
    service.mInterface   = chip::Inet::InterfaceId(interfaceId);
    service.mProtocol    = protocol;

    auto baseType = GetBaseType(type);
    chip::Platform::CopyString(service.mType, baseType.c_str());
    chip::Platform::CopyString(service.mName, name);

    return service;
}

} // namespace

namespace chip {
namespace Dnssd {

CHIP_ERROR GenericContext::FinalizeInternal(const char * errorStr, CHIP_ERROR err)
{
    if (MdnsContexts::GetInstance().Has(this) == CHIP_NO_ERROR)
    {
        if (CHIP_NO_ERROR == err)
        {
            DispatchSuccess();
        }
        else
        {
            DispatchFailure(errorStr, err);
        }
    }
    else
    {
        // Ensure that we clean up our service ref, if any, correctly.
        MdnsContexts::GetInstance().Delete(this);
    }

    return err;
}

CHIP_ERROR GenericContext::Finalize(CHIP_ERROR err)
{
    return FinalizeInternal(err.AsString(), err);
}

CHIP_ERROR GenericContext::Finalize(DNSServiceErrorType err)
{
    return FinalizeInternal(Error::ToString(err), Error::ToChipError(err));
}

MdnsContexts::~MdnsContexts()
{
    std::vector<GenericContext *>::const_iterator iter = mContexts.cbegin();
    while (iter != mContexts.cend())
    {
        Delete(*iter);
        mContexts.erase(iter);
    }
}

CHIP_ERROR MdnsContexts::Add(GenericContext * context)
{
    VerifyOrReturnError(context != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    if (!context->serviceRef)
    {
        Delete(context);
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    auto err = DNSServiceSetDispatchQueue(context->serviceRef, chip::DeviceLayer::PlatformMgrImpl().GetWorkQueue());
    if (kDNSServiceErr_NoError != err)
    {
        Delete(context);
        return Error::ToChipError(err);
    }

    mContexts.push_back(context);

    return CHIP_NO_ERROR;
}

bool MdnsContexts::RemoveWithoutDeleting(GenericContext * context)
{
    std::vector<GenericContext *>::const_iterator iter = mContexts.cbegin();
    while (iter != mContexts.cend())
    {
        if (*iter != context)
        {
            iter++;
            continue;
        }

        mContexts.erase(iter);
        return true;
    }

    return false;
}

CHIP_ERROR MdnsContexts::Remove(GenericContext * context)
{
    bool found = RemoveWithoutDeleting(context);
    if (found)
    {
        Delete(context);
    }
    return found ? CHIP_NO_ERROR : CHIP_ERROR_KEY_NOT_FOUND;
}

CHIP_ERROR MdnsContexts::RemoveAllOfType(ContextType type)
{
    bool found = false;

    std::vector<GenericContext *>::const_iterator iter = mContexts.cbegin();
    while (iter != mContexts.cend())
    {
        if ((*iter)->type != type)
        {
            iter++;
            continue;
        }

        Delete(*iter);
        mContexts.erase(iter);
        found = true;
    }

    return found ? CHIP_NO_ERROR : CHIP_ERROR_KEY_NOT_FOUND;
}

// TODO: Perhaps this cleanup code should just move into ~GenericContext, and
// the places that call this method should just Platform::Delete() the context?
void MdnsContexts::Delete(GenericContext * context)
{
    if (context->serviceRef != nullptr)
    {
        DNSServiceRefDeallocate(context->serviceRef);
        context->serviceRef = nullptr;
    }
    chip::Platform::Delete(context);
}

CHIP_ERROR MdnsContexts::Has(GenericContext * context)
{
    std::vector<GenericContext *>::iterator iter;

    for (iter = mContexts.begin(); iter != mContexts.end(); iter++)
    {
        if ((*iter) == context)
        {
            return CHIP_NO_ERROR;
        }
    }

    return CHIP_ERROR_KEY_NOT_FOUND;
}

#if DNSSD_VERBOSE_CONTEXT_PRINT
void MdnsContexts::Print() const
{
    ChipLogDetail(Discovery, "MdnsContexts:");
    std::vector<GenericContext *>::const_iterator iter = mContexts.cbegin();
    while (iter != mContexts.cend())
    {
        const char * contextType = "Unknown";
        switch ((*iter)->type)
        {
        case ContextType::Register:
            contextType = "Register";
            break;
        case ContextType::Browse:
            contextType = "Browse";
            break;
        case ContextType::BrowseWithDelegate:
            contextType = "BrowseWithDelegate";
            break;
        case ContextType::Resolve:
            contextType = "Resolve";
            break;
        }

        // Print the pointer as a unique identifier so concurrent contexts can be distinguished in logs
        ChipLogDetail(Discovery, "\t%p: %s", *iter, contextType);
        iter++;
    }
}
#endif // DNSSD_VERBOSE_CONTEXT_PRINT

CHIP_ERROR MdnsContexts::GetRegisterContextOfTypeAndName(const char * type, const char * name, RegisterContext ** context)
{
    bool found = false;
    std::vector<GenericContext *>::iterator iter;

    for (iter = mContexts.begin(); iter != mContexts.end(); iter++)
    {
        if ((*iter)->type == ContextType::Register && (static_cast<RegisterContext *>(*iter))->matches(type, name))
        {
            *context = static_cast<RegisterContext *>(*iter);
            found    = true;
            break;
        }
    }

    return found ? CHIP_NO_ERROR : CHIP_ERROR_KEY_NOT_FOUND;
}

ResolveContext * MdnsContexts::GetExistingResolveForInstanceName(const char * instanceName)
{
    for (auto & ctx : mContexts)
    {
        if (ctx->type == ContextType::Resolve && (static_cast<ResolveContext *>(ctx))->Matches(instanceName))
        {
            return static_cast<ResolveContext *>(ctx);
        }
    }

    return nullptr;
}

BrowseWithDelegateContext * MdnsContexts::GetExistingBrowseForDelegate(DnssdBrowseDelegate * delegate)
{
    for (auto & ctx : mContexts)
    {
        if (ctx->type == ContextType::BrowseWithDelegate && (static_cast<BrowseWithDelegateContext *>(ctx))->Matches(delegate))
        {
            return static_cast<BrowseWithDelegateContext *>(ctx);
        }
    }

    return nullptr;
}

RegisterContext::RegisterContext(const char * sType, const char * instanceName, DnssdPublishCallback cb, void * cbContext)
{
    type     = ContextType::Register;
    context  = cbContext;
    callback = cb;

    mType         = sType;
    mInstanceName = instanceName;
}

void RegisterContext::DispatchFailure(const char * errorStr, CHIP_ERROR err)
{
    ChipLogError(Discovery, "Mdns: Register failure (%s)", errorStr);
    callback(context, nullptr, nullptr, err);
    MdnsContexts::GetInstance().Remove(this);
}

void RegisterContext::DispatchSuccess()
{
    std::string typeWithoutSubTypes = GetFullTypeWithoutSubTypes(mType);
    callback(context, typeWithoutSubTypes.c_str(), mInstanceName.c_str(), CHIP_NO_ERROR);
}

BrowseContext * BrowseContext::sContextDispatchingSuccess      = nullptr;
std::vector<DnssdService> * BrowseContext::sDispatchedServices = nullptr;

BrowseContext::BrowseContext(void * cbContext, DnssdBrowseCallback cb, DnssdServiceProtocol cbContextProtocol)
{
    type     = ContextType::Browse;
    context  = cbContext;
    callback = cb;
    protocol = cbContextProtocol;
}

void BrowseContext::DispatchFailure(const char * errorStr, CHIP_ERROR err)
{
    if (err == CHIP_ERROR_CANCELLED && dispatchedSuccessOnce)
    {
        // We've been canceled after finding some devices.  Treat this as a
        // success, because maybe (common case, in fact) those were the devices
        // our consumer was looking for.
        ChipLogProgress(Discovery, "Mdns: Browse canceled");
    }
    else
    {
        ChipLogError(Discovery, "Mdns: Browse failure (%s)", errorStr);
    }
    callback(context, nullptr, 0, true, err);
    MdnsContexts::GetInstance().Remove(this);
}

void BrowseContext::DispatchSuccess()
{
    // This should never be called: We either DispatchPartialSuccess or
    // DispatchFailure.
    VerifyOrDie(false);
}

void BrowseContext::DispatchPartialSuccess()
{
    sContextDispatchingSuccess = this;
    std::vector<DnssdService> dnsServices;
    for (auto iter : services)
    {
        dnsServices.push_back(std::move(iter.first));
    }

    sDispatchedServices = &dnsServices;
    callback(context, dnsServices.data(), dnsServices.size(), false, CHIP_NO_ERROR);
    sDispatchedServices        = nullptr;
    sContextDispatchingSuccess = nullptr;
    services.clear();

    dispatchedSuccessOnce = true;
}

void BrowseContext::OnBrowse(DNSServiceFlags flags, const char * name, const char * type, const char * domain, uint32_t interfaceId)
{
    (flags & kDNSServiceFlagsAdd) ? OnBrowseAdd(name, type, domain, interfaceId) : OnBrowseRemove(name, type, domain, interfaceId);

    if (!(flags & kDNSServiceFlagsMoreComing))
    {
        DispatchPartialSuccess();
    }
}

void BrowseContext::OnBrowseAdd(const char * name, const char * type, const char * domain, uint32_t interfaceId)
{
    ChipLogProgress(Discovery, "Mdns: %s  name: %s, type: %s, domain: %s, interface: %" PRIu32, __func__, StringOrNullMarker(name),
                    StringOrNullMarker(type), StringOrNullMarker(domain), interfaceId);

    auto service = GetService(name, type, protocol, interfaceId);
    services.push_back(std::make_pair(std::move(service), std::string(domain)));
}

void BrowseContext::OnBrowseRemove(const char * name, const char * type, const char * domain, uint32_t interfaceId)
{
    ChipLogProgress(Discovery, "Mdns: %s  name: %s, type: %s, domain: %s, interface: %" PRIu32, __func__, StringOrNullMarker(name),
                    StringOrNullMarker(type), StringOrNullMarker(domain), interfaceId);

    VerifyOrReturn(name != nullptr);
    std::string domain_str(domain);

    services.erase(std::remove_if(services.begin(), services.end(),
                                  [name, type, interfaceId, &domain_str](const auto & service) {
                                      return strcmp(name, service.first.mName) == 0 && type == GetFullType(&service.first) &&
                                          service.first.mInterface == chip::Inet::InterfaceId(interfaceId) &&
                                          service.second == domain_str;
                                  }),
                   services.end());
}

BrowseWithDelegateContext::BrowseWithDelegateContext(DnssdBrowseDelegate * delegate, DnssdServiceProtocol cbContextProtocol)
{
    type     = ContextType::BrowseWithDelegate;
    context  = static_cast<void *>(delegate);
    protocol = cbContextProtocol;
}

void BrowseWithDelegateContext::DispatchFailure(const char * errorStr, CHIP_ERROR err)
{
    ChipLogError(Discovery, "Mdns: Browse failure (%s)", errorStr);

    auto delegate = static_cast<DnssdBrowseDelegate *>(context);
    delegate->OnBrowseStop(err);
    MdnsContexts::GetInstance().Remove(this);
}

void BrowseWithDelegateContext::DispatchSuccess()
{
    auto delegate = static_cast<DnssdBrowseDelegate *>(context);
    delegate->OnBrowseStop(CHIP_NO_ERROR);
    MdnsContexts::GetInstance().Remove(this);
}

void BrowseWithDelegateContext::OnBrowse(DNSServiceFlags flags, const char * name, const char * type, const char * domain,
                                         uint32_t interfaceId)
{
    (flags & kDNSServiceFlagsAdd) ? OnBrowseAdd(name, type, domain, interfaceId) : OnBrowseRemove(name, type, domain, interfaceId);
}

void BrowseWithDelegateContext::OnBrowseAdd(const char * name, const char * type, const char * domain, uint32_t interfaceId)
{
    ChipLogProgress(Discovery, "Mdns: %s  name: %s, type: %s, domain: %s, interface: %" PRIu32, __func__, StringOrNullMarker(name),
                    StringOrNullMarker(type), StringOrNullMarker(domain), interfaceId);

    auto delegate = static_cast<DnssdBrowseDelegate *>(context);
    auto service  = GetService(name, type, protocol, interfaceId);
    delegate->OnBrowseAdd(service);
}

void BrowseWithDelegateContext::OnBrowseRemove(const char * name, const char * type, const char * domain, uint32_t interfaceId)
{
    ChipLogProgress(Discovery, "Mdns: %s  name: %s, type: %s, domain: %s, interface: %" PRIu32, __func__, StringOrNullMarker(name),
                    StringOrNullMarker(type), StringOrNullMarker(domain), interfaceId);

    VerifyOrReturn(name != nullptr);

    auto delegate = static_cast<DnssdBrowseDelegate *>(context);
    auto service  = GetService(name, type, protocol, interfaceId);
    delegate->OnBrowseRemove(service);
}

ResolveContext::ResolveContext(void * cbContext, DnssdResolveCallback cb, chip::Inet::IPAddressType cbAddressType,
                               const char * instanceNameToResolve, BrowseContext * browseCausingResolve,
                               std::shared_ptr<uint32_t> && consumerCounterToUse) :
    browseThatCausedResolve(browseCausingResolve)
{
    type            = ContextType::Resolve;
    context         = cbContext;
    callback        = cb;
    protocol        = GetProtocol(cbAddressType);
    instanceName    = instanceNameToResolve;
    consumerCounter = std::move(consumerCounterToUse);
}

ResolveContext::ResolveContext(DiscoverNodeDelegate * delegate, chip::Inet::IPAddressType cbAddressType,
                               const char * instanceNameToResolve, std::shared_ptr<uint32_t> && consumerCounterToUse) :
    browseThatCausedResolve(nullptr)
{
    type            = ContextType::Resolve;
    context         = delegate;
    callback        = nullptr;
    protocol        = GetProtocol(cbAddressType);
    instanceName    = instanceNameToResolve;
    consumerCounter = std::move(consumerCounterToUse);
}

void ResolveContext::DispatchFailure(const char * errorStr, CHIP_ERROR err)
{
    ChipLogError(Discovery, "Mdns: Resolve failure (%s)", errorStr);
    // Remove before dispatching, so calls back into
    // ChipDnssdResolveNoLongerNeeded don't find us and try to also remove us.
    bool needDelete = MdnsContexts::GetInstance().RemoveWithoutDeleting(this);

    if (nullptr == callback)
    {
        // Nothing to do.
    }
    else
    {
        callback(context, nullptr, Span<Inet::IPAddress>(), err);
    }

    if (needDelete)
    {
        MdnsContexts::GetInstance().Delete(this);
    }
}

void ResolveContext::DispatchSuccess()
{
    // Remove before dispatching, so calls back into
    // ChipDnssdResolveNoLongerNeeded don't find us and try to also remove us.
    bool needDelete = MdnsContexts::GetInstance().RemoveWithoutDeleting(this);

    class AutoSelfDeleter
    {
    public:
        AutoSelfDeleter(bool needDelete, ResolveContext * self) : mNeedDelete(needDelete), mSelf(self) {}

        ~AutoSelfDeleter()
        {
            if (mNeedDelete)
            {
                MdnsContexts::GetInstance().Delete(mSelf);
            }
        }

    private:
        bool mNeedDelete;
        ResolveContext * mSelf;
    };

    AutoSelfDeleter selfDeleter(needDelete, this);

#if TARGET_OS_TV
    // On tvOS, prioritize results from en0, en1, ir0 in that order, if those
    // interfaces are present, since those will generally have more up-to-date
    // information.
    static const unsigned int priorityInterfaceIndices[] = {
        if_nametoindex("en0"),
        if_nametoindex("en1"),
        if_nametoindex("ir0"),
    };
#else
    // Elsewhere prioritize "lo0" over other interfaces.
    static const unsigned int priorityInterfaceIndices[] = {
        if_nametoindex("lo0"),
    };
#endif // TARGET_OS_TV

    std::vector<InterfaceKey> interfacesOrder;
    for (auto priorityInterfaceIndex : priorityInterfaceIndices)
    {
        if (priorityInterfaceIndex == 0)
        {
            // Not actually an interface we have, since if_nametoindex
            // returned 0.
            continue;
        }

        for (auto & interface : interfaces)
        {
            if (interface.second.HasAddresses() && priorityInterfaceIndex == interface.first.interfaceId)
            {
                interfacesOrder.push_back(interface.first);
            }
        }
    }

    for (auto & interface : interfaces)
    {
        // Skip interfaces that have already been prioritized to avoid duplicate results
        auto interfaceKey = std::find(std::begin(interfacesOrder), std::end(interfacesOrder), interface.first);
        if (interfaceKey != std::end(interfacesOrder))
        {
            continue;
        }

        // Some interface may not have any ips, just ignore them.
        if (!interface.second.HasAddresses())
        {
            continue;
        }

        interfacesOrder.push_back(interface.first);
    }

    for (auto & interfaceKey : interfacesOrder)
    {
        auto & interfaceInfo = interfaces[interfaceKey];
        auto & service       = interfaceInfo.service;
        auto & ips           = interfaceInfo.addresses;
        auto addresses       = Span<Inet::IPAddress>(ips.data(), ips.size());

        ChipLogProgress(Discovery, "Mdns: Resolve success on interface %" PRIu32, interfaceKey.interfaceId);

        if (nullptr == callback)
        {
            auto delegate = static_cast<DiscoverNodeDelegate *>(context);
            DiscoveredNodeData nodeData;

            // Check whether mType (service name) exactly matches with operational service name
            if (strcmp(service.mType, kOperationalServiceName) == 0)
            {
                service.ToDiscoveredOperationalNodeBrowseData(nodeData);
            }
            else
            {
                service.ToDiscoveredCommissionNodeData(addresses, nodeData);
            }
            delegate->OnNodeDiscovered(nodeData);
        }
        else
        {
            CHIP_ERROR error = &interfaceKey == &interfacesOrder.back() ? CHIP_NO_ERROR : CHIP_ERROR_IN_PROGRESS;
            callback(context, &service, addresses, error);
        }
    }

    VerifyOrDo(interfacesOrder.size(),
               ChipLogError(Discovery, "Successfully finalizing resolve for %s without finding any actual IP addresses.",
                            instanceName.c_str()));
}

CHIP_ERROR ResolveContext::OnNewAddress(const InterfaceKey & interfaceKey, const struct sockaddr * address)
{
    // If we don't have any information about this interfaceId, just ignore the
    // address, since it won't be usable anyway without things like the port.
    // This can happen if "local" is set up as a search domain in the DNS setup
    // on the system, because the hostnames we are looking up all end in
    // ".local".  In other words, we can get regular DNS results in here, not
    // just DNS-SD ones.
    auto interfaceId = interfaceKey.interfaceId;

    if (interfaces.find(interfaceKey) == interfaces.end())
    {
        return CHIP_NO_ERROR;
    }

    chip::Inet::IPAddress ip;
    ReturnErrorOnFailure(chip::Inet::IPAddress::GetIPAddressFromSockAddr(*address, ip));

#ifdef CHIP_PROGRESS_LOGGING
    char addrStr[INET6_ADDRSTRLEN];
    ip.ToString(addrStr, sizeof(addrStr));
    ChipLogProgress(Discovery, "Mdns: %s instance: %s interface: %" PRIu32 " ip: %s", __func__, instanceName.c_str(), interfaceId,
                    addrStr);
#endif // CHIP_PROGRESS_LOGGING

    if (ip.IsIPv6LinkLocal() && interfaceId == kDNSServiceInterfaceIndexLocalOnly)
    {
        // We need a real interface to use a link-local address.  Just ignore
        // this one, because trying to use it will simply lead to "No route to
        // host" errors.
        ChipLogProgress(Discovery, "Mdns: Ignoring link-local address with no usable interface");
        return CHIP_NO_ERROR;
    }

    interfaces[interfaceKey].addresses.push_back(ip);

    return CHIP_NO_ERROR;
}

bool ResolveContext::HasAddress()
{
    for (auto & interface : interfaces)
    {
        if (interface.second.addresses.size())
        {
            return true;
        }
    }

    return false;
}

void ResolveContext::OnNewInterface(uint32_t interfaceId, const char * fullname, const char * hostnameWithDomain, uint16_t port,
                                    uint16_t txtLen, const unsigned char * txtRecord, bool isFromSRPResolve)
{
#if CHIP_PROGRESS_LOGGING
    std::string txtString;
    auto txtRecordIter  = txtRecord;
    size_t remainingLen = txtLen;
    while (remainingLen > 0)
    {
        size_t len = *txtRecordIter;
        ++txtRecordIter;
        --remainingLen;
        len = std::min(len, remainingLen);
        chip::Span<const unsigned char> bytes(txtRecordIter, len);
        if (txtString.size() > 0)
        {
            txtString.push_back(',');
        }
        for (auto & byte : bytes)
        {
            if ((std::isalnum(byte) || std::ispunct(byte)) && byte != '\\' && byte != ',')
            {
                txtString.push_back(static_cast<char>(byte));
            }
            else
            {
                char hex[5];
                snprintf(hex, sizeof(hex), "\\x%02x", byte);
                txtString.append(hex);
            }
        }
        txtRecordIter += len;
        remainingLen -= len;
    }
#endif // CHIP_PROGRESS_LOGGING
    ChipLogProgress(Discovery, "Mdns : %s hostname:%s fullname:%s interface: %" PRIu32 " port: %u TXT:\"%s\"", __func__,
                    hostnameWithDomain, fullname, interfaceId, ntohs(port), txtString.c_str());

    InterfaceInfo interface;
    interface.service.mPort = ntohs(port);

    if (kDNSServiceInterfaceIndexLocalOnly == interfaceId)
    {
        // Set interface to ANY (0) - network stack can decide how to route this.
        interface.service.mInterface = Inet::InterfaceId(0);
    }
    else
    {
        interface.service.mInterface = Inet::InterfaceId(interfaceId);
    }

    // The hostname parameter contains the hostname followed by the domain. But the mHostName field is sized
    // to contain either a 12 bytes mac address or an extended address of at most 16 bytes, not the domain name.
    auto hostname = GetHostNameWithoutDomain(hostnameWithDomain);
    Platform::CopyString(interface.service.mHostName, hostname.c_str());
    Platform::CopyString(interface.service.mName, fullname);

    GetTextEntries(interface.service, txtRecord, txtLen);

    // If for some reason the hostname can not fit into the hostname field (e.g it is not a mac address) then
    // DNSServiceGetAddrInfo will never return anything. So instead, copy the name as the FQDN and use it for
    // resolving.
    interface.fullyQualifiedDomainName = hostnameWithDomain;

    InterfaceKey interfaceKey = { interfaceId, hostnameWithDomain, isFromSRPResolve };
    interfaces.insert(std::make_pair(std::move(interfaceKey), std::move(interface)));
}

bool ResolveContext::HasInterface()
{
    return interfaces.size();
}

InterfaceInfo::InterfaceInfo()
{
    service.mTextEntrySize = 0;
    service.mTextEntries   = nullptr;
}

InterfaceInfo::InterfaceInfo(InterfaceInfo && other) :
    service(std::move(other.service)), addresses(std::move(other.addresses)),
    fullyQualifiedDomainName(std::move(other.fullyQualifiedDomainName)), isDNSLookUpRequested(other.isDNSLookUpRequested)
{
    // Make sure we're not trying to free any state from the other DnssdService,
    // since we took over ownership of its allocated bits.
    other.service.mTextEntrySize = 0;
    other.service.mTextEntries   = nullptr;
}

InterfaceInfo::~InterfaceInfo()
{
    if (service.mTextEntries == nullptr)
    {
        return;
    }

    const size_t count = service.mTextEntrySize;
    for (size_t i = 0; i < count; i++)
    {
        const auto & textEntry = service.mTextEntries[i];
        free(const_cast<char *>(textEntry.mKey));
        free(const_cast<uint8_t *>(textEntry.mData));
    }
    Platform::MemoryFree(const_cast<TextEntry *>(service.mTextEntries));
}

} // namespace Dnssd
} // namespace chip
