#include "Ipc.h"
#include "Private.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "FocusAnnouncementPolicy.h"
#include "ToolbarVisibilityPolicy.h"
#include <debugapi.h>
#include <cwchar>
#include "fmt/xchar.h"

namespace
{
bool IsWindowsTextInputHostWindow(HWND window)
{
    if (!window)
    {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0)
    {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
    {
        return false;
    }

    WCHAR imagePath[32768] = {};
    DWORD imagePathLength = ARRAYSIZE(imagePath);
    const bool queried = QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength) != FALSE;
    CloseHandle(process);
    if (!queried || imagePathLength == 0)
    {
        return false;
    }

    const WCHAR *fileName = wcsrchr(imagePath, L'\\');
    fileName = fileName ? fileName + 1 : imagePath;
    return _wcsicmp(fileName, L"TextInputHost.exe") == 0;
}
} // namespace

bool CMetasequoiaIME::_CaptureWindowsTextInputHostFocusLoss()
{
    _focusLostToWindowsTextInputHost =
        _focusLostToWindowsTextInputHost || IsWindowsTextInputHostWindow(GetForegroundWindow());
    return _focusLostToWindowsTextInputHost;
}

bool CMetasequoiaIME::_IsOwnThreadTheActiveInputMethod()
{
    // ITfActiveLanguageProfileNotifySink::OnActivated is the framework's
    // authoritative profile notification: it fires for the manual switch and
    // for the per-app-window switch alike, so the flag alone says whether
    // this thread's active input method is this text service.
    return _ownProfileActiveInThread;
}

bool CMetasequoiaIME::_ThreadOwnsInputFocus()
{
    // ITfThreadMgr::IsThreadFocus is the framework's own record of which thread
    // owns the input focus, so unlike a foreground window query it cannot lag a
    // window switch. Claiming the floating toolbar has to require it: a focus
    // notification queued before the user left is dispatched after
    // OnKillThreadFocus already ended the claim, and re-claiming there is the
    // half of the flash that no foreground window check can see.
    if (!_pThreadMgr)
    {
        return false;
    }
    BOOL hasThreadFocus = FALSE;
    return SUCCEEDED(_pThreadMgr->IsThreadFocus(&hasThreadFocus)) && hasThreadFocus != FALSE;
}

ToolbarVisibilityDecision CMetasequoiaIME::_DecideToolbarVisibilityForForegroundWindow(const wchar_t *source,
                                                                                       bool logKeepDecision)
{
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundThreadId = 0;
    HKL foregroundLayout = nullptr;
    if (foreground)
    {
        DWORD processId = 0;
        foregroundThreadId = GetWindowThreadProcessId(foreground, &processId);
        if (foregroundThreadId != 0)
        {
            // A TIP profile owns a system-synthesized HKL that is unique per
            // profile (00000804 plain Chinese layout, E0220804 Sogou,
            // E0230804 Bing), so the foreground thread's layout names its
            // input method across processes. A failed query is not a
            // verdict: keep the current visibility.
            foregroundLayout = GetKeyboardLayout(foregroundThreadId);
        }
    }

    const bool foreign = foregroundLayout != nullptr && foregroundLayout != GetKeyboardLayout(GetCurrentThreadId());
    // The Win+. panel is hosted by TextInputHost.exe, so it must not be read as
    // "the user moved to another input method". Telling the two apart costs a
    // process query, and document focus callbacks arrive per edit, so it is
    // only paid when the layouts actually disagree: a foreground window that
    // shares this thread's layout is this input method either way.
    //
    // The foreground window is absent (secure desktop), is the Win+. panel, or
    // could not be resolved: none of those means "the user moved to another
    // input method", so the toolbar stays as it is.
    const bool resolved = foregroundLayout != nullptr && !(foreign && IsWindowsTextInputHostWindow(foreground));
    const ToolbarVisibilityDecision decision = DecideToolbarVisibility(_ownProfileActiveInThread, resolved, foreign);
    if ((logKeepDecision || decision != ToolbarVisibilityDecision::Claim) &&
        Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
    {
        QueueTsfDiagnosticLog(fmt::format(L"[toolbar-visibility] {}: fg={:#x} tid={} hkl_fg={:#x} "
                                          L"hkl_own={:#x} own_profile={} -> {}",
                                          source ? source : L"unspecified", reinterpret_cast<uintptr_t>(foreground),
                                          foregroundThreadId, reinterpret_cast<uintptr_t>(foregroundLayout),
                                          reinterpret_cast<uintptr_t>(GetKeyboardLayout(GetCurrentThreadId())),
                                          _ownProfileActiveInThread,
                                          decision == ToolbarVisibilityDecision::Hide    ? L"hide"
                                          : decision == ToolbarVisibilityDecision::Claim ? L"claim"
                                                                                         : L"keep"));
    }
    return decision;
}

bool CMetasequoiaIME::_ForegroundWindowRunsAnotherInputMethod(bool logKeepDecision)
{
    return _DecideToolbarVisibilityForForegroundWindow(L"focus-gate", logKeepDecision) ==
           ToolbarVisibilityDecision::Hide;
}

bool CMetasequoiaIME::_ForegroundWindowUsesThisInputMethod()
{
    return _DecideToolbarVisibilityForForegroundWindow(L"focus-return", false) == ToolbarVisibilityDecision::Claim;
}

bool CMetasequoiaIME::_EndToolbarClaimForForeignForegroundWindow(const wchar_t *source)
{
    if (_DecideToolbarVisibilityForForegroundWindow(source) != ToolbarVisibilityDecision::Hide)
    {
        return false;
    }

    // End only the Server's active UI route. Keep this TSF instance and its
    // pipe handles alive so returning to this thread can activate it again.
    _CancelForegroundInputMethodCheck();
    if (Global::g_connected)
    {
        FlushNamedpipeImeDeactivation();
        MarkNamedpipeFocusLost();
        Global::g_connected = false;
    }
    return true;
}

void CMetasequoiaIME::_ScheduleForegroundInputMethodCheck()
{
    if (!_msgWndHandle || !IsWindow(_msgWndHandle) || _focusLostToWindowsTextInputHost ||
        _foregroundInputMethodCheckStarted != 0)
    {
        return;
    }

    _foregroundInputMethodCheckStarted = GetTickCount64();
    if (!SetTimer(_msgWndHandle, TIMER_FOREGROUND_INPUT_METHOD_CHECK, FOREGROUND_INPUT_METHOD_CHECK_INTERVAL_MS,
                  nullptr))
    {
        _foregroundInputMethodCheckStarted = 0;
    }
}

void CMetasequoiaIME::_CancelForegroundInputMethodCheck()
{
    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        KillTimer(_msgWndHandle, TIMER_FOREGROUND_INPUT_METHOD_CHECK);
    }
    _foregroundInputMethodCheckStarted = 0;
}

void CMetasequoiaIME::_ScheduleFocusedInputModeAnnouncement()
{
    // Re-arming collapses the document- and thread-focus callbacks of one
    // switch (Chromium fires a burst) into a single announcement.
    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        SetTimer(_msgWndHandle, TIMER_FOCUS_CARET_STATE, FOCUS_CARET_STATE_DELAY_MS, nullptr);
    }
}

void CMetasequoiaIME::_AnnounceFocusedInputMode()
{
    if (!IsNamedpipeFocusStateOwner(this) || !Global::g_connected || !_pThreadMgr || !_pCompositionProcessorEngine)
    {
        return;
    }
    // Focus may have moved on again while the timer was pending.
    BOOL hasThreadFocus = FALSE;
    if (FAILED(_pThreadMgr->IsThreadFocus(&hasThreadFocus)) || !hasThreadFocus)
    {
        return;
    }
    // The user is already typing into this field; a field change in the
    // middle of a composition is rare, a host's transient document is not.
    if (_IsComposing())
    {
        return;
    }
    // The badge request resolves the caret itself; a document without a
    // caret (non-edit control) resolves nothing and stays silent. Whether the
    // user enabled this announcement is the Server's decision.
    const bool imeOpen = _pCompositionProcessorEngine->GetIMEMode(_pThreadMgr, _tfClientId) != FALSE;
    const bool capsLockEnabled = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    _pCompositionProcessorEngine->SendCaretStateSwitchEvent(FanyImePipeEventType::IMESwitch, imeOpen,
                                                            FanyImeCaretStateTrigger::FocusEntered, capsLockEnabled);
}

//+---------------------------------------------------------------------------
//
// ITfTextLayoutSink::OnSetThreadFocus
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnSetThreadFocus()
{
    if (!IsNamedpipeFocusStateOwner(this))
    {
        return S_OK;
    }
    _CancelForegroundInputMethodCheck();
    // Do not consume this marker here. TextInputHost owns a separate TSF
    // client, so document focus or its deferred thread-focus recovery must use
    // it to force a new Server activation even when the pipes still look live.
    // The owner window rechecks actual thread focus before resuming a pending
    // connection and refreshing status. Document focus can arrive while this
    // thread is still in the background and need not be repeated on return.
    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_ThreadFocus, 0, 0);
    }
    // Some window switches surface only as thread focus, with no document
    // focus callback, so the return itself has to count.
    if (ShouldAnnounceThreadFocusReturn(_threadFocusLostForAnnouncement, _focusLostToWindowsTextInputHost))
    {
        _ScheduleFocusedInputModeAnnouncement();
    }
    _threadFocusLostForAnnouncement = false;
    if (_pCandidateListUIPresenter)
    {
        ITfDocumentMgr *pCandidateListDocumentMgr = nullptr;
        ITfContext *pTfContext = _pCandidateListUIPresenter->_GetContextDocument();

        if ((nullptr != pTfContext) && SUCCEEDED(pTfContext->GetDocumentMgr(&pCandidateListDocumentMgr)))
        {
            if (pCandidateListDocumentMgr == _pDocMgrLastFocused)
            {
                _pCandidateListUIPresenter->OnSetThreadFocus();
            }

            pCandidateListDocumentMgr->Release();
        }
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfTextLayoutSink::OnKillThreadFocus
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnKillThreadFocus()
{
    if (!IsNamedpipeFocusStateOwner(this) || !Global::g_connected)
    {
        return S_OK;
    }
    // Losing thread focus ends the input burst the action was armed in. The
    // focus token would already reject it on interception; clearing here keeps
    // the stale action from outliving the session it describes.
    _ClearSmartPunctuationAction();
    // The same burst owns the Backspace hold guard; it must never suppress
    // Backspace in whatever context gains focus next (#347).
    _backspaceHoldArmed = false;
    _focusLostToWindowsTextInputHost = false;
    (void)_CaptureWindowsTextInputHostFocusLoss();
    // Some hosts report only thread-focus loss on an application switch.
    // Same-process auxiliary windows and TextInputHost do not end the preview.
    if (_voiceCompositionActive && _IsComposing() && !_focusLostToWindowsTextInputHost &&
        !_localSessionResetPending.load(std::memory_order_acquire))
    {
        DWORD foregroundProcessId = 0;
        const HWND foreground = GetForegroundWindow();
        if (foreground)
            GetWindowThreadProcessId(foreground, &foregroundProcessId);
        if (foregroundProcessId != 0 && foregroundProcessId != GetCurrentProcessId())
        {
            // Queued keys belong to the preview being discarded. Use the
            // existing local cancellation, without rotating the pipe session.
            _ResetSessionAfterFailure(DeferredKeyFailureKind::Resync);
        }
    }
    _threadFocusLostForAnnouncement = true;
    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        KillTimer(_msgWndHandle, TIMER_FOCUS_CARET_STATE);
    }

    // "Let me use a different input method for each app window" moves the user
    // to a window whose active input method is not this text service without
    // routing through Deactivate() or OnActivated(FALSE) here: this thread's
    // profile never changed, and the focused thread never activated us. Thread
    // focus loss is therefore the only observable signal. Mirror
    // OnActivated(isActivated = FALSE): terminal deactivation of the UI session
    // (the Server hides the toolbar) without TIP teardown, so the pipes and
    // the profile survive the trip back.
    if (!_EndToolbarClaimForForeignForegroundWindow(L"focus-left") && _ForegroundWindowUsesThisInputMethod())
    {
        // The foreground HWND may still identify the old window during this
        // callback. Recheck on the message loop until it settles or the
        // deferral timer decides; only the hide is ever taken from those
        // rechecks, so a switch that lands late still hides the toolbar
        // promptly. A foreground that names no input method at all (the Win+.
        // panel, the secure desktop) is left alone, exactly as before.
        _ScheduleForegroundInputMethodCheck();
    }

    // ITfThreadFocusSink reports temporary auxiliary UI transitions as well
    // as real application focus changes. The known-good implementation did
    // not use this callback as an IPC/session boundary: document/top-context
    // changes are already handled by ITfThreadMgrEventSink, and TIP switching
    // is handled by Deactivate. Ordinary compositions remain intact here,
    // especially for TextInputHost.exe / Win+.. Voice previews are cancelled
    // above only when another application has actually become foreground, and
    // that block is the single exception: a per-app-window switch to another
    // input method produces no same-thread TIP switch, so this callback is the
    // only observable signal that the user moved to a window not using this tip.
    if (_pCandidateListUIPresenter)
    {
        ITfDocumentMgr *pCandidateListDocumentMgr = nullptr;
        ITfContext *pTfContext = _pCandidateListUIPresenter->_GetContextDocument();

        if ((nullptr != pTfContext) && SUCCEEDED(pTfContext->GetDocumentMgr(&pCandidateListDocumentMgr)))
        {
            if (_pDocMgrLastFocused)
            {
                _pDocMgrLastFocused->Release();
                _pDocMgrLastFocused = nullptr;
            }
            // GetDocumentMgr already returned one owned reference. Transfer it
            // directly to _pDocMgrLastFocused; adding another reference here
            // leaks one on every Win+. / thread-focus round trip.
            _pDocMgrLastFocused = pCandidateListDocumentMgr;
            pCandidateListDocumentMgr = nullptr;
        }
        _pCandidateListUIPresenter->OnKillThreadFocus();
    }
    return S_OK;
}

BOOL CMetasequoiaIME::_InitThreadFocusSink()
{
    ITfSource *pSource = nullptr;

    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource)))
    {
        return FALSE;
    }

    if (FAILED(pSource->AdviseSink(IID_ITfThreadFocusSink, (ITfThreadFocusSink *)this, &_dwThreadFocusSinkCookie)))
    {
        pSource->Release();
        return FALSE;
    }

    pSource->Release();

    return TRUE;
}

void CMetasequoiaIME::_UninitThreadFocusSink()
{
    ITfSource *pSource = nullptr;

    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource)))
    {
        return;
    }

    if (FAILED(pSource->UnadviseSink(_dwThreadFocusSinkCookie)))
    {
        pSource->Release();
        return;
    }

    pSource->Release();
}
