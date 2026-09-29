#pragma once

// Counts calls to the global operator new (every form, including nothrow and aligned) made
// by the calling thread while a ScopedAllocationCounter is alive. The replacement operators
// live in AllocationCounter.cpp. State is thread_local, so the counter is thread-safe and
// allocations made by other threads (JUCE's message thread helpers, the OS) are ignored.
//
// Limitation: memory obtained straight from malloc/calloc (juce::HeapBlock) bypasses
// operator new and is not counted.
namespace rcvtest
{

class ScopedAllocationCounter
{
public:
    ScopedAllocationCounter() noexcept;
    ~ScopedAllocationCounter() noexcept;

    ScopedAllocationCounter (const ScopedAllocationCounter&) = delete;
    ScopedAllocationCounter& operator= (const ScopedAllocationCounter&) = delete;

    long count() const noexcept;   // allocations on this thread since construction

private:
    long startCount;
    bool previousState;
};

// Total allocations counted on the calling thread while counting was enabled.
long countedAllocationsOnThisThread() noexcept;

} // namespace rcvtest
