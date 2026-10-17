#include <cstdint>
#if defined(_WIN32)
#include <windows.h>
#define BASE_RECYCLER_GUARD
#define BASE_RECYCLER_LOAD(pointer) *pointer
#define BASE_RECYCLER_STORE(pointer, value) *pointer = value
#define BASE_RECYCLER_CAS(pointer, value, expected) InterlockedCompareExchangePointer(pointer, value, expected)
#define BASE_RECYCLER_INCREMENT(pointer) InterlockedIncrement(reinterpret_cast<volatile LONG*>(pointer))
#define BASE_RECYCLER_DECREMENT(pointer) InterlockedDecrement(reinterpret_cast<volatile LONG*>(pointer))
#else
#include <cstring>
#include <mutex>
namespace {
std::mutex s_recyclerLists;
// All list accesses hold s_recyclerLists. Byte copies avoid accessing a typed
// Node*/NodeBlock* slot through an incompatible void** lvalue.
void* RecyclerLoad(const void* pointer) {
    void* value;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}
void RecyclerStore(void* pointer, void* value) {
    std::memcpy(pointer, &value, sizeof(value));
}
void* RecyclerCompareExchange(void** pointer, void* value, void* expected) {
    void* previous = RecyclerLoad(pointer);
    if (previous == expected) RecyclerStore(pointer, value);
    return previous;
}
}
#define BASE_RECYCLER_LOAD(pointer) RecyclerLoad(pointer)
#define BASE_RECYCLER_STORE(pointer, value) RecyclerStore(pointer, value)
#define BASE_RECYCLER_GUARD std::lock_guard<std::mutex> listGuard(s_recyclerLists);
#define BASE_RECYCLER_CAS(pointer, value, expected) RecyclerCompareExchange(pointer, value, expected)
#define BASE_RECYCLER_INCREMENT(pointer) __atomic_add_fetch(pointer, 1, __ATOMIC_SEQ_CST)
#define BASE_RECYCLER_DECREMENT(pointer) __atomic_sub_fetch(pointer, 1, __ATOMIC_SEQ_CST)
#endif
#line 3
#include "Engine/Source/Base/CDataRecycler.h"
#include "Engine/Source/Storm/H/storm.h"

// VERIFY: a2b3cf86 manual
// OFFSET: 0x0095CD20
CDataRecycler::CDataRecycler(uint32_t nodesPerBlock, int32_t maxNodes) {
    this->m_nodeBlockList = nullptr;
    this->m_nodeFullList = nullptr;
    this->m_nodeEmptyList = nullptr;
    this->m_nodesRecyclable = maxNodes <= 1 ? 1 : maxNodes;
    this->m_nodesPerBlock = nodesPerBlock <= 1 ? 1 : nodesPerBlock;
    this->m_nodesPerBlock = this->m_nodesPerBlock >= static_cast<uint32_t>(this->m_nodesRecyclable) ? this->m_nodesRecyclable : this->m_nodesPerBlock;
}

// VERIFY: d99656c0 auto
// OFFSET: [0x0095CD70, 0x0095CE00]
CDataRecycler::~CDataRecycler() {
}

// VERIFY: 6077ec21 auto
// OFFSET: 0x0095CD80
void* CDataRecycler::AllocData(uint32_t allocBytes, uint32_t* bytes, const char* fileName, int32_t lineNumber) {
    void* data = SMemAlloc(allocBytes, fileName, lineNumber, 0);

    if (bytes) {
        *bytes = allocBytes;
    }

    return data;
}

// VERIFY: 5ada6833 auto
// OFFSET: 0x0095CDB0
void* CDataRecycler::ReallocData(void* data, uint32_t allocBytes, uint32_t* bytes, const char* fileName, int32_t lineNumber) {
    void* result = SMemReAlloc(data, allocBytes, fileName, lineNumber, 0);

    if (bytes) {
        *bytes = allocBytes;
    }

    return result;
}

// VERIFY: bbf3e45c auto
// OFFSET: 0x0095CDE0
void CDataRecycler::FreeData(void* data, const char* fileName, int32_t lineNumber) {
    SMemFree(data, fileName, lineNumber, 0);
}

// VERIFY: da650ee0 manual
// OFFSET: 0x0095CE30
void CDataRecycler::Link(Node** list, NodeBlock* nodeBlock) { BASE_RECYCLER_GUARD
    Node* node = nodeBlock->m_nodes;

    for (uint32_t i = 0; i < this->m_nodesPerBlock - 1; i++) {
        node->m_next = node + 1;
        node++;
    }

    Node* last = &nodeBlock->m_nodes[this->m_nodesPerBlock - 1];
    Node* head;

    do {
        head = *list;
        last->m_next = head;
    } while (BASE_RECYCLER_CAS(reinterpret_cast<void**>(list), nodeBlock->m_nodes, head) != head);
}

// VERIFY: 1f959b49 manual
// OFFSET: 0x0095CE90
void CDataRecycler::Link(void** list, void* item) { BASE_RECYCLER_GUARD
    void* head;

    do {
        head = BASE_RECYCLER_LOAD(list);
        BASE_RECYCLER_STORE(static_cast<void**>(item), head);
    } while (BASE_RECYCLER_CAS(list, item, head) != head);
}

// INLINED
void* CDataRecycler::Unlink(void** list) { BASE_RECYCLER_GUARD
    void* item;

    for (item = BASE_RECYCLER_LOAD(list); BASE_RECYCLER_LOAD(list); item = BASE_RECYCLER_LOAD(list)) {
        if (BASE_RECYCLER_CAS(list, BASE_RECYCLER_LOAD(static_cast<void**>(item)), item) == item) {
            break;
        }
    }

    return item;
}

// VERIFY: a256c569 manual
// OFFSET: 0x0095CEC0
void CDataRecycler::Clear() {
    for (Node* node = static_cast<Node*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeFullList))); node; node = static_cast<Node*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeFullList)))) {
        this->FreeData(node->m_data, nullptr, 0);
    }

    this->m_nodeEmptyList = nullptr;

    for (NodeBlock* block = static_cast<NodeBlock*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeBlockList))); block; block = static_cast<NodeBlock*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeBlockList)))) {
        delete block;
    }
}

// VERIFY: 380c0464 manual
// OFFSET: 0x0095CF40
void CDataRecycler::GetData(void*& data, uint32_t& bytes, const char* fileName, int32_t lineNumber) {
    Node* node = static_cast<Node*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeFullList)));

    if (node) {
        BASE_RECYCLER_INCREMENT(&this->m_nodesRecyclable);
        data = node->m_data;
        bytes = node->m_bytes;
        this->Link(reinterpret_cast<void**>(&this->m_nodeEmptyList), node);
    } else {
        data = nullptr;
        bytes = 0;
    }
}

// VERIFY: 6af524b4 manual
// OFFSET: 0x0095CFB0
void CDataRecycler::PutData(void* data, uint32_t bytes, const char* fileName, int32_t lineNumber) {
    if (BASE_RECYCLER_DECREMENT(&this->m_nodesRecyclable) >= 0) {
        Node* node;

        for (node = static_cast<Node*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeEmptyList))); !node; node = static_cast<Node*>(this->Unlink(reinterpret_cast<void**>(&this->m_nodeEmptyList)))) {
            NodeBlock* block = static_cast<NodeBlock*>(SMemAlloc(sizeof(NodeBlock) + sizeof(Node) * (this->m_nodesPerBlock - 1), __FILE__, __LINE__, 0));
            this->Link(reinterpret_cast<void**>(&this->m_nodeBlockList), block);
            this->Link(&this->m_nodeEmptyList, block);
        }

        node->m_data = data;
        node->m_bytes = bytes;
        this->Link(reinterpret_cast<void**>(&this->m_nodeFullList), node);
    } else {
        BASE_RECYCLER_INCREMENT(&this->m_nodesRecyclable);
        this->FreeData(data, fileName, lineNumber);
    }
}
