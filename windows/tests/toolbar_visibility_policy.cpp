#include "Thread/ToolbarVisibilityPolicy.h"

int main()
{
    // Do not use assert: Release builds must execute these checks too.

    // The window the user moved to runs another input method: hide it, and
    // never let a focus callback claim the toolbar back for that window.
    if (DecideToolbarVisibility(true, true, true) != ToolbarVisibilityDecision::Hide)
        return 1;

    // The window the user moved to runs this input method: claim it.
    if (DecideToolbarVisibility(true, true, false) != ToolbarVisibilityDecision::Claim)
        return 2;

    // The secure desktop, the Win+. panel and a failed query answer nothing, so
    // neither direction may act on them.
    if (DecideToolbarVisibility(true, false, false) != ToolbarVisibilityDecision::Keep)
        return 3;
    // A query that failed while it looked foreign is still not a verdict.
    if (DecideToolbarVisibility(true, false, true) != ToolbarVisibilityDecision::Keep)
        return 4;

    // This thread's active input method is another one: OnActivated(FALSE) and
    // Deactivate() already ended our claim, and an observational callback here
    // must neither claim the toolbar nor retire the foreground client's.
    if (DecideToolbarVisibility(false, true, true) != ToolbarVisibilityDecision::Keep)
        return 5;
    if (DecideToolbarVisibility(false, true, false) != ToolbarVisibilityDecision::Keep)
        return 6;
    return 0;
}
