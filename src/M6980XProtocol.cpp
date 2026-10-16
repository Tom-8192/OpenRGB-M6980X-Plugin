/*---------------------------------------------------------*\
| M6980XProtocol.cpp                                        |
|                                                           |
|   Command layer of the Gigabyte GHOST M6980X              |
|                                                           |
|   Packet on the wire (8 bytes, no report ID):             |
|   [group, command, arg0..arg5]. Handshake from the        |
|   original Macro.exe (0x444aac):                          |
|     1. GET_FEATURE until status bit 0x80 (busy) is clear  |
|     2. SET_FEATURE with the command                       |
|     3. GET_FEATURE until status bit 0x40 (done) is set;   |
|        reply = [status, data0..data6]                     |
|   Reply bytes a command does not fill keep stale values.  |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "M6980XProtocol.h"

#include <cstdio>
#include <thread>

namespace m6980x
{

const char* StatusText(Status status)
{
    switch(status)
    {
        case Status::Ok:            return "ok";
        case Status::Locked:        return "another program is using the mouse";
        case Status::Busy:          return "the mouse stays busy";
        case Status::NoReply:       return "no reply from the mouse";
        case Status::SendFailed:    return "sending to the mouse failed";
        case Status::Invalid:       return "invalid argument";
        case Status::BadReply:      return "unexpected reply";
        case Status::NotApplied:    return "the mouse did not take the value";
    }

    return "unknown";
}

unsigned char LevelFromByte(unsigned char value)
{
    if(value < 64)
    {
        return 0;
    }

    if(value < 192)
    {
        return 1;
    }

    return 2;
}

unsigned char ByteFromLevel(unsigned char level)
{
    static const unsigned char bytes[LED_LEVELS] = { 0x00, 0x80, 0xFF };

    /*-----------------------------------------------------*\
    | Values above 2 were written by other software as      |
    | 0-255 colours; show them as they are                  |
    \*-----------------------------------------------------*/
    return(level < LED_LEVELS ? bytes[level] : level);
}

Mouse::Mouse(Transport& transport)
    : poll_interval(4), write_delay(10), busy_tries(20), done_tries(200), transport(transport)
{
}

Status Mouse::Identify(Identity& identity)
{
    Report reply;
    Status status = Xfer(GROUP_SYSTEM, 0x00, nullptr, 0, std::chrono::milliseconds(0), reply);

    if(status != Status::Ok)
    {
        return status;
    }

    std::string raw;

    for(std::size_t idx = 1; idx < REPORT_LEN; idx++)
    {
        char hex[4];
        snprintf(hex, sizeof(hex), idx == 1 ? "%02X" : " %02X", reply[idx]);
        raw += hex;
    }

    /*-----------------------------------------------------*\
    | Model as in the original (0x42ebd8), data = reply     |
    | from byte 1: data[6] = 1 and data[2] = 9 is the       |
    | 6980X; M8000X/M8600/6980 have another memory layout   |
    \*-----------------------------------------------------*/
    const unsigned char* data   = reply.data() + 1;
    identity.signature_ok       = data[0] == 0x0C && data[1] == 0x12;
    identity.is_6980x           = identity.signature_ok && data[6] == 1 && data[2] == 9;
    identity.raw                = raw;

    return Status::Ok;
}

Status Mouse::ActiveProfile(unsigned char& profile)
{
    Report reply;
    Status status = Xfer(GROUP_REGISTER, REG_PROFILE, nullptr, 0, std::chrono::milliseconds(0), reply);

    if(status != Status::Ok)
    {
        return status;
    }

    if(reply[1] >= PROFILES)
    {
        return Status::BadReply;
    }

    profile = reply[1];
    return Status::Ok;
}

Status Mouse::LedColor(unsigned char profile, Levels& levels)
{
    if(profile >= PROFILES)
    {
        return Status::Invalid;
    }

    Report reply;
    Status status = Xfer(GROUP_REGISTER, REG_LED, &profile, 1, std::chrono::milliseconds(0), reply);

    if(status != Status::Ok)
    {
        return status;
    }

    /*-----------------------------------------------------*\
    | Raw values: other software (e.g. an older OpenRGB     |
    | fork) has written 0-255 values, which the mouse keeps |
    | and reports back as they are                          |
    \*-----------------------------------------------------*/
    for(std::size_t channel = 0; channel < levels.size(); channel++)
    {
        levels[channel] = reply[1 + channel];
    }

    return Status::Ok;
}

Status Mouse::SetLedColor(unsigned char profile, const Levels& levels, bool& written)
{
    written = false;

    if(profile >= PROFILES)
    {
        return Status::Invalid;
    }

    for(unsigned char level : levels)
    {
        if(level >= LED_LEVELS)
        {
            return Status::Invalid;
        }
    }

    Levels current;
    Status status = LedColor(profile, current);

    if(status != Status::Ok)
    {
        return status;
    }

    if(current == levels)
    {
        return Status::Ok;
    }

    /*-----------------------------------------------------*\
    | 01 86 <profile> R G B like the original (0x445ba8),   |
    | no trailer, 10 ms; does not switch the profile        |
    \*-----------------------------------------------------*/
    const unsigned char args[4] = { profile, levels[0], levels[1], levels[2] };
    Report              reply;

    status = Xfer(GROUP_REGISTER, REG_LED | WRITE_BIT, args, sizeof(args), write_delay, reply);

    if(status != Status::Ok)
    {
        return status;
    }

    written = true;

    status = LedColor(profile, current);

    if(status != Status::Ok)
    {
        return status;
    }

    return(current == levels ? Status::Ok : Status::NotApplied);
}

Status Mouse::Xfer(unsigned char group, unsigned char cmd, const unsigned char* args, std::size_t count,
                   std::chrono::milliseconds delay, Report& reply)
{
    if(count > 6)
    {
        return Status::Invalid;
    }

    Report packet = {0};
    packet[0]     = group;
    packet[1]     = cmd;

    for(std::size_t idx = 0; idx < count; idx++)
    {
        packet[2 + idx] = args[idx];
    }

    if(!transport.Lock())
    {
        return Status::Locked;
    }

    Status status = Handshake(packet, delay, reply);
    transport.Unlock();

    return status;
}

Status Mouse::Handshake(const Report& packet, std::chrono::milliseconds delay, Report& reply)
{
    bool ready = false;

    for(unsigned int attempt = 0; attempt < busy_tries; attempt++)
    {
        Report state;

        if(transport.GetFeature(state) && (state[0] & STATUS_BUSY) == 0)
        {
            ready = true;
            break;
        }

        Pause(poll_interval);
    }

    if(!ready)
    {
        return Status::Busy;
    }

    if(!transport.SetFeature(packet))
    {
        return Status::SendFailed;
    }

    Pause(delay);

    for(unsigned int attempt = 0; attempt < done_tries; attempt++)
    {
        if(transport.GetFeature(reply) && (reply[0] & STATUS_DONE) != 0)
        {
            return Status::Ok;
        }

        Pause(poll_interval);
    }

    return Status::NoReply;
}

void Mouse::Pause(std::chrono::milliseconds duration)
{
    if(duration.count() > 0)
    {
        std::this_thread::sleep_for(duration);
    }
}

}
