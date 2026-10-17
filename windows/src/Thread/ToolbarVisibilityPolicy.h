#pragma once

// The floating toolbar belongs to the input method the user is looking at.
//
// Both directions of that rule have to come from the same authority, or the two
// halves of one window switch fight each other: the hide is decided from the
// foreground window (ITfThreadFocusSink::OnKillThreadFocus and the re-checks
// that follow it), while the show was decided from a focus callback alone. A
// focus callback that runs after the hide — a queued thread-focus notification,
// or a document focus the host re-establishes while it is being deactivated —
// then claimed the toolbar back for a window the user had already left, and the
// next hide signal took it down again: hide, show, hide, seen as a flash.
//
// The foreground window is therefore the single question every caller asks, and
// it can answer three ways:
// - this input method: the toolbar belongs here, so a focus callback may claim
//   it and a re-check must not take it down;
// - another input method: the toolbar must not be on screen, whatever focus
//   callback arrives;
// - no answer at all: the secure desktop, the Win+. panel (TextInputHost) and a
//   failed query do not mean the user moved to another input method, so the
//   current visibility stands.
//
// Claiming additionally requires that this thread still owns the input focus
// (ITfThreadMgr::IsThreadFocus); that half is checked by the caller, since it
// answers a different question: not which input method the user is looking at,
// but whether this instance is still the one that may speak for it.
enum class ToolbarVisibilityDecision
{
    // Leave the toolbar as it is.
    Keep,
    // The window the user is looking at runs another input method.
    Hide,
    // The window the user is looking at runs this input method.
    Claim,
};

// ownProfileActiveInThread: this thread's active input processor profile is
// this text service. A thread that switched to another input method already
// ended our claim in ITfActiveLanguageProfileNotifySink::OnActivated(FALSE) and
// Deactivate(); an observational callback on such a thread must neither claim
// the toolbar nor tear down whatever client the foreground window belongs to.
//
// foregroundResolved: the foreground window named an input method at all.
// foregroundIsForeign: it named one that is not this text service.
inline ToolbarVisibilityDecision DecideToolbarVisibility(bool ownProfileActiveInThread, bool foregroundResolved,
                                                         bool foregroundIsForeign)
{
    if (!ownProfileActiveInThread || !foregroundResolved)
    {
        return ToolbarVisibilityDecision::Keep;
    }
    return foregroundIsForeign ? ToolbarVisibilityDecision::Hide : ToolbarVisibilityDecision::Claim;
}
