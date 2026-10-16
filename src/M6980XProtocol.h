/*---------------------------------------------------------*\
| M6980XProtocol.h                                          |
|                                                           |
|   The few commands of the Gigabyte GHOST M6980X the       |
|   plugin needs (identify, active profile, LED colour).    |
|   Port of m6980x-core/src/protocol.rs; evidence for every |
|   command: m6980x-driver docs/PROTOCOL.md                 |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <chrono>
#include <string>

#include "HidTransport.h"

namespace m6980x
{

constexpr unsigned char     STATUS_BUSY     = 0x80;
constexpr unsigned char     STATUS_DONE     = 0x40;

constexpr unsigned char     GROUP_SYSTEM    = 0x00;
constexpr unsigned char     GROUP_REGISTER  = 0x01;

constexpr unsigned char     REG_LED         = 0x06;
constexpr unsigned char     REG_PROFILE     = 0x08;
constexpr unsigned char     WRITE_BIT       = 0x80;

constexpr unsigned int      PROFILES        = 3;
/*---------------------------------------------------------*\
| Per LED channel: 0 off, 1 half, 2 full                    |
\*---------------------------------------------------------*/
constexpr unsigned char     LED_LEVELS      = 3;

/*---------------------------------------------------------*\
| R, G, B as levels 0-2                                     |
\*---------------------------------------------------------*/
typedef std::array<unsigned char, 3> Levels;

enum class Status
{
    Ok,
    Locked,         /* another program held the lock for 5 s    */
    Busy,           /* the mouse never reported "not busy"      */
    NoReply,        /* the mouse never reported "done"          */
    SendFailed,     /* SET_FEATURE failed                       */
    Invalid,        /* argument out of range                    */
    BadReply,       /* reply out of range                       */
    NotApplied,     /* read back differs from what was written  */
};

const char* StatusText(Status status);

/*---------------------------------------------------------*\
| 0-255 to the nearest level and back                       |
\*---------------------------------------------------------*/
unsigned char   LevelFromByte(unsigned char value);
unsigned char   ByteFromLevel(unsigned char level);

struct Identity
{
    bool        signature_ok;   /* reply bytes 1-2 = 0C 12      */
    bool        is_6980x;
    std::string raw;
};

class Mouse
{
public:
    explicit Mouse(Transport& transport);

    /*-----------------------------------------------------*\
    | Timing of the original Macro.exe; tests set the       |
    | delays to zero                                        |
    \*-----------------------------------------------------*/
    std::chrono::milliseconds   poll_interval;
    std::chrono::milliseconds   write_delay;
    unsigned int                busy_tries;
    unsigned int                done_tries;

    Status  Identify(Identity& identity);
    Status  ActiveProfile(unsigned char& profile);
    /*-----------------------------------------------------*\
    | Raw bytes as the mouse reports them; normally levels  |
    | 0-2, but values written by other software (0-255)     |
    | come back unchanged                                   |
    \*-----------------------------------------------------*/
    Status  LedColor(unsigned char profile, Levels& levels);
    /*-----------------------------------------------------*\
    | Reads the colour first and only writes when it        |
    | differs (the mouse keeps LED colours without the      |
    | "save" command, so they presumably go to its flash);  |
    | written tells whether 01 86 was sent                  |
    \*-----------------------------------------------------*/
    Status  SetLedColor(unsigned char profile, const Levels& levels, bool& written);

private:
    Status  Xfer(unsigned char group, unsigned char cmd, const unsigned char* args, std::size_t count,
                 std::chrono::milliseconds delay, Report& reply);
    Status  Handshake(const Report& packet, std::chrono::milliseconds delay, Report& reply);
    void    Pause(std::chrono::milliseconds duration);

    Transport& transport;
};

}
