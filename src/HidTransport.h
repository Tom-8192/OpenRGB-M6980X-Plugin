/*---------------------------------------------------------*\
| HidTransport.h                                            |
|                                                           |
|   Feature report transport for the Gigabyte GHOST        |
|   M6980X (USB E0FF:0001, interface 0)                     |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace m6980x
{

constexpr unsigned short    VID                 = 0xE0FF;
constexpr unsigned short    PID                 = 0x0001;
constexpr int               INTERFACE           = 0;
/*---------------------------------------------------------*\
| Data bytes of a feature report; report ID 0 not included  |
\*---------------------------------------------------------*/
constexpr std::size_t       REPORT_LEN          = 8;
/*---------------------------------------------------------*\
| Longest wait for another program (e.g. the M6980X         |
| Configuration Tool) that is talking to the mouse          |
\*---------------------------------------------------------*/
constexpr unsigned int      LOCK_TIMEOUT_MS     = 5000;

typedef std::array<unsigned char, REPORT_LEN> Report;

/*---------------------------------------------------------*\
| The mouse is configured with an 8-byte feature report     |
| without report ID on USB interface 0 (the mouse itself).  |
| Every command runs under a cross-process lock shared with |
| m6980x_configtool, so that two programs never take each   |
| other's replies.                                          |
\*---------------------------------------------------------*/
class Transport
{
public:
    virtual             ~Transport() {}

    virtual bool        SetFeature(const Report& data)  = 0;
    /*-----------------------------------------------------*\
    | Returns false on errors and on short reports (the     |
    | mouse sends 1 byte while the DPI/profile button is    |
    | pressed); the caller treats both as "not ready yet"   |
    \*-----------------------------------------------------*/
    virtual bool        GetFeature(Report& data)        = 0;

    virtual bool        Lock()                          { return true; }
    virtual void        Unlock()                        {}
    virtual std::string Path() const                    { return ""; }
};

/*---------------------------------------------------------*\
| Platform functions (HidTransport_Windows.cpp /            |
| HidTransport_Linux.cpp)                                   |
\*---------------------------------------------------------*/
std::vector<std::string>    FindDevicePaths();
std::unique_ptr<Transport>  OpenTransport(const std::string& path, std::string& error);

}
