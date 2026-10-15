#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// The low latency input/output selection is read by every game thread that calls into the hooks and written by
// whichever of them notices a change. The available inputs are a bitmask and the selected input and output share
// one 64-bit word, so each read is a single atomic load and each write a single atomic update.
//
// ActiveInput, ActiveOutput and AvailableInputs are proxies over that state: they behave like the plain variables
// they replace (implicit read, assignment, flag_set-style set/[]/count), so users of the selection don't change.
template <typename Input, typename Output, size_t InputCount> class ConcurrentInputSelection
{
    static_assert(std::is_enum_v<Input> && std::is_enum_v<Output>);
    static_assert(InputCount > 0 && InputCount <= 64);

    std::atomic<uint64_t> available_ { 0 };
    std::atomic<uint64_t> selected_ { 0 };

    static constexpr uint64_t Pack(Input input, Output output) noexcept
    {
        return static_cast<uint32_t>(input) | (static_cast<uint64_t>(static_cast<uint32_t>(output)) << 32);
    }

    Input ReadInput() const noexcept
    {
        return static_cast<Input>(static_cast<uint32_t>(selected_.load(std::memory_order_acquire)));
    }

    Output ReadOutput() const noexcept
    {
        return static_cast<Output>(selected_.load(std::memory_order_acquire) >> 32);
    }

    // Changing one half must not lose a concurrent change of the other
    void WriteInput(Input input) noexcept
    {
        uint64_t old = selected_.load(std::memory_order_relaxed);
        while (!selected_.compare_exchange_weak(old, Pack(input, static_cast<Output>(old >> 32)),
                                                std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }

    void WriteOutput(Output output) noexcept
    {
        uint64_t old = selected_.load(std::memory_order_relaxed);
        while (!selected_.compare_exchange_weak(old, Pack(static_cast<Input>(static_cast<uint32_t>(old)), output),
                                                std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }

  public:
    class ActiveInput
    {
        ConcurrentInputSelection& _s;

      public:
        explicit ActiveInput(ConcurrentInputSelection& s) : _s(s) {}
        ActiveInput(const ActiveInput&) = delete;
        ActiveInput& operator=(const ActiveInput&) = delete;

        operator Input() const noexcept { return _s.ReadInput(); }
        // For indexing, static_cast<size_t>(activeInput)
        explicit operator size_t() const noexcept { return static_cast<size_t>(_s.ReadInput()); }
        Input operator=(Input input) noexcept
        {
            _s.WriteInput(input);
            return input;
        }
    };

    class ActiveOutput
    {
        ConcurrentInputSelection& _s;

      public:
        explicit ActiveOutput(ConcurrentInputSelection& s) : _s(s) {}
        ActiveOutput(const ActiveOutput&) = delete;
        ActiveOutput& operator=(const ActiveOutput&) = delete;

        operator Output() const noexcept { return _s.ReadOutput(); }
        explicit operator size_t() const noexcept { return static_cast<size_t>(_s.ReadOutput()); }
        Output operator=(Output output) noexcept
        {
            _s.WriteOutput(output);
            return output;
        }
    };

    class AvailableInputs
    {
        ConcurrentInputSelection& _s;

      public:
        explicit AvailableInputs(ConcurrentInputSelection& s) : _s(s) {}
        AvailableInputs(const AvailableInputs&) = delete;
        AvailableInputs& operator=(const AvailableInputs&) = delete;

        void set(Input input) noexcept
        {
            const auto index = static_cast<size_t>(input);
            if (index < InputCount)
                _s.available_.fetch_or(uint64_t { 1 } << index, std::memory_order_release);
        }

        bool operator[](Input input) const noexcept
        {
            const auto index = static_cast<size_t>(input);
            return index < InputCount &&
                   (_s.available_.load(std::memory_order_acquire) & (uint64_t { 1 } << index)) != 0;
        }

        size_t count() const noexcept { return std::popcount(_s.available_.load(std::memory_order_acquire)); }

        // A snapshot as the flag set type the plain variable was
        template <typename Flags> Flags to_flags() const
        {
            Flags result;
            const auto mask = _s.available_.load(std::memory_order_acquire);
            for (size_t i = 0; i < InputCount; ++i)
                if (mask & (uint64_t { 1 } << i))
                    result.set(static_cast<Input>(i));
            return result;
        }
    };
};
