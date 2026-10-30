#include "pch.h"

#include "ext/Seams.h"
#include "ext/fg/FrameJumpLog.h"

#include <mutex>

namespace ext::fg
{
// IFGFeature::StartNewFrame, where the frame count jumped by more than the two frames a re-sync expects. The state is shared by
// every frame generator of the process (a provider switch can leave two of them starting frames) and a jump is rare, so one
// lock around the counter costs nothing and keeps the counts exact.
void NoteFrameCountJump(uint64_t frameCount, uint64_t lastDispatchedFrame)
{
    static std::mutex mutex;
    static JumpLimiter limiter;
    JumpNote note;
    {
        std::lock_guard lock(mutex);
        note = limiter.Note(GetTickCount64());
    }
    if (!note.log)
        return;
    if (note.suppressed == 0)
        LOG_WARN("Frame count jumped too much! _frameCount: {}, _lastDispatchedFrame: {}", frameCount, lastDispatchedFrame);
    else
        LOG_WARN("Frame count jumped too much! {} times in the last {} ms (latest _frameCount: {}, _lastDispatchedFrame: {})",
                 note.suppressed, note.sinceMs, frameCount, lastDispatchedFrame);
}
} // namespace ext::fg
