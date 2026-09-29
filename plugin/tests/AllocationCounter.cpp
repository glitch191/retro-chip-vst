#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
    #include <malloc.h>
#endif

namespace
{
    thread_local bool tCounting = false;
    thread_local long tCount = 0;

    void noteAllocation() noexcept
    {
        if (tCounting)
            ++tCount;
    }

    void* allocate (std::size_t size) noexcept
    {
        noteAllocation();
        return std::malloc (size == 0 ? 1 : size);
    }

    void* allocateAligned (std::size_t size, std::align_val_t alignment) noexcept
    {
        noteAllocation();
        const auto align = static_cast<std::size_t> (alignment);
        if (size == 0)
            size = align;
#if defined(_MSC_VER)
        return _aligned_malloc (size, align);
#else
        return std::aligned_alloc (align, (size + align - 1) / align * align);
#endif
    }

    void freeAligned (void* p) noexcept
    {
#if defined(_MSC_VER)
        _aligned_free (p);
#else
        std::free (p);
#endif
    }
} // namespace

namespace rcvtest
{

ScopedAllocationCounter::ScopedAllocationCounter() noexcept
    : startCount (tCount), previousState (tCounting)
{
    tCounting = true;
}

ScopedAllocationCounter::~ScopedAllocationCounter() noexcept
{
    tCounting = previousState;
}

long ScopedAllocationCounter::count() const noexcept
{
    return tCount - startCount;
}

long countedAllocationsOnThisThread() noexcept
{
    return tCount;
}

} // namespace rcvtest

// ----- replacement global allocation functions ----------------------------------------------

void* operator new (std::size_t size)
{
    if (void* p = allocate (size))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    if (void* p = allocate (size))
        return p;
    throw std::bad_alloc();
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept { return allocate (size); }
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept { return allocate (size); }

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }

void* operator new (std::size_t size, std::align_val_t alignment)
{
    if (void* p = allocateAligned (size, alignment))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    if (void* p = allocateAligned (size, alignment))
        return p;
    throw std::bad_alloc();
}

void* operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { return allocateAligned (size, alignment); }
void* operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { return allocateAligned (size, alignment); }

void operator delete (void* p, std::align_val_t) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { freeAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (p); }
