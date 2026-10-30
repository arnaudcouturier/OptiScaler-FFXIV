#pragma once
// The two fields the lease observer reads from the process state, for tests that compile the product source without the product's State.h.
class State
{
  public:
    static State& Instance()
    {
        static State state;
        return state;
    }
    bool isShuttingDown = false;
};
