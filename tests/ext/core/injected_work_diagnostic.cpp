#include "ext/core/InjectedWorkDiagnostic.h"

int main()
{
    using namespace ext::core::diagnostic;
    if (ObserversSuppressed() || TotalSuppressedCallbacks() != 0) return 1;
    {
        SuppressObserversScope inactive(false);
        if (ObserversSuppressed()) return 2;
        {
            SuppressObserversScope outer(true);
            if (!ObserversSuppressed()) return 3;
            {
                SuppressObserversScope nested(true);
                if (!ObserversSuppressed()) return 4;
            }
            if (!ObserversSuppressed()) return 5;
            SuppressObserversScope inactiveNested(false);
            if (!ObserversSuppressed()) return 6;
        }
        if (ObserversSuppressed()) return 7;
    }
    try
    {
        SuppressObserversScope scope(true);
        throw 42;
    }
    catch (int) {}
    if (ObserversSuppressed()) return 8;
    NoteSuppressedCallback();
    if (TotalSuppressedCallbacks() != 1) return 9;
    return 0;
}
