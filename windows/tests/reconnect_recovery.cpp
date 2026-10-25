#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>

using UINT = unsigned int;
using HWND = int;
using std::min;

constexpr UINT CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS = 50;
constexpr UINT CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS = 2000;
constexpr UINT TIMER_CONNECT_ALL_NAMEDPIPE = 1;
constexpr UINT WM_IpcReconnect = 2;
constexpr UINT WM_ConnectNamedpipe = 3;

namespace Global
{
bool g_connected = true;
}

// windows/src/Thread/ToolbarVisibilityPolicy.h defines this for the production
// handler. The fixture compiles that handler body in this standalone TU, so the
// enum is mirrored here with the same enumerators and nothing else: the
// dispatch itself is what the fixture exercises.
enum class ToolbarVisibilityDecision
{
    Keep,
    Hide,
    Claim,
};

namespace
{
bool transportReady = false;
int handshakeAttempts = 0;
int stoppedTimers = 0;
int armedTimers = 0;
bool threadFocused = true;
bool ownsFocusState = true;
// Inputs to the WM_ThreadFocus dispatch: which way the floating-toolbar decision
// goes for this notification, and whether this instance still owns the thread's
// input focus by the time the handler runs.
ToolbarVisibilityDecision visibilityDecision = ToolbarVisibilityDecision::Claim;
bool threadOwnsFocus = true;
int reconnectMessages = 0;
int connectMessages = 0;
int activationRequests = 0;
int endedClaims = 0;

bool IsNamedpipeThreadFocused()
{
    return threadFocused;
}

bool IsNamedpipeFocusStateOwner(const void *)
{
    return ownsFocusState;
}

void RequireNamedpipeFocusActivation()
{
    ++activationRequests;
}

void PostMessage(HWND, UINT message, int, int)
{
    if (message == WM_IpcReconnect)
        ++reconnectMessages;
    else if (message == WM_ConnectNamedpipe)
        ++connectMessages;
}

bool EnsureNamedpipeFocusSessionActivated()
{
    ++handshakeAttempts;
    return transportReady;
}

void KillTimer(HWND, UINT)
{
    ++stoppedTimers;
}

void SetTimer(HWND, UINT, UINT, void *)
{
    ++armedTimers;
}
} // namespace

class CMetasequoiaIME
{
  public:
    bool _serverUnavailableFallbackActive = true;
    bool _focusLostToWindowsTextInputHost = false;
    bool composing = false;
    bool releaseReset = false;
    std::atomic<uint64_t> _expectedWorkerFocusToken{42};
    std::atomic<uint64_t> _acknowledgedWorkerFocusToken{42};
    std::atomic<bool> _workerCommitReady{true};
    std::atomic<bool> _localSessionResetPending{false};
    std::atomic<UINT> _localSessionResetToken{7};
    UINT _ipcReconnectDelayMs = 100;
    UINT _ipcConsecutiveFailures = 4;
    int snapshots = 0;
    int restoredSnapshots = 0;
    int drains = 0;
    bool fallbackAtSnapshot = true;
    bool fallbackAtDrain = true;

    ToolbarVisibilityDecision _DecideToolbarVisibilityForForegroundWindow(const wchar_t *, bool = true)
    {
        return visibilityDecision;
    }

    bool _ThreadOwnsInputFocus()
    {
        return threadOwnsFocus;
    }

    bool _EndToolbarClaimForForeignForegroundWindow(const wchar_t *)
    {
        ++endedClaims;
        Global::g_connected = false;
        return true;
    }

    void _RefreshLanguageBarThemeIcons()
    {
    }
    void _ClearSmartPunctuationAction()
    {
    }

    bool _IsComposing()
    {
        return composing;
    }

    void _RequestLocalSessionReset(void *, UINT)
    {
        if (releaseReset)
        {
            _localSessionResetPending.store(false);
            composing = false;
        }
    }

    void _TryLeaveServerUnavailableFallback();

    void _ScheduleDeferredKeyDownDrain()
    {
        ++drains;
        fallbackAtDrain = _serverUnavailableFallbackActive;
    }
};

void SendCurrentImeStatusSnapshot(CMetasequoiaIME *service, bool assertsFocusOwnership = false)
{
    ++service->snapshots;
    service->fallbackAtSnapshot = service->_serverUnavailableFallbackActive;
    // FocusRestored is an implicit activation: the Server moves the toolbar
    // route to this client. Counted apart so a plain refresh cannot pass for a
    // claim.
    if (assertsFocusOwnership)
        ++service->restoredSnapshots;
}

// The generator supplies the actual timer/message handlers and fallback transition.
#include "reconnect_recovery_source.h"

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n';                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

int main()
{
    // A key arrived while the Server was restarting. The immediate attempt failed,
    // leaving raw fallback active until the background timer completes the handshake.
    CMetasequoiaIME delayed;
    RunReconnectMessage(delayed);
    CHECK(delayed._serverUnavailableFallbackActive);
    CHECK(delayed.snapshots == 0 && delayed.drains == 0 && armedTimers == 1);
    transportReady = true;
    RunReconnectTimer(delayed);
    CHECK(!delayed._serverUnavailableFallbackActive);
    CHECK(!delayed.fallbackAtSnapshot && !delayed.fallbackAtDrain);
    CHECK(delayed.snapshots == 1 && delayed.drains == 1 && stoppedTimers == 1);
    CHECK(delayed._ipcReconnectDelayMs == CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS);
    CHECK(delayed._ipcConsecutiveFailures == 0);

    // Immediate and delayed success must resume the same input lane.
    CMetasequoiaIME immediate;
    RunReconnectMessage(immediate);
    CHECK(!immediate._serverUnavailableFallbackActive);
    CHECK(!immediate.fallbackAtSnapshot && !immediate.fallbackAtDrain);

    // An offline composition owns its raw text until it has been finalized.
    CMetasequoiaIME composing;
    composing.composing = true;
    RunReconnectTimer(composing);
    CHECK(composing._serverUnavailableFallbackActive && composing.fallbackAtDrain);
    composing.composing = false;
    composing._TryLeaveServerUnavailableFallback();
    CHECK(!composing._serverUnavailableFallbackActive);

    // Invalid or stale worker acknowledgements must not release raw fallback.
    for (int invalid = 0; invalid < 3; ++invalid)
    {
        CMetasequoiaIME unacknowledged;
        if (invalid == 0)
            unacknowledged._expectedWorkerFocusToken.store(0);
        else if (invalid == 1)
            unacknowledged._acknowledgedWorkerFocusToken.store(41);
        else
            unacknowledged._workerCommitReady.store(false);
        unacknowledged._TryLeaveServerUnavailableFallback();
        CHECK(unacknowledged._serverUnavailableFallbackActive);
    }

    // A local reset still waiting for its edit session must block the handshake.
    CMetasequoiaIME resetting;
    resetting._localSessionResetPending.store(true);
    const int attemptsBeforeReset = handshakeAttempts;
    RunReconnectTimer(resetting);
    CHECK(resetting._serverUnavailableFallbackActive);
    CHECK(resetting.snapshots == 0 && resetting.drains == 0);
    CHECK(handshakeAttempts == attemptsBeforeReset);
    resetting.releaseReset = true;
    RunReconnectTimer(resetting);
    CHECK(!resetting._serverUnavailableFallbackActive && !resetting.fallbackAtDrain);

    // A failed handshake, or focus that moved away, cannot restore Server input.
    transportReady = false;
    CMetasequoiaIME unavailable;
    RunReconnectTimer(unavailable);
    CHECK(unavailable._serverUnavailableFallbackActive);
    CHECK(unavailable.snapshots == 0 && unavailable.drains == 0);
    transportReady = true;
    Global::g_connected = false;
    CMetasequoiaIME unfocused;
    const int attemptsBeforeFocusLoss = handshakeAttempts;
    RunReconnectTimer(unfocused);
    CHECK(unfocused._serverUnavailableFallbackActive);
    CHECK(unfocused.snapshots == 0 && unfocused.drains == 0);
    CHECK(handshakeAttempts == attemptsBeforeFocusLoss);

    // Document focus can remain set on a background thread. Neither a queued
    // reconnect message nor a retry timer may activate that thread's session.
    Global::g_connected = true;
    threadFocused = false;
    CMetasequoiaIME background;
    const int timersBeforeBackground = armedTimers;
    RunReconnectMessage(background);
    RunReconnectTimer(background);
    CHECK(handshakeAttempts == attemptsBeforeFocusLoss);
    CHECK(armedTimers == timersBeforeBackground);
    CHECK(background.snapshots == 0 && background.drains == 0);

    // The thread-focus handler dispatches on the floating-toolbar decision and
    // on thread focus, not on whether a document is focused: a per-app-window
    // switch to another input method produces neither a TIP switch nor a
    // document notification, so this callback is the only signal there is.
    threadFocused = true;

    // An instance that does not own this thread's focus state must not touch the
    // Server route, whatever the foreground window says.
    Global::g_connected = false;
    ownsFocusState = false;
    RunThreadFocusMessage(background);
    CHECK(!Global::g_connected && background.snapshots == 0);
    CHECK(reconnectMessages == 0 && connectMessages == 0 && activationRequests == 0);
    ownsFocusState = true;

    // The foreground window names no input method (the secure desktop, the
    // Win+. panel) or this thread's profile is another one: refresh the Server's
    // view and leave the route where it is.
    visibilityDecision = ToolbarVisibilityDecision::Keep;
    RunThreadFocusMessage(background);
    CHECK(!Global::g_connected && background.snapshots == 1);
    CHECK(reconnectMessages == 0 && connectMessages == 0 && activationRequests == 0);

    // The user moved to a window that runs another input method. The claim is
    // retired here instead of being taken back, and no snapshot claims it.
    visibilityDecision = ToolbarVisibilityDecision::Hide;
    RunThreadFocusMessage(background);
    CHECK(endedClaims == 1 && !Global::g_connected && background.snapshots == 1);
    CHECK(reconnectMessages == 0 && connectMessages == 0 && activationRequests == 0);

    // This notification was queued before the user left: the thread no longer
    // owns the input focus, so OnKillThreadFocus already retired the claim and
    // this is the stale half of the flash. A plain refresh only.
    visibilityDecision = ToolbarVisibilityDecision::Claim;
    threadOwnsFocus = false;
    RunThreadFocusMessage(background);
    CHECK(!Global::g_connected && background.snapshots == 2 && background.restoredSnapshots == 0);
    CHECK(reconnectMessages == 0 && connectMessages == 0 && activationRequests == 0);

    // Thread focus came back for this input method after the toolbar was hidden
    // for another one. That re-runs the activation handshake without waiting for
    // a document notification, which a host need not repeat on return.
    threadOwnsFocus = true;
    RunThreadFocusMessage(background);
    CHECK(Global::g_connected && connectMessages == 1 && activationRequests == 1);
    // The handshake restarts the session, so the worker commit gate is closed
    // again: raw fallback outlives the reconnect until the worker acknowledges.
    CHECK(!background._workerCommitReady.load());
    RunReconnectMessage(background);
    CHECK(handshakeAttempts == attemptsBeforeFocusLoss + 1);
    CHECK(background._serverUnavailableFallbackActive);
    background._workerCommitReady.store(true);
    background._TryLeaveServerUnavailableFallback();
    CHECK(!background._serverUnavailableFallbackActive);

    // Returning with a live session restates the route without rotating the
    // activation token: FocusRestored is an implicit activation.
    RunThreadFocusMessage(background);
    CHECK(connectMessages == 1 && activationRequests == 1);
    CHECK(background.snapshots == 4 && background.restoredSnapshots == 1);

    // TextInputHost owns a separate client, so that return is not a claim: it
    // refreshes the view only and keeps the marker for the next handshake.
    background._focusLostToWindowsTextInputHost = true;
    RunThreadFocusMessage(background);
    CHECK(background.snapshots == 5 && background.restoredSnapshots == 1);
    CHECK(connectMessages == 1 && activationRequests == 1);
    CHECK(background._focusLostToWindowsTextInputHost);

    // A later return that has to re-run the handshake clears the marker, so a
    // Win+. round trip cannot leave the route on plain refreshes forever.
    Global::g_connected = false;
    RunThreadFocusMessage(background);
    CHECK(Global::g_connected && connectMessages == 2 && activationRequests == 2);
    CHECK(!background._focusLostToWindowsTextInputHost);

    std::cout << "Reconnect recovery and offline-composition guards passed\n";
    return 0;
}
