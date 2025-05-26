/*
 *  ---------------------------------------------------------------------------
 *  GLPI-AgentMonitor.cpp
 *  Copyright (C) 2023, 2025 Leonardo Bernardes (redddcyclone)
 *  ---------------------------------------------------------------------------
 * 
 *  LICENSE
 * 
 *  This file is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *
 *  This file is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA,
 *  or see <http://www.gnu.org/licenses/>.
 * 
 *  ---------------------------------------------------------------------------
 * 
 *  @author(s) Leonardo Bernardes (redddcyclone)
 *  @license   GNU GPL version 2 or (at your option) any later version
 *             http://www.gnu.org/licenses/old-licenses/gpl-2.0-standalone.html
 *  @since     2023
 * 
 *  ---------------------------------------------------------------------------
 */


//-[LIBRARIES]-----------------------------------------------------------------

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Shlwapi.lib")


//-[DEFINES]-------------------------------------------------------------------

#define SERVICE_NAME L"GLPI-Agent"
#define USERAGENT_NAME L"GLPI-AgentMonitor"


//-[INCLUDES]------------------------------------------------------------------

#include <vector>
#include <string>
#include <windows.h>
#include <winhttp.h>
#include <winuser.h>
#include <shellapi.h>
#include <CommCtrl.h>
#include <gdiplus.h>
#include <Shlwapi.h>
#include <Tlhelp32.h>
#include <ShlObj.h>
#include "framework.h"
#include "resource.h"


//-[GLOBALS AND OTHERS]--------------------------------------------------------

using namespace std;

// Main window message processing callback
LRESULT CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
// Settings dialog message processing callback
LRESULT CALLBACK SettingsDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

// App instance
HINSTANCE hInst;

// App mutex (to prevent multiple instances)
HANDLE hMutex;

// EnumWindows callback data
struct EnumWindowsData {
    DWORD dwSearchPID;
    HWND hWndFound;
};

// WinHTTP connection and session handles
HINTERNET hSession, hConn;

// Runtime variables
BOOL bAgentInstalled = true;
BOOL glpiAgentOk = true;
SERVICE_STATUS svcStatus;
SERVICE_STATUS lastSvcStatus = {};
WCHAR szAgStatus[128];

// WinHTTP request handle
HINTERNET hReq = NULL;

// GDI+ related
Gdiplus::GdiplusStartupInput gdiplusStartupInput;
ULONG_PTR gdiplusToken;

// Taskbar icon identifier
NOTIFYICONDATA nid = { sizeof(nid) };
// Taskbar icon interaction message ID
UINT const WMAPP_NOTIFYCALLBACK = WM_APP + 1;

// Dynamic text colors
COLORREF colorSvcStatus = RGB(0, 0, 0);

// GLPI server URL
WCHAR szServer[256];

// New ticket URL
WCHAR szNewTicketURL[300];

// Agent logfile
WCHAR szLogfile[MAX_PATH];

// Enable screenshot capture
BOOL bNewTicketScreenshot = TRUE;

// Global string buffer
WCHAR szBuffer[256];
DWORD dwBufferLen = sizeof(szBuffer) / sizeof(WCHAR);


//-[APP FUNCTIONS]-------------------------------------------------------------

// Load resource embedded PNG file as a bitmap 
BOOL LoadPNGAsBitmap(HMODULE hInstance, LPCWSTR pName, LPCWSTR pType, HBITMAP *bitmap) 
{
    Gdiplus::Bitmap* m_pBitmap;

    HRSRC hResource = FindResource(hInstance, pName, pType);
    if (!hResource)
        return false;
    DWORD imageSize = SizeofResource(hInstance, hResource);
    if (!imageSize)
        return false;
    HGLOBAL tempRes = LoadResource(hInstance, hResource);
    if (!tempRes)
        return false;
    const void* pResourceData = LockResource(tempRes);
    if (!pResourceData)
        return false;

    HGLOBAL m_hBuffer = GlobalAlloc(GMEM_MOVEABLE, imageSize);
    if (m_hBuffer) {
        void* pBuffer = GlobalLock(m_hBuffer);
        if (pBuffer) {
            CopyMemory(pBuffer, pResourceData, imageSize);
            IStream* pStream = NULL;
            if (CreateStreamOnHGlobal(m_hBuffer, FALSE, &pStream) == S_OK) {
                m_pBitmap = Gdiplus::Bitmap::FromStream(pStream);
                pStream->Release();
                if (m_pBitmap) {
                    if (m_pBitmap->GetLastStatus() == Gdiplus::Ok) {
                        m_pBitmap->GetHBITMAP(0, bitmap);
                        return true;
                    }
                    delete m_pBitmap;
                    m_pBitmap = NULL;
                }
            }
            m_pBitmap = NULL;
            GlobalUnlock(m_hBuffer);
        }
        GlobalFree(m_hBuffer);
        m_hBuffer = NULL;
    }
    return false;
}

// Shows a window and force it to appear over all others
VOID ShowWindowFront(HWND hWnd, int nCmdShow) 
{
    ShowWindow(hWnd, nCmdShow);
    HWND hCurWnd = GetForegroundWindow();
    DWORD dwMyID = GetCurrentThreadId();
    DWORD dwCurID = GetWindowThreadProcessId(hCurWnd, NULL);
    AttachThreadInput(dwCurID, dwMyID, TRUE);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    SetActiveWindow(hWnd);
    AttachThreadInput(dwCurID, dwMyID, FALSE);
}

// Loads the specified strings from the resources and shows a message box
VOID LoadStringAndMessageBox(HINSTANCE hIns, HWND hWn, UINT msgResId, UINT titleResId, UINT mbFlags, UINT errCode = NULL)
{
    WCHAR szBuf[256], szTitleBuf[128];
    LoadString(hIns, msgResId, szBuf, sizeof(szBuf) / sizeof(WCHAR));
    LoadString(hIns, titleResId, szTitleBuf, sizeof(szTitleBuf) / sizeof(WCHAR));
    if (errCode != NULL)
    {
        LPWSTR errMsgBuf = nullptr;
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, errCode, GetUserDefaultUILanguage(), (LPWSTR)&errMsgBuf, 0, NULL);
        wsprintf(szBuf, L"%s\n\n0x%08x - %s", szBuf, errCode, errMsgBuf);
        LocalFree(errMsgBuf);
    }
    MessageBox(hWn, szBuf, szTitleBuf, mbFlags);
}

// Unsets the asynchronous callback and close the WinHttp handle
VOID CloseWinHttpRequest(HINTERNET hInternet) {
    WinHttpSetStatusCallback(hInternet, NULL, NULL, NULL);
    WinHttpCloseHandle(hInternet);
    hReq = NULL;
}

// Callback called by the asynchronous WinHTTP request
VOID CALLBACK WinHttpCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus, LPVOID lpvStatusInfo, DWORD dwStatusInfoLength)
{
    switch (dwInternetStatus)
    {
        // Request is sent, receive response
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            if (!WinHttpReceiveResponse(hInternet, NULL)) {
                CloseWinHttpRequest(hInternet);
            }
            break;

        // Response headers are available, query for data
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            if (!WinHttpQueryDataAvailable(hInternet, NULL)) {
                CloseWinHttpRequest(hInternet);
            }
            break;

        // Data is available, read it
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
            DWORD dwSize = *(LPDWORD)lpvStatusInfo;
            DWORD dwDownloaded;
            CHAR szResponse[128] = "";
            DWORD dwResponseLen = sizeof(szResponse);

            // If the response size is greater than expected, set "Agent not responding" string and close the WinHTTP handle
            if (dwSize >= dwResponseLen) {
                LoadString(hInst, IDS_ERR_NOTRESPONDING, szAgStatus, sizeof(szAgStatus) / sizeof(WCHAR));
                SetDlgItemText((HWND)dwContext, IDC_AGENTSTATUS, szAgStatus);
                CloseWinHttpRequest(hInternet);
                break;
            }

            if (dwSize == 0 || !WinHttpReadData(hInternet, &szResponse, dwSize, &dwDownloaded)) {
                CloseWinHttpRequest(hInternet);
                break;
            }

            // If the number of downloaded bytes is equal or greater than expected, set "Agent not responding" string and close the WinHTTP handle
            if(dwDownloaded >= dwResponseLen) {
                LoadString(hInst, IDS_ERR_NOTRESPONDING, szAgStatus, sizeof(szAgStatus) / sizeof(WCHAR));
                SetDlgItemText((HWND)dwContext, IDC_AGENTSTATUS, szAgStatus);
                CloseWinHttpRequest(hInternet);
                break;
            }

            // Set last character to null
            szResponse[dwDownloaded] = '\0';

            size_t cnvChars = 0;

            // Must remove "status: " from the string (dwDownloaded - 7, szResponse[8])
            mbstowcs_s(&cnvChars, szAgStatus, (size_t)dwDownloaded - 7, (LPCSTR)&szResponse[8], _TRUNCATE);
            SetDlgItemText((HWND)dwContext, IDC_AGENTSTATUS, szAgStatus);
            break;
        }

        // Set "Agent not responding" string on error and close the WinHTTP handle (on both error and read complete)
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            LoadString(hInst, IDS_ERR_NOTRESPONDING, szAgStatus, sizeof(szAgStatus) / sizeof(WCHAR));
            SetDlgItemText((HWND)dwContext, IDC_AGENTSTATUS, szAgStatus);
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            CloseWinHttpRequest(hInternet);
            break;
    }
}

// Requests GLPI Agent status via HTTP (asynchronous)
VOID GetAgentStatus(HWND hWnd)
{
    if (svcStatus.dwCurrentState == SERVICE_RUNNING)
    {
        // Only do another request if the previous one is closed
        if (hReq == NULL)
        {
            hReq = WinHttpOpenRequest(hConn, L"GET", L"/status", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_BYPASS_PROXY_CACHE);

            // Callback is set for this request only, not for the entire WinHTTP session,
            // as "Force Inventory" is synchronous
            WinHttpSetStatusCallback(hReq, WinHttpCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);

            WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, NULL, WINHTTP_NO_REQUEST_DATA, NULL, NULL, (DWORD_PTR)hWnd);
        }
    }
}

// Requests an inventory via HTTP (synchronous)
VOID ForceInventory(HWND hWnd)
{
    if (svcStatus.dwCurrentState == SERVICE_RUNNING)
    {
        if (glpiAgentOk)
        {
            HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", L"/now", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_BYPASS_PROXY_CACHE);

            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, NULL, WINHTTP_NO_REQUEST_DATA, NULL, NULL, NULL)) 
            {
                if (WinHttpReceiveResponse(hReq, NULL)) {
                    DWORD dwStatusCode = 0;
                    DWORD dwSize = sizeof(dwStatusCode);

                    if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX))
                    {
                        LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_FORCEINV_NORESPONSE, IDS_ERROR, MB_OK | MB_ICONERROR);
                    }
                    else {
                        if (dwStatusCode != 200)
                            LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_FORCEINV_NOTALLOWED, IDS_ERROR, MB_OK | MB_ICONERROR);
                        else
                            LoadStringAndMessageBox(hInst, hWnd, IDS_MSG_FORCEINV_OK, IDS_APP_TITLE, MB_OK | MB_ICONINFORMATION);
                    }
                }
            }
            WinHttpCloseHandle(hReq);
        }
        else
            LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_AGENTERR, IDS_ERROR, MB_OK | MB_ICONERROR);
    }
    else
        LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_NOTRUNNING, IDS_ERROR, MB_OK | MB_ICONERROR);
}

// Updates service related statuses
VOID CALLBACK UpdateServiceStatus(HWND hWnd, UINT message, UINT idTimer, DWORD dwTime) {
    WCHAR szBtnString[32];
    DWORD dwBtnStringLen = sizeof(szBtnString) / sizeof(WCHAR);
    BOOL bQuerySvcOk = FALSE;
    BOOL bSvcChangedState = FALSE;

    // Open Service Manager and Agent service handle and query Agent service status
    SC_HANDLE hSc = OpenSCManager(NULL, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (hSc != NULL) {
        SC_HANDLE hAgentSvc = OpenService(hSc, SERVICE_NAME, SERVICE_QUERY_STATUS);

        if (hAgentSvc != NULL) {
            bQuerySvcOk = QueryServiceStatus(hAgentSvc, &svcStatus);
            CloseServiceHandle(hAgentSvc);
        }

        CloseServiceHandle(hSc);
    }

    if (!bQuerySvcOk || !bAgentInstalled) {
        if (IsWindowVisible(hWnd)) {
            LoadString(hInst, IDS_ERR_SERVICE, szBuffer, dwBufferLen);
            LoadString(hInst, IDS_STARTSVC, szBtnString, dwBtnStringLen);
            colorSvcStatus = RGB(255, 0, 0);
            SetDlgItemText(hWnd, IDC_SERVICESTATUS, szBuffer);
            SetDlgItemText(hWnd, IDC_BTN_STARTSTOPSVC, szBtnString);
            HWND hWndSvcButton = GetDlgItem(hWnd, IDC_BTN_STARTSTOPSVC);
            EnableWindow(hWndSvcButton, FALSE);
            SetFocus(hWndSvcButton);
        }

        LoadIconMetric(hInst, MAKEINTRESOURCE(IDI_GLPIERR), LIM_LARGE, &nid.hIcon);
        LoadString(hInst, IDS_GLPINOTIFYERROR, nid.szTip, ARRAYSIZE(nid.szTip));
        nid.szInfo[0] = '\0';
        Shell_NotifyIcon(NIM_MODIFY, &nid);
        glpiAgentOk = false;
    }
    else if (bQuerySvcOk) {
        bSvcChangedState = svcStatus.dwCurrentState != lastSvcStatus.dwCurrentState;
    }

    if (bSvcChangedState)
    {
        BOOL bEnableButton = FALSE;
        switch (svcStatus.dwCurrentState)
        {
            case SERVICE_STOPPED:
                LoadString(hInst, IDS_SVC_STOPPED, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_STARTSVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 0, 0);
                bEnableButton = TRUE;
                break;
            case SERVICE_RUNNING:
                LoadString(hInst, IDS_SVC_RUNNING, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_STOPSVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(0, 127, 0);
                bEnableButton = TRUE;
                break;
            case SERVICE_PAUSED:
                LoadString(hInst, IDS_SVC_PAUSED, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_RESUMESVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 165, 0);
                bEnableButton = TRUE;
                break;
            case SERVICE_CONTINUE_PENDING:
                LoadString(hInst, IDS_SVC_CONTINUEPENDING, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_RESUMESVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 165, 0);
                break;
            case SERVICE_PAUSE_PENDING:
                LoadString(hInst, IDS_SVC_PAUSEPENDING, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_STOPSVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 165, 0);
                break;
            case SERVICE_START_PENDING:
                LoadString(hInst, IDS_SVC_STARTPENDING, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_STARTSVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 165, 0);
                break;
            case SERVICE_STOP_PENDING:
                LoadString(hInst, IDS_SVC_STOPPENDING, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_STOPSVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 165, 0);
                break;
            default:
                LoadString(hInst, IDS_ERR_SERVICE, szBuffer, dwBufferLen);
                LoadString(hInst, IDS_STARTSVC, szBtnString, dwBtnStringLen);
                colorSvcStatus = RGB(255, 0, 0);
                break;
        }
        SetDlgItemText(hWnd, IDC_SERVICESTATUS, szBuffer);
        SetDlgItemText(hWnd, IDC_BTN_STARTSTOPSVC, szBtnString);

        LoadString(hInst, (svcStatus.dwCurrentState == SERVICE_STOPPED ? IDS_ERR_NOTRUNNING : IDS_WAIT), szBuffer, dwBufferLen);
        SetDlgItemText(hWnd, IDC_AGENTSTATUS, szBuffer);

        HWND hWndSvcButton = GetDlgItem(hWnd, IDC_BTN_STARTSTOPSVC);
        EnableWindow(hWndSvcButton, bEnableButton);
        if (IsWindowVisible(hWnd)) {
            SetFocus(hWndSvcButton);
        }

        // Taskbar icon routine
        if (svcStatus.dwCurrentState != SERVICE_RUNNING)
        {
            if (glpiAgentOk)
            {
                LoadIconMetric(hInst, MAKEINTRESOURCE(IDI_GLPIERR), LIM_LARGE, &nid.hIcon);
                LoadString(hInst, IDS_GLPINOTIFYERROR, nid.szTip, ARRAYSIZE(nid.szTip));
                nid.szInfo[0] = '\0';
                Shell_NotifyIcon(NIM_MODIFY, &nid);
                glpiAgentOk = false;
            }
        }
        else
        {
            if (!glpiAgentOk)
            {
                LoadIconMetric(hInst, MAKEINTRESOURCE(IDI_GLPIOK), LIM_LARGE, &nid.hIcon);
                LoadString(hInst, IDS_GLPINOTIFY, nid.szTip, ARRAYSIZE(nid.szTip));
                nid.szInfo[0] = '\0';
                Shell_NotifyIcon(NIM_MODIFY, &nid);
                glpiAgentOk = true;
            }
        }

        lastSvcStatus.dwCurrentState = svcStatus.dwCurrentState;
    }
}

// Updates the main window statuses
VOID CALLBACK UpdateStatus(HWND hWnd, UINT message, UINT idTimer, DWORD dwTime)
{
    if (IsWindowVisible(hWnd))
    {
        // Agent version
        HKEY hk;
        LONG lRes;
        WCHAR szKey[MAX_PATH];
        DWORD dwValue = -1;
        DWORD dwValueLen = sizeof(dwValue);

        // We can find the agent version under the Installer subkey, value "Version"
        lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GLPI-Agent\\Installer", 0, KEY_READ | KEY_WOW64_64KEY, &hk);
        if (lRes != ERROR_SUCCESS)
        {
            lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GLPI-Agent\\Installer", 0, KEY_READ | KEY_WOW64_64KEY, &hk);
            if (lRes != ERROR_SUCCESS) {
                LoadString(hInst, IDS_ERR_AGENTNOTFOUND, szBuffer, dwBufferLen);
                SetDlgItemText(hWnd, IDC_AGENTVER, szBuffer);
                bAgentInstalled = false;
            }
        }
        if (lRes == ERROR_SUCCESS)
        {
            WCHAR szValue[128] = {};
            DWORD szValueLen = sizeof(szValue);
            lRes = RegQueryValueEx(hk, L"Version", 0, NULL, (LPBYTE)szValue, &szValueLen);
            if (lRes != ERROR_SUCCESS)
                LoadString(hInst, IDS_ERR_AGENTVERNOTFOUND, szBuffer, dwBufferLen);
            else
                wsprintf(szBuffer, L"GLPI Agent %s", szValue);
            SetDlgItemText(hWnd, IDC_AGENTVER, szBuffer);
            bAgentInstalled = true;
        }


        // Startup type
        wsprintf(szKey, L"SYSTEM\\CurrentControlSet\\Services\\%s", SERVICE_NAME);
        lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ | KEY_WOW64_64KEY, &hk);
        if (lRes == ERROR_SUCCESS)
        {
            lRes = RegQueryValueEx(hk, L"Start", 0, NULL, (LPBYTE)&dwValue, &dwValueLen);
            if (lRes == ERROR_SUCCESS)
            {
                switch (dwValue)
                {
                    case SERVICE_BOOT_START:
                        LoadString(hInst, IDS_SVCSTART_BOOT, szBuffer, dwBufferLen);
                        break;
                    case SERVICE_SYSTEM_START:
                        LoadString(hInst, IDS_SVCSTART_SYSTEM, szBuffer, dwBufferLen);
                        break;
                    case SERVICE_AUTO_START:
                    {
                        lRes = RegQueryValueEx(hk, L"DelayedAutostart", 0, NULL, (LPBYTE)&dwValue, &dwValueLen);
                        if (lRes == ERROR_SUCCESS)
                            LoadString(hInst, IDS_SVCSTART_DELAYEDAUTO, szBuffer, dwBufferLen);
                        else
                            LoadString(hInst, IDS_SVCSTART_AUTO, szBuffer, dwBufferLen);
                        break;
                    }
                    case SERVICE_DEMAND_START:
                        LoadString(hInst, IDS_SVCSTART_MANUAL, szBuffer, dwBufferLen);
                        break;
                    case SERVICE_DISABLED:
                        LoadString(hInst, IDS_SVCSTART_DISABLED, szBuffer, dwBufferLen);
                        break;
                    default:
                        LoadString(hInst, IDS_ERR_UNKSVCSTART, szBuffer, dwBufferLen);
                }
                SetDlgItemText(hWnd, IDC_STARTTYPE, szBuffer);
            }
            else
            {
                LoadString(hInst, IDS_ERR_UNKSVCSTART, szBuffer, dwBufferLen);
                SetDlgItemText(hWnd, IDC_STARTTYPE, szBuffer);
            }
        }
        else
        {
            LoadString(hInst, IDS_ERR_REGFAIL, szBuffer, dwBufferLen);
            SetDlgItemText(hWnd, IDC_STARTTYPE, szBuffer);
        }

        RegCloseKey(hk);

        // Update agent status
        // If the service is not running, the status will
        // be replaced by UpdateServiceStatus
        if (svcStatus.dwCurrentState == SERVICE_RUNNING) {
            GetAgentStatus(hWnd);
        }
    }
}

// EnumWindows callback
// (called by EnumWindows to find the hWnd for the running GLPI Agent Monitor instance)
BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    EnumWindowsData& ed = *(EnumWindowsData*)lParam;
    DWORD dwPID = 0x0;
    GetWindowThreadProcessId(hWnd, &dwPID);
    if (ed.dwSearchPID == dwPID)
    {
        ed.hWndFound = hWnd;
        SetLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return TRUE;
}

// Get running Monitor process' hWnd
HWND GetRunningMonitorHwnd() {

    // Get GLPI Agent Monitor executable name
    WCHAR szFilePath[MAX_PATH];
    GetModuleFileName(NULL, szFilePath, MAX_PATH);
    LPWSTR szFileName = PathFindFileName(szFilePath);

    // Look through processes, try to get it's hWnd when the process is found
    // and then send a WM_CLOSE message to stop it gracefully
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, NULL);
    PROCESSENTRY32 pEntry{};
    pEntry.dwSize = sizeof(pEntry);
    BOOL hRes = Process32First(hSnapshot, &pEntry);
    while (hRes)
    {
        if (lstrcmp(pEntry.szExeFile, szFileName) == 0)
        {
            // Don't close itself
            if (pEntry.th32ProcessID != GetCurrentProcessId())
            {
                EnumWindowsData ed = {
                    pEntry.th32ProcessID,
                    NULL
                };

                if (!EnumWindows(EnumWindowsProc, (LPARAM)&ed) && (GetLastError() == ERROR_SUCCESS)) {
                    CloseHandle(hSnapshot);
                    return ed.hWndFound;
                }
            }
        }
        hRes = Process32Next(hSnapshot, &pEntry);
    }
    CloseHandle(hSnapshot);

    return NULL;
}


//-[MAIN FUNCTIONS]------------------------------------------------------------

// Entry point
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    hInst = hInstance;
    DWORD dwErr = NULL;

    // Process service operations
    if (wcsstr(lpCmdLine, L"/startSvc") != nullptr || wcsstr(lpCmdLine, L"/stopSvc") != nullptr ||
        wcsstr(lpCmdLine, L"/continueSvc") != nullptr) {
        SC_HANDLE hSc = OpenSCManager(NULL, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT);
        if (!hSc) {
            dwErr = GetLastError();
            LoadStringAndMessageBox(hInst, NULL, IDS_ERR_SCHANDLE, IDS_ERROR, MB_OK | MB_ICONERROR, dwErr);
            return dwErr;
        }
        SC_HANDLE hAgentSvc = OpenService(hSc, SERVICE_NAME, SERVICE_START | SERVICE_PAUSE_CONTINUE | SERVICE_STOP);
        if (!hAgentSvc) {
            dwErr = GetLastError();
            LoadStringAndMessageBox(hInst, NULL, IDS_ERR_SVCHANDLE, IDS_ERROR, MB_OK | MB_ICONERROR, dwErr);
            return dwErr;
        }
        if (wcsstr(lpCmdLine, L"/startSvc") != nullptr)
            StartService(hAgentSvc, NULL, NULL);
        else if (wcsstr(lpCmdLine, L"/stopSvc") != nullptr)
            ControlService(hAgentSvc, SERVICE_CONTROL_STOP, &svcStatus);
        else if (wcsstr(lpCmdLine, L"/continueSvc") != nullptr)
            ControlService(hAgentSvc, SERVICE_CONTROL_CONTINUE, &svcStatus);
        dwErr = GetLastError();
        if (dwErr != NULL) {
            LoadStringAndMessageBox(hInst, NULL, IDS_ERR_SVCOPERATION, IDS_ERROR, MB_OK | MB_ICONERROR, dwErr);
            return dwErr;
        }
        CloseServiceHandle(hAgentSvc);
        CloseServiceHandle(hSc);
        return 0;
    }

    // Process elevation request (kill previous process)
    if (wcsstr(lpCmdLine, L"/elevate") != nullptr)
    {
        HWND runningMonitorHwnd = GetRunningMonitorHwnd();
        if (runningMonitorHwnd != NULL) {
            SendMessage(runningMonitorHwnd, WM_CLOSE, 0xBEBAF7F3, 0xC0CAF7F3);
        }
    }


    // Create app mutex to keep only one instance running
    hMutex = CreateMutex(NULL, TRUE, L"GLPI-AgentMonitor");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Wait for mutex to be available if trying to elevate
        // as the other Monitor instance may not close instantly
        if (wcsstr(lpCmdLine, L"/elevate") != nullptr)
        {
            do {
                hMutex = CreateMutex(NULL, TRUE, L"GLPI-AgentMonitor");
            } while (WaitForSingleObject(hMutex, 500) != WAIT_OBJECT_0);
        }
        else {
            // Show running agent window
            HWND runningMonitorHwnd = GetRunningMonitorHwnd();
            if (runningMonitorHwnd != NULL) {
                ShowWindowFront(runningMonitorHwnd, SW_SHOW);
                UpdateStatus(runningMonitorHwnd, NULL, NULL, NULL);
            }
            return 0;
        }
    }


    // Read app version
    WCHAR szFileName[MAX_PATH];
    GetModuleFileName(NULL, szFileName, MAX_PATH);
    DWORD dwSize = GetFileVersionInfoSize(szFileName, 0);
    VS_FIXEDFILEINFO* lpFfi = NULL;
    UINT uFfiLen = 0;
    WCHAR* szVerBuffer = new WCHAR[dwSize];
    GetFileVersionInfo(szFileName, 0, dwSize, szVerBuffer);
    VerQueryValue(szVerBuffer, L"\\", (LPVOID*)&lpFfi, &uFfiLen);
    DWORD dwVerMaj = HIWORD(lpFfi->dwFileVersionMS);
    DWORD dwVerMin = LOWORD(lpFfi->dwFileVersionMS);
    DWORD dwVerRev = HIWORD(lpFfi->dwFileVersionLS);

    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Load agent settings
    HKEY hk;
    WCHAR szKey[MAX_PATH];
    WCHAR szValueBuf[MAX_PATH] = {};
    DWORD szValueBufLen = sizeof(szValueBuf);
    DWORD szServerLen = sizeof(szServer);
    DWORD dwPort = 62354;

    wsprintf(szKey, L"SOFTWARE\\%s", SERVICE_NAME);
    LONG lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ | KEY_WOW64_64KEY, &hk);
    if (lRes != ERROR_SUCCESS)
    {
        wsprintf(szKey, L"SOFTWARE\\WOW6432Node\\%s", SERVICE_NAME);
        lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ | KEY_WOW64_64KEY, &hk);
        if (lRes != ERROR_SUCCESS) {
            LoadStringAndMessageBox(hInst, NULL, IDS_ERR_AGENTSETTINGS, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
            return lRes;
        }
    }
    else
    {   
        // Get HTTPD port
        lRes = RegQueryValueEx(hk, L"httpd-port", 0, NULL, (LPBYTE)szValueBuf, &szValueBufLen);
        if (lRes != ERROR_SUCCESS) {
            LoadStringAndMessageBox(hInst, NULL, IDS_ERR_HTTPDPORT, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
            return lRes;
        }
        else
            dwPort = _wtoi(szValueBuf);

        // Get server URL
        lRes = RegQueryValueEx(hk, L"server", 0, NULL, (LPBYTE)szServer, &szServerLen);
        if (lRes == ERROR_SUCCESS) {
            // Strip any quotes from the "server" value
            wstring strServer = szServer;
            strServer.erase(remove(strServer.begin(), strServer.end(), '\''), strServer.end());
            strServer.erase(remove(strServer.begin(), strServer.end(), '\"'), strServer.end());
            wcscpy_s(szServer, strServer.c_str());

            if (wcscmp(L"", szServer) != 0) {
                // Get only the first URL if more than one is configured
                LPWSTR szSubstr = wcsstr(szServer, L",");
                if (szSubstr != nullptr)
                    szSubstr[0] = '\0';
                // Get GLPI server base URL (as GLPI may be located in a subfolder,
                // we can't guess the exact location just by stripping the domain
                // from the "server" parameter).
                szSubstr = wcsstr(szServer, L"/plugins/");
                if (szSubstr == nullptr) {
                    szSubstr = wcsstr(szServer, L"/marketplace/");
                    if (szSubstr == nullptr)
                        szSubstr = wcsstr(szServer, L"/front/inventory.php");
                }
                if (szSubstr != nullptr) {
                    // As szSubstr points to where the first character of the substring
                    // was found in the string itself, replacing it with a null character
                    // effectively cuts the string.
                    szSubstr[0] = '\0';
                }
                // Strip any trailing slash
                size_t lServerLen = wcslen(szServer);
                while (--lServerLen >= 0 && szServer[lServerLen] == '/') {
                    szServer[lServerLen] = '\0';
                }
                // In case we didn't find the substrings, assume the "server" value
                // in the registry is the base GLPI url itself.
            }
        }

        // Get agent logfile path
        DWORD szLogfileLen = sizeof(szLogfile);
        lRes = RegQueryValueEx(hk, L"logfile", 0, NULL, (LPBYTE)szLogfile, &szLogfileLen);
        if (lRes != ERROR_SUCCESS)
            szLogfile[0] = '\0';


        // Load GLPI Agent Monitor settings from registry
        wsprintf(szKey, L"SOFTWARE\\%s\\Monitor", SERVICE_NAME);
        LONG lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ | KEY_WOW64_64KEY, &hk);
        if (lRes != ERROR_SUCCESS)
        {
            wsprintf(szKey, L"SOFTWARE\\WOW6432Node\\%s\\Monitor", SERVICE_NAME);
            lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ | KEY_WOW64_64KEY, &hk);
        }

        // Here I won't check if the key could be opened, as
        // this is already checked when RegQueryValueEx below
        // does not return ERROR_SUCCESS.
        
        // Get new ticket URL
        DWORD szNewTicketURLLen = sizeof(szNewTicketURL);
        lRes = RegQueryValueEx(hk, L"NewTicket-URL", 0, NULL, (LPBYTE)szNewTicketURL, &szNewTicketURLLen);
        if (lRes != ERROR_SUCCESS || !wcscmp(szNewTicketURL, L"")) {
            // Default value if not found or empty
            if (wcsncmp(L"https://", szServer, 8) != 0 && wcsncmp(L"http://", szServer, 7) != 0) {
                // Place an "http://" before the URL so that it is at least opened by the system's
                // default browser instead of doing nothing or unexpected behavior, even if
                // for some reason the Agent's "server" parameter is empty
                wsprintf(szNewTicketURL, L"http://%s/front/ticket.form.php", szServer);
            }
            else {
                wsprintf(szNewTicketURL, L"%s/front/ticket.form.php", szServer);
            }
        }

        // Get new ticket screenshot enable
        DWORD dwNewTicketScreenshotTmp = NULL;
        DWORD dwNewTicketScreenshotLen = sizeof(dwNewTicketScreenshotTmp);
        lRes = RegQueryValueExW(hk, L"NewTicket-Screenshot", 0, NULL, (LPBYTE)&dwNewTicketScreenshotTmp, &dwNewTicketScreenshotLen);
        if (lRes == ERROR_SUCCESS) {
            bNewTicketScreenshot = (dwNewTicketScreenshotTmp == 1);
        }
    }

    RegCloseKey(hk);

    // Create WinHTTP handles
    WCHAR szUserAgent[64];
    wsprintf(szUserAgent, L"%s/%d.%d.%d", USERAGENT_NAME, dwVerMaj, dwVerMin, dwVerRev);

    hSession = WinHttpOpen(szUserAgent, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    WinHttpSetTimeouts(hSession, 100, 10000, 10000, 10000);
    hConn = WinHttpConnect(hSession, L"127.0.0.1", (INTERNET_PORT)dwPort, 0);

    //-------------------------------------------------------------------------

    HWND hWnd = CreateDialog(hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, (DLGPROC)DlgProc);
    if (!hWnd) {
        dwErr = GetLastError();
        LoadStringAndMessageBox(hInst, NULL, IDS_ERR_MAINWINDOW, IDS_ERROR, MB_OK | MB_ICONERROR, dwErr);
        return dwErr;
    }

    HICON icon = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_GLPIOK), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);

    // Taskbar icon
    nid.hWnd = hWnd;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
    LoadIconMetric(hInst, MAKEINTRESOURCE(IDI_GLPIOK), LIM_LARGE, &nid.hIcon);
    LoadString(hInst, IDS_GLPINOTIFY, nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIcon(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &nid);

    HBITMAP hLogo = nullptr;
    LoadPNGAsBitmap(hInst, MAKEINTRESOURCE(IDB_LOGO), L"PNG", &hLogo);
    SendMessage(GetDlgItem(hWnd, IDC_PCLOGO), STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hLogo);

    WCHAR szVer[20];
    wsprintf(szVer, L"v%d.%d.%d", dwVerMaj, dwVerMin, dwVerRev);
    SetDlgItemText(hWnd, IDC_VERSION, szVer);

    LoadString(hInst, IDS_APP_TITLE, szBuffer, dwBufferLen);
    SetWindowText(hWnd, szBuffer);
    SetDlgItemText(hWnd, IDC_STATIC_TITLE, szBuffer);

    LoadString(hInst, IDS_STATIC_INFO, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_GBMAIN, szBuffer);

    LoadString(hInst, IDS_STATIC_AGENTVER, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_STATIC_AGENTVER, szBuffer);
    LoadString(hInst, IDS_STATIC_SERVICESTATUS, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_STATIC_SERVICESTATUS, szBuffer);
    LoadString(hInst, IDS_STATIC_STARTTYPE, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_STATIC_STARTTYPE, szBuffer);

    LoadString(hInst, IDS_LOADING, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_AGENTVER, szBuffer);
    SetDlgItemText(hWnd, IDC_SERVICESTATUS, szBuffer);
    SetDlgItemText(hWnd, IDC_STARTTYPE, szBuffer);
    SetDlgItemText(hWnd, IDC_AGENTSTATUS, szBuffer);

    LoadString(hInst, IDS_STATIC_AGENTSTATUS, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_GBSTATUS, szBuffer);

    LoadString(hInst, IDS_FORCEINV, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_BTN_FORCE, szBuffer);
    LoadString(hInst, IDS_VIEWLOGS, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_BTN_VIEWLOGS, szBuffer);
    LoadString(hInst, IDS_NEWTICKET, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_BTN_NEWTICKET, szBuffer);
    LoadString(hInst, IDS_BTN_SETTINGS, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_BTN_SETTINGS, szBuffer);
    LoadString(hInst, IDS_CLOSE, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_BTN_CLOSE, szBuffer);
    LoadString(hInst, IDS_STARTSVC, szBuffer, dwBufferLen);
    SetDlgItemText(hWnd, IDC_BTN_STARTSTOPSVC, szBuffer);

    // Set UAC shields
    SendMessage(GetDlgItem(hWnd, IDC_BTN_STARTSTOPSVC), BCM_SETSHIELD, 0, 1);
    SendMessage(GetDlgItem(hWnd, IDC_BTN_SETTINGS), BCM_SETSHIELD, 0, 1);

    //-------------------------------------------------------------------------

    UpdateStatus(hWnd, NULL, NULL, NULL);
    UpdateServiceStatus(hWnd, NULL, NULL, NULL);
    SetTimer(hWnd, IDT_UPDSTATUS, 2000, (TIMERPROC)UpdateStatus);
    SetTimer(hWnd, IDT_UPDSVCSTATUS, 500, (TIMERPROC)UpdateServiceStatus);

    //-------------------------------------------------------------------------

    // Show window if elevation request was made
    if (wcsstr(lpCmdLine, L"/elevate") != nullptr)
    {
        ShowWindowFront(hWnd, SW_SHOW);
        UpdateStatus(hWnd, NULL, NULL, NULL);

        // Show settings dialog immediately if requested
        if (wcsstr(lpCmdLine, L"/openSettings") != nullptr) {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_DLG_SETTINGS), hWnd, (DLGPROC)SettingsDlgProc);
        }
    }

    // Main message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessage(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}

LRESULT CALLBACK SettingsDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_INITDIALOG:
        {
            // Initialize dialog strings
            LoadString(hInst, IDS_SETTINGS, szBuffer, dwBufferLen);
            SetWindowText(hWnd, szBuffer);
            LoadString(hInst, IDS_SETTINGS_NEWTICKET, szBuffer, dwBufferLen);
            SetDlgItemText(hWnd, IDC_SETTINGS_GROUPBOX_NEWTICKET, szBuffer);
            LoadString(hInst, IDS_SETTINGS_NEWTICKET_URL, szBuffer, dwBufferLen);
            SetDlgItemText(hWnd, IDC_SETTINGS_TEXT_NEWTICKET_URL, szBuffer);
            LoadString(hInst, IDS_SETTINGS_NEWTICKET_SCREENSHOT, szBuffer, dwBufferLen);
            SetDlgItemText(hWnd, IDC_SETTINGS_CHECKBOX_NEWTICKET_SCREENSHOT, szBuffer);
            LoadString(hInst, IDS_CANCEL, szBuffer, dwBufferLen);
            SetDlgItemText(hWnd, IDC_SETTINGS_BTN_CANCEL, szBuffer);
            LoadString(hInst, IDS_SAVE, szBuffer, dwBufferLen);
            SetDlgItemText(hWnd, IDC_SETTINGS_BTN_SAVE, szBuffer);

            // Fill values
            SendDlgItemMessage(hWnd, IDC_SETTINGS_EDIT_NEWTICKET_URL, WM_SETTEXT, 0, (LPARAM)szNewTicketURL);
            SendDlgItemMessage(hWnd, IDC_SETTINGS_CHECKBOX_NEWTICKET_SCREENSHOT, BM_SETCHECK, (bNewTicketScreenshot ? BST_CHECKED : BST_UNCHECKED), 0);

            return TRUE;
        }
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case IDC_SETTINGS_EDIT_NEWTICKET_URL:
                case IDC_SETTINGS_CHECKBOX_NEWTICKET_SCREENSHOT:
                    // Prevent closing the dialog when clicking these components
                    return TRUE;
                case IDC_SETTINGS_BTN_SAVE:
                {
                    HKEY hk, hkMonitor;
                    LONG lRes;
                    WCHAR szKey[MAX_PATH];

                    // Get settings from dialog
                    WCHAR szTempNewTicketURL[300];
                    LRESULT copiedChars = SendMessage(GetDlgItem(hWnd, IDC_SETTINGS_EDIT_NEWTICKET_URL), WM_GETTEXT,
                        sizeof(szTempNewTicketURL), (LPARAM)szTempNewTicketURL);
                    szTempNewTicketURL[copiedChars] = '\0';
                    BOOL bTempNewTicketScreenshot = IsDlgButtonChecked(hWnd, IDC_SETTINGS_CHECKBOX_NEWTICKET_SCREENSHOT);

                    // Save settings in registry
                    wsprintf(szKey, L"SOFTWARE\\%s", SERVICE_NAME);
                    lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_WRITE | KEY_WOW64_64KEY, &hk);
                    if (lRes != ERROR_SUCCESS)
                    {
                        if (lRes == ERROR_FILE_NOT_FOUND)
                        {
                            wsprintf(szKey, L"SOFTWARE\\WOW6432Node\\%s", SERVICE_NAME);
                            lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_WRITE | KEY_WOW64_64KEY, &hk);
                            if (lRes != ERROR_SUCCESS)
                            {
                                LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_SAVE_SETTINGS, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
                                return FALSE;
                            }
                        }
                        else {
                            LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_SAVE_SETTINGS, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
                            return FALSE;
                        }
                    }
                    wsprintf(szKey, L"%s\\Monitor", szKey);
                    lRes = RegOpenKeyEx(HKEY_LOCAL_MACHINE, szKey, 0, KEY_WRITE | KEY_WOW64_64KEY, &hkMonitor);
                    if (lRes != ERROR_SUCCESS)
                    {
                        lRes = RegCreateKeyEx(hk, L"Monitor", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkMonitor, NULL);
                        if (lRes != ERROR_SUCCESS)
                        {
                            LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_SAVE_SETTINGS, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
                            return FALSE;
                        }
                    }

                    // Save new ticket URL if it was changed
                    if (wcscmp(szTempNewTicketURL, szNewTicketURL) != 0)
                    {
                        size_t szTempNewTicketURLLen = wcslen(szTempNewTicketURL) * sizeof(WCHAR);
                        lRes = RegSetValueEx(hkMonitor, L"NewTicket-URL", 0, REG_SZ, (LPBYTE)szTempNewTicketURL, (DWORD)szTempNewTicketURLLen);
                        if (lRes != ERROR_SUCCESS)
                        {
                            LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_SAVE_SETTINGS, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
                            return FALSE;
                        }
                    }

                    // Save new ticket screenshot setting if it was changed
                    // Save new ticket URL if it was changed
                    if (bTempNewTicketScreenshot != bNewTicketScreenshot)
                    {
                        lRes = RegSetValueEx(hkMonitor, L"NewTicket-Screenshot", 0, REG_DWORD,
                            (LPBYTE)&bTempNewTicketScreenshot, sizeof(bTempNewTicketScreenshot));
                        if (lRes != ERROR_SUCCESS)
                        {
                            LoadStringAndMessageBox(hInst, hWnd, IDS_ERR_SAVE_SETTINGS, IDS_ERROR, MB_OK | MB_ICONERROR, lRes);
                            return FALSE;
                        }
                    }

                    RegCloseKey(hkMonitor);
                    RegCloseKey(hk);

                    // Use the default new ticket URL if the provided one is empty
                    if (!wcscmp(szTempNewTicketURL, L"")) {
                        wsprintf(szNewTicketURL, L"%s/front/ticket.form.php", szServer);
                    }
                    else {
                        // Store new ticket URL in memory
                        wcscpy_s(szNewTicketURL, szTempNewTicketURL);
                    }
                    bNewTicketScreenshot = bTempNewTicketScreenshot;

                    PostMessage(hWnd, WM_CLOSE, 0, 0);
                    return TRUE;
                }
                case IDC_SETTINGS_BTN_CANCEL:
                    PostMessage(hWnd, WM_CLOSE, 0, 0);
                    return TRUE;
            }
        }
        case WM_CLOSE:
        {
            EndDialog(hWnd, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

LRESULT CALLBACK DlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                // Start/stop/resume service
                case IDC_BTN_STARTSTOPSVC:
                    WCHAR szFilename[MAX_PATH];
                    WCHAR szOperation[16];
                    GetModuleFileName(NULL, szFilename, MAX_PATH);
                    switch(svcStatus.dwCurrentState) {
                        case SERVICE_RUNNING:
                            wsprintf(szOperation, L"/stopSvc");
                            break;
                        case SERVICE_PAUSED:
                            wsprintf(szOperation, L"/continueSvc");
                            break;
                        case SERVICE_STOPPED:
                            wsprintf(szOperation, L"/startSvc");
                            break;
                        default:
                            return FALSE;
                    }
                    ShellExecute(hWnd, L"runas", szFilename, szOperation, NULL, SW_HIDE);
                    return TRUE;
                // Force inventory
                case IDC_BTN_FORCE:
                case ID_RMENU_FORCE:
                    ForceInventory(hWnd);
                    return TRUE;
                // Close
                case IDCANCEL:  // This handles ESC key pressing via IsDialogMessage
                case IDC_BTN_CLOSE:
                    EndDialog(hWnd, NULL);
                    return TRUE;
                // Open
                case ID_RMENU_OPEN:
                    ShowWindowFront(hWnd, SW_SHOW);
                    UpdateStatus(hWnd, NULL, NULL, NULL);
                    return TRUE;
                // View logs
                case IDC_BTN_VIEWLOGS:
                case ID_RMENU_VIEWLOGS:
                    ShellExecute(NULL, L"open", szLogfile, NULL, NULL, SW_SHOWNORMAL);
                    return TRUE;
                // New ticket
                case IDC_BTN_NEWTICKET:
                    EndDialog(hWnd, NULL);
                case ID_RMENU_NEWTICKET: {
                    if (bNewTicketScreenshot) {
                        // Take screenshot to clipboard (simulating PrintScreen)
                        INPUT ipInput[2] = { 0 };
                        Sleep(300);
                        ipInput[0].type = INPUT_KEYBOARD;
                        ipInput[0].ki.wVk = VK_SNAPSHOT;
                        ipInput[1] = ipInput[0];
                        ipInput[1].ki.dwFlags |= KEYEVENTF_KEYUP;
                        SendInput(2, ipInput, sizeof(INPUT));
                    }
                    // Open the new ticket URL
                    ShellExecute(NULL, L"open", szNewTicketURL, NULL, NULL, SW_SHOWNORMAL);

                    if (bNewTicketScreenshot) {
                        // Notify user that a screenshot is in the clipboard
                        nid.uFlags |= NIF_INFO;
                        LoadString(hInst, IDS_NOTIF_NEWTICKET_TITLE, nid.szInfoTitle, sizeof(nid.szInfoTitle) / sizeof(WCHAR));
                        LoadString(hInst, IDS_NOTIF_NEWTICKET, nid.szInfo, sizeof(nid.szInfo) / sizeof(WCHAR));
                        Shell_NotifyIcon(NIM_MODIFY, &nid);
                    }

                    return TRUE;
                }
                // Settings
                case ID_RMENU_SETTINGS:
                case IDC_BTN_SETTINGS:
                {
                    if(IsUserAnAdmin()) 
                    {
                        if (LOWORD(wParam) == ID_RMENU_SETTINGS)
                        {
                            ShowWindowFront(hWnd, SW_SHOW);
                            UpdateStatus(hWnd, NULL, NULL, NULL);
                        }
                        DialogBox(hInst, MAKEINTRESOURCE(IDD_DLG_SETTINGS), hWnd, (DLGPROC)SettingsDlgProc);
                    }
                    else
                    {
                        WCHAR szFilename[MAX_PATH];
                        GetModuleFileName(NULL, szFilename, MAX_PATH);
                        ShellExecute(hWnd, L"runas", szFilename, L"/elevate /openSettings", NULL, SW_SHOWNORMAL);
                    }
                    return TRUE;
                }
                // Exit
                case ID_RMENU_EXIT:
                    // wParam and lParam are randomly chosen values
                    PostMessage(hWnd, WM_CLOSE, 0xBEBAF7F3, 0xC0CAF7F3);
                    return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            switch(GetDlgCtrlID((HWND)lParam))
            {
                case IDC_SERVICESTATUS:
                    SetTextColor(hdc, colorSvcStatus);
                    return (LRESULT)GetSysColorBrush(COLOR_MENU);
            }
            break;
        }
        // Taskbar icon callback
        case WMAPP_NOTIFYCALLBACK:
        {
            switch (LOWORD(lParam))
            {
                // Left click
                case NIN_SELECT:
                    ShowWindowFront(hWnd, SW_SHOW);
                    UpdateStatus(hWnd, NULL, NULL, NULL);
                    return TRUE;
                // Right click
                case WM_CONTEXTMENU:
                {
                    POINT const pt = { LOWORD(wParam), HIWORD(wParam) };
                    HMENU hMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_RMENU));
                    if (hMenu)
                    {
                        MENUITEMINFO mi = { sizeof(MENUITEMINFO) };
                        GetMenuItemInfo(hMenu, ID_RMENU_OPEN, false, &mi);
                        mi.fMask = MIIM_TYPE | MIIM_DATA;

                        LoadString(hInst, IDS_RMENU_OPEN, szBuffer, dwBufferLen);
                        mi.dwTypeData = szBuffer;
                        SetMenuItemInfo(hMenu, ID_RMENU_OPEN, false, &mi);
                        LoadString(hInst, IDS_RMENU_FORCE, szBuffer, dwBufferLen);
                        mi.dwTypeData = szBuffer;
                        SetMenuItemInfo(hMenu, ID_RMENU_FORCE, false, &mi);
                        LoadString(hInst, IDS_RMENU_VIEWLOGS, szBuffer, dwBufferLen);
                        mi.dwTypeData = szBuffer;
                        SetMenuItemInfo(hMenu, ID_RMENU_VIEWLOGS, false, &mi);
                        LoadString(hInst, IDS_RMENU_NEWTICKET, szBuffer, dwBufferLen);
                        mi.dwTypeData = szBuffer;
                        SetMenuItemInfo(hMenu, ID_RMENU_NEWTICKET, false, &mi);
                        LoadString(hInst, IDS_RMENU_SETTINGS, szBuffer, dwBufferLen);
                        mi.dwTypeData = szBuffer;
                        SetMenuItemInfo(hMenu, ID_RMENU_SETTINGS, false, &mi);
                        LoadString(hInst, IDS_RMENU_EXIT, szBuffer, dwBufferLen);
                        mi.dwTypeData = szBuffer;
                        SetMenuItemInfo(hMenu, ID_RMENU_EXIT, false, &mi);

                        // Set UAC shield (a bit ugly, especially in Windows 11)
                        int cx = GetSystemMetrics(SM_CXSMICON);
                        int cy = GetSystemMetrics(SM_CYSMICON);
                        HICON hShield = NULL;
                        HRESULT hr = LoadIconWithScaleDown(NULL, IDI_SHIELD, cx, cy, &hShield);
                        if (SUCCEEDED(hr))
                        {
                            ICONINFO iconInfo;
                            GetIconInfo(hShield, &iconInfo);
                            HBITMAP bitmap = (HBITMAP)CopyImage(iconInfo.hbmColor, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
                            SetMenuItemBitmaps(hMenu, ID_RMENU_SETTINGS, MF_BYCOMMAND, bitmap, bitmap);
                            DestroyIcon(hShield);
                        }

                        HMENU hSubMenu = GetSubMenu(hMenu, 0);
                        if (hSubMenu)
                        {
                            SetForegroundWindow(hWnd);
                            UINT uFlags = TPM_RIGHTBUTTON;
                            if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
                                uFlags |= TPM_RIGHTALIGN;
                            else
                                uFlags |= TPM_LEFTALIGN;
                            TrackPopupMenuEx(hSubMenu, uFlags, pt.x, pt.y, hWnd, NULL);
                        }

                        DestroyMenu(hMenu);
                    }
                    return TRUE;
                }
            }
            break;
        }
        // Restart Manager
        case WM_QUERYENDSESSION:
        {
            if (lParam == ENDSESSION_CLOSEAPP) {
                SetWindowLongPtr(hWnd, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
            break;
        }
        case WM_ENDSESSION:
        {
            if (lParam == ENDSESSION_CLOSEAPP && wParam == TRUE) {
                PostMessage(hWnd, WM_CLOSE, 0xBEBAF7F3, 0xC0CAF7F3);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
        {
            // Right-click Exit button
            if (wParam == 0xBEBAF7F3 && lParam == 0xC0CAF7F3)
                DestroyWindow(hWnd);
            else
                EndDialog(hWnd, NULL);
            return TRUE;
        }
        case WM_DESTROY:
        {
            WinHttpCloseHandle(hConn);
            WinHttpCloseHandle(hSession);

            Gdiplus::GdiplusShutdown(gdiplusToken);

            // Remove taskbar icon
            Shell_NotifyIcon(NIM_DELETE, &nid);

            // Release mutex
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);

            PostQuitMessage(0);
            return TRUE;
        }
    }
    return FALSE;
}

