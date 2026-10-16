#ifndef WOW_SOURCE_BATTLENET_BSN_ADAPTERADDRESS_H
#define WOW_SOURCE_BATTLENET_BSN_ADAPTERADDRESS_H

#if defined(_WIN32)
#define BSN_ADAPTER_ADDRESS(Type, slot) reinterpret_cast<Type>(slot)
#define BSN_ADAPTER_DATA(data) data
#else
namespace BSN {
namespace Adapter {
// Generated code-info stores function addresses as const void*. Remove only
// that storage qualification before the native object/function conversion.
template <class Callback>
Callback FromAddress(const void* address) {
    return reinterpret_cast<Callback>(const_cast<void*>(address));
}
}
}
#define BSN_ADAPTER_ADDRESS(Type, slot) BSN::Adapter::FromAddress<Type>(slot)
// EncodeContext's opaque storage is mutable even for read-only traversal.
#define BSN_ADAPTER_DATA(data) const_cast<void*>(data)
#endif

#endif
