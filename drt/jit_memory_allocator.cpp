#include "jit_memory_allocator.h"

void JitMemoryLargeAllocationHeader::Destroy()
{
    DoublyLink* next = m_link.next;
    DoublyLink* prev = m_link.prev;
    next->prev = prev;
    prev->next = next;
    assert(reinterpret_cast<uint64_t>(this) % x_pageSize == 0);
}

JitMemoryPageHeader* WARN_UNUSED JitMemoryAllocator::AllocateUninitalizedPage()
{
    constexpr size_t x_pageSize = JitMemoryPageHeaderBase::x_pageSize;
    if (unlikely(m_reservedRangeCur == m_reservedRangeEnd))
    {
        ReleaseAssert(false && "We can't allocate more JIT memory because this breaks our expectation that every piece of jit code can jump to any other with a relative address diffrence of <4GiB");
    }

    assert(m_reservedRangeCur + x_pageSize <= m_reservedRangeEnd);
    assert(m_reservedRangeCur % x_pageSize == 0);
    void* pageAddr = reinterpret_cast<void*>(m_reservedRangeCur);
    m_reservedRangeCur += x_pageSize;

    void* r = mmap(pageAddr, x_pageSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(r == MAP_FAILED, "Failed to allocate JIT memory of size %llu", static_cast<unsigned long long>(x_pageSize));
    assert(pageAddr == r);

    m_totalOsMemoryUsage += x_pageSize;

    return reinterpret_cast<JitMemoryPageHeader*>(pageAddr);
}

void* WARN_UNUSED JitMemoryAllocator::DoLargeAllocation(size_t size)
{
    constexpr size_t x_pageSize = JitMemoryPageHeaderBase::x_pageSize;
    size = RoundUpToMultipleOf<16384>(size + sizeof(JitMemoryLargeAllocationHeader));

    if (unlikely(m_reservedRangeCur + size >= m_reservedRangeEnd))
    {
        ReleaseAssert(false && "We can't allocate more JIT memory because this breaks our expectation that every piece of jit code can jump to any other with a relative address diffrence of <4GiB");
    }

    assert(m_reservedRangeCur + x_pageSize <= m_reservedRangeEnd);
    assert(m_reservedRangeCur % x_pageSize == 0);
    void* addr = reinterpret_cast<void*>(m_reservedRangeCur);
    m_reservedRangeCur += size;

    void* r = mmap(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(r == MAP_FAILED, "Failed to allocate JIT memory of size %llu", static_cast<unsigned long long>(x_pageSize));
    assert(addr == r);

    m_totalUsedMemory += size;
    m_totalOsMemoryUsage += size;

    JitMemoryLargeAllocationHeader* hdr = reinterpret_cast<JitMemoryLargeAllocationHeader*>(addr);
    hdr->Initialize(size, &m_laAnchor);

    void* res = hdr->GetAllocatedObject();
    assert(reinterpret_cast<uint64_t>(res) % 16 == 0);
    return res;
}

void JitMemoryAllocator::Shutdown()
{
    while (m_laAnchor.next != &m_laAnchor)
    {
        JitMemoryLargeAllocationHeader* hdr = JitMemoryLargeAllocationHeader::GetFromLinkNode(m_laAnchor.next);
        Free(hdr->GetAllocatedObject());
    }

    assert(m_laAnchor.prev == &m_laAnchor);
    assert(m_laAnchor.next == &m_laAnchor);

    do_munmap(reinterpret_cast<void*>(m_reservedRangeEnd - x_reserveRangeSize), x_reserveRangeSize);

    // This is guaranteed by the munmap above but still a bit ugly
    //
    m_totalOsMemoryUsage = 0;

    // assert(m_totalOsMemoryUsage == 0);
}
