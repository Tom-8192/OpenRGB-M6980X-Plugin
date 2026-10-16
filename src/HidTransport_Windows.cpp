/*---------------------------------------------------------*\
| HidTransport_Windows.cpp                                  |
|                                                           |
|   Windows: HID class driver directly (SetupAPI, HidD_*),  |
|   a named mutex as the lock between processes             |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#ifdef _WIN32

#include "HidTransport.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace m6980x
{

namespace
{

/*---------------------------------------------------------*\
| From hidclass.h (DDK), defined here like hidapi does      |
\*---------------------------------------------------------*/
const DWORD     IOCTL_GET_FEATURE   = CTL_CODE(FILE_DEVICE_KEYBOARD, 100, METHOD_OUT_DIRECT, FILE_ANY_ACCESS);

/*---------------------------------------------------------*\
| Same name as m6980x_configtool (transport/windows.rs)     |
\*---------------------------------------------------------*/
const wchar_t   MUTEX_NAME[]        = L"Local\\m6980x_configtool";

std::string Narrow(const std::wstring& s)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::string LastError()
{
    return "Windows error " + std::to_string(GetLastError());
}

class WindowsTransport : public Transport
{
public:
    WindowsTransport(HANDLE dev, HANDLE mutex, const std::string& path)
        : dev(dev), mutex(mutex), path(path), locked(false)
    {
    }

    ~WindowsTransport() override
    {
        Unlock();
        CloseHandle(mutex);
        CloseHandle(dev);
    }

    bool SetFeature(const Report& data) override
    {
        unsigned char buf[REPORT_LEN + 1] = {0};
        memcpy(buf + 1, data.data(), REPORT_LEN);
        return HidD_SetFeature(dev, buf, sizeof(buf)) == TRUE;
    }

    bool GetFeature(Report& data) override
    {
        /*-------------------------------------------------*\
        | IOCTL instead of HidD_GetFeature to learn the     |
        | length: like hidapi, the byte count leaves out    |
        | report ID 0 at buf[0]                             |
        \*-------------------------------------------------*/
        unsigned char   buf[REPORT_LEN + 1] = {0};
        DWORD           returned            = 0;

        if(!DeviceIoControl(dev, IOCTL_GET_FEATURE, buf, sizeof(buf), buf, sizeof(buf), &returned, nullptr))
        {
            return false;
        }

        if(returned < REPORT_LEN)
        {
            return false;
        }

        memcpy(data.data(), buf + 1, REPORT_LEN);
        return true;
    }

    bool Lock() override
    {
        DWORD result = WaitForSingleObject(mutex, LOCK_TIMEOUT_MS);

        if(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
        {
            locked = true;
        }

        return locked;
    }

    void Unlock() override
    {
        if(locked)
        {
            ReleaseMutex(mutex);
            locked = false;
        }
    }

    std::string Path() const override
    {
        return path;
    }

private:
    HANDLE          dev;
    HANDLE          mutex;
    std::string     path;
    bool            locked;
};

}

std::vector<std::string> FindDevicePaths()
{
    std::vector<std::string>    paths;
    GUID                        hid_guid;

    HidD_GetHidGuid(&hid_guid);

    HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if(info == INVALID_HANDLE_VALUE)
    {
        return paths;
    }

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);

    for(DWORD idx = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, idx, &iface); idx++)
    {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &size, nullptr);

        if(size == 0)
        {
            continue;
        }

        std::vector<unsigned char> detail_buf(size);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detail_buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if(!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, size, nullptr, nullptr))
        {
            continue;
        }

        std::wstring path  = detail->DevicePath;
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });

        if(lower.find(L"vid_e0ff&pid_0001") != std::wstring::npos && lower.find(L"&mi_00") != std::wstring::npos)
        {
            paths.push_back(Narrow(path));
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    return paths;
}

std::unique_ptr<Transport> OpenTransport(const std::string& path, std::string& error)
{
    std::wstring wpath = Widen(path);

    /*-----------------------------------------------------*\
    | Windows opens mice exclusively for reading; like      |
    | hidapi fall back to a handle without access rights,   |
    | feature reports still work with it                    |
    \*-----------------------------------------------------*/
    HANDLE dev = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if(dev == INVALID_HANDLE_VALUE)
    {
        dev = CreateFileW(wpath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }

    if(dev == INVALID_HANDLE_VALUE)
    {
        error = "cannot open " + path + ": " + LastError();
        return nullptr;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);

    if(mutex == nullptr)
    {
        error = "cannot create the lock: " + LastError();
        CloseHandle(dev);
        return nullptr;
    }

    return std::unique_ptr<Transport>(new WindowsTransport(dev, mutex, path));
}

}

#endif
