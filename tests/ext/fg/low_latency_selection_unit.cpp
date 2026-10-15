#include "low_latency/input/ConcurrentInputSelection.h"

#include <cassert>
#include <thread>

// D5: the input/output selection is written from several hook threads. Rewritten against the proxy design of the
// [UP] header (ActiveInput / ActiveOutput / AvailableInputs over one ConcurrentInputSelection).

enum class Input : uint32_t
{
    None,
    Reflex,
    XeLL,
    Count
};
enum class Output : uint32_t
{
    None,
    Reflex,
    XeLL
};

int main()
{
    ConcurrentInputSelection<Input, Output, static_cast<size_t>(Input::Count)> state;
    ConcurrentInputSelection<Input, Output, static_cast<size_t>(Input::Count)>::ActiveInput activeInput { state };
    ConcurrentInputSelection<Input, Output, static_cast<size_t>(Input::Count)>::ActiveOutput activeOutput { state };
    ConcurrentInputSelection<Input, Output, static_cast<size_t>(Input::Count)>::AvailableInputs available { state };

    assert(available.count() == 0);
    assert(static_cast<Input>(activeInput) == Input::None);
    assert(static_cast<Output>(activeOutput) == Output::None);
    available.set(Input::Count); // out of range: ignored
    assert(available.count() == 0);
    assert(!available[Input::Count]);

    activeInput = Input::Reflex;
    assert(static_cast<Output>(activeOutput) == Output::None);
    activeOutput = Output::XeLL;
    assert(static_cast<Input>(activeInput) == Input::Reflex);
    activeInput = Input::XeLL;
    assert(static_cast<Output>(activeOutput) == Output::XeLL);
    activeOutput = Output::Reflex;
    assert(static_cast<Input>(activeInput) == Input::XeLL);
    activeInput = Input::None;
    activeOutput = Output::None;

    std::thread reflex(
        [&]
        {
            for (int i = 0; i < 100000; ++i)
            {
                available.set(Input::Reflex);
                activeInput = Input::Reflex;
            }
        });
    std::thread xell(
        [&]
        {
            for (int i = 0; i < 100000; ++i)
            {
                available.set(Input::XeLL);
                activeOutput = Output::XeLL;
            }
        });
    reflex.join();
    xell.join();

    assert(available.count() == 2);
    assert(available[Input::Reflex]);
    assert(available[Input::XeLL]);
    // neither writer may lose the other's half of the packed word
    assert(static_cast<Input>(activeInput) == Input::Reflex);
    assert(static_cast<Output>(activeOutput) == Output::XeLL);
}
