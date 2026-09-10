#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"

namespace {
std::string Utf8(const std::wstring& value) {
  if (value.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), size, nullptr, nullptr);
  return result;
}

std::string JsonString(const std::wstring& value) {
  std::string out = "\"";
  for (unsigned char ch : Utf8(value)) {
    if (ch == '\\' || ch == '\"') { out += '\\'; out += (char)ch; }
    else if (ch == '\n') out += "\\n";
    else if (ch == '\r') out += "\\r";
    else if (ch == '\t') out += "\\t";
    else if (ch < 0x20) out += ' ';
    else out += (char)ch;
  }
  out += "\"";
  return out;
}

void NotifyDoneBubble(const std::wstring& commit) {
  if (commit.empty()) return;
  HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\DoneBubble.RimeInput", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return;
  HWND hwnd = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  wchar_t title[512] = {};
  GetWindowTextW(hwnd, title, ARRAYSIZE(title));
  std::wstring process = L"";
  if (pid != 0) {
    HANDLE target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (target) {
      wchar_t path[MAX_PATH] = {};
      DWORD length = ARRAYSIZE(path);
      if (QueryFullProcessImageNameW(target, 0, path, &length)) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        process = slash ? slash + 1 : path;
      }
      CloseHandle(target);
    }
  }
  std::string payload = "{\"text\":" + JsonString(commit) + ",\"process\":" + JsonString(process) + ",\"window\":" + JsonString(title) + ",\"password\":false}\n";
  DWORD written = 0;
  WriteFile(pipe, payload.data(), (DWORD)payload.size(), &written, nullptr);
  CloseHandle(pipe);
}
}

STDMETHODIMP WeaselTSF::DoEditSession(TfEditCookie ec) {
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  bool ok = m_client.GetResponseData(std::ref(parser));

  _UpdateLanguageBar(_status);

  bool compositionEnded = false;
  if (ok) {
    compositionEnded = false;
    if (!commit.empty()) {
      // For auto-selecting, commit and preedit can both exist.
      // Commit the old TSF composition. If Rime immediately has a new
      // preedit (top-word input), _EndComposition() drops the local pointer
      // synchronously, so the following state check starts a new TSF
      // composition instead of observing the old one.
      if (!_IsComposing()) {
        _StartComposition(_pEditSessionContext,
                          _fCUASWorkaroundEnabled && !config.inline_preedit);
      }
      _InsertText(_pEditSessionContext, commit);
      NotifyDoneBubble(commit);
      // Keep the candidate UI alive while the replacement composition is
      // being created; otherwise the key-down path destroys the old window
      // and the new one cannot be positioned until key-up.
      _EndComposition(_pEditSessionContext, false, !_status.composing);
      compositionEnded = true;
      _committed = TRUE;
    } else {
      _committed = FALSE;
    }
    if (_status.composing && (compositionEnded || !_IsComposing())) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
    } else if (!_status.composing && _IsComposing()) {
      _EndComposition(_pEditSessionContext, true);
    }
    if (_IsComposing() && config.inline_preedit) {
      _ShowInlinePreedit(_pEditSessionContext, context);
    }
  }

  if (ok && !compositionEnded)
    _UpdateCompositionWindow(_pEditSessionContext);
  // Keep the existing candidate window alive during top-word input, but
  // publish the new candidates in this key-down edit session. Positioning is
  // still updated by the queued read session after the new composition is
  // created.
  _UpdateUI(*context, _status);

  return TRUE;
}
