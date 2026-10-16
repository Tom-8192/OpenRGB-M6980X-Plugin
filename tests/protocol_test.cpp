/*---------------------------------------------------------*\
| protocol_test.cpp                                         |
|                                                           |
|   Tests of the command layer against a simulated mouse    |
|   (no Qt, no OpenRGB, no device needed)                   |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cstdio>
#include <vector>

#include "M6980XProtocol.h"

using namespace m6980x;

static int failed = 0;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        if(!(cond))                                                             \
        {                                                                       \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            failed++;                                                           \
        }                                                                       \
    } while(0)

/*---------------------------------------------------------*\
| Simulated M6980X: answers like the real mouse, with stale |
| reply bytes, optional short reports and a stuck busy bit  |
\*---------------------------------------------------------*/
class FakeMouse : public Transport
{
public:
    unsigned char               model_byte      = 1;    /* identify data[6]          */
    unsigned char               profile         = 0;
    Levels                      colors[PROFILES]= { {2, 0, 0}, {0, 2, 0}, {0, 0, 2} };
    unsigned int                short_reports   = 0;    /* GETs that fail first     */
    bool                        always_busy     = false;
    bool                        ignore_writes   = false;
    bool                        lock_fails      = false;
    std::vector<Report>         sent;

    bool SetFeature(const Report& data) override
    {
        sent.push_back(data);

        unsigned char group = data[0];
        unsigned char cmd   = data[1];

        /*-------------------------------------------------*\
        | Only the bytes a command fills change; the rest   |
        | stays stale like on the device                    |
        \*-------------------------------------------------*/
        reply[0] = STATUS_DONE | group;

        if(group == GROUP_SYSTEM && cmd == 0x00)
        {
            const unsigned char ident[7] = { 0x0C, 0x12, 0x09, 0x00, 0x00, 0x00, model_byte };

            for(int idx = 0; idx < 7; idx++)
            {
                reply[1 + idx] = ident[idx];
            }
        }
        else if(group == GROUP_REGISTER && cmd == REG_PROFILE)
        {
            reply[1] = profile;
        }
        else if(group == GROUP_REGISTER && cmd == REG_LED)
        {
            const Levels& c = colors[data[2]];
            reply[1] = c[0];
            reply[2] = c[1];
            reply[3] = c[2];
        }
        else if(group == GROUP_REGISTER && cmd == (REG_LED | WRITE_BIT))
        {
            if(!ignore_writes)
            {
                colors[data[2]] = { data[3], data[4], data[5] };
            }
        }

        done = true;
        return true;
    }

    bool GetFeature(Report& data) override
    {
        if(short_reports > 0)
        {
            short_reports--;
            return false;
        }

        if(always_busy)
        {
            data    = reply;
            data[0] = STATUS_BUSY;
            return true;
        }

        data = reply;

        if(!done)
        {
            data[0] = 0x00;
        }

        return true;
    }

    bool Lock() override
    {
        return !lock_fails;
    }

    unsigned int CountSent(unsigned char group, unsigned char cmd) const
    {
        unsigned int count = 0;

        for(const Report& r : sent)
        {
            if(r[0] == group && r[1] == cmd)
            {
                count++;
            }
        }

        return count;
    }

private:
    Report  reply   = { 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };
    bool    done    = false;
};

static Mouse MakeMouse(Transport& transport)
{
    Mouse mouse(transport);
    mouse.poll_interval = std::chrono::milliseconds(0);
    mouse.write_delay   = std::chrono::milliseconds(0);
    return mouse;
}

static void TestLevels()
{
    CHECK(LevelFromByte(0)   == 0);
    CHECK(LevelFromByte(63)  == 0);
    CHECK(LevelFromByte(64)  == 1);
    CHECK(LevelFromByte(128) == 1);
    CHECK(LevelFromByte(191) == 1);
    CHECK(LevelFromByte(192) == 2);
    CHECK(LevelFromByte(255) == 2);

    for(unsigned char level = 0; level < LED_LEVELS; level++)
    {
        CHECK(LevelFromByte(ByteFromLevel(level)) == level);
    }
}

static void TestIdentify()
{
    FakeMouse       fake;
    Mouse           mouse = MakeMouse(fake);
    Identity        identity;

    CHECK(mouse.Identify(identity) == Status::Ok);
    CHECK(identity.signature_ok);
    CHECK(identity.is_6980x);
    CHECK(identity.raw == "0C 12 09 00 00 00 01");
    CHECK(fake.sent.size() == 1);
    CHECK((fake.sent[0] == Report{ 0x00, 0x00, 0, 0, 0, 0, 0, 0 }));

    /*-----------------------------------------------------*\
    | M8600 (data[6] = 4) has another memory layout         |
    \*-----------------------------------------------------*/
    fake.model_byte = 4;
    CHECK(mouse.Identify(identity) == Status::Ok);
    CHECK(identity.signature_ok);
    CHECK(!identity.is_6980x);
}

static void TestHandshake()
{
    FakeMouse       fake;
    Mouse           mouse = MakeMouse(fake);
    unsigned char   profile = 9;

    /*-----------------------------------------------------*\
    | Short reports while a button is pressed are retried   |
    \*-----------------------------------------------------*/
    fake.profile        = 2;
    fake.short_reports  = 5;
    CHECK(mouse.ActiveProfile(profile) == Status::Ok);
    CHECK(profile == 2);

    fake.always_busy = true;
    CHECK(mouse.ActiveProfile(profile) == Status::Busy);
    fake.always_busy = false;

    fake.lock_fails = true;
    std::size_t sent = fake.sent.size();
    CHECK(mouse.ActiveProfile(profile) == Status::Locked);
    CHECK(fake.sent.size() == sent);
    fake.lock_fails = false;

    fake.profile = 3;
    CHECK(mouse.ActiveProfile(profile) == Status::BadReply);
}

static void TestSetLedColor()
{
    FakeMouse       fake;
    Mouse           mouse = MakeMouse(fake);
    bool            written = true;

    /*-----------------------------------------------------*\
    | Same colour: read only, nothing written               |
    \*-----------------------------------------------------*/
    CHECK(mouse.SetLedColor(1, Levels{ 0, 2, 0 }, written) == Status::Ok);
    CHECK(!written);
    CHECK(fake.CountSent(GROUP_REGISTER, REG_LED | WRITE_BIT) == 0);

    /*-----------------------------------------------------*\
    | Different colour: 01 86 p R G B, read back            |
    \*-----------------------------------------------------*/
    fake.sent.clear();
    CHECK(mouse.SetLedColor(1, Levels{ 1, 0, 2 }, written) == Status::Ok);
    CHECK(written);
    CHECK(fake.sent.size() == 3);
    CHECK((fake.sent[0] == Report{ 0x01, 0x06, 1, 0, 0, 0, 0, 0 }));
    CHECK((fake.sent[1] == Report{ 0x01, 0x86, 1, 1, 0, 2, 0, 0 }));
    CHECK((fake.sent[2] == Report{ 0x01, 0x06, 1, 0, 0, 0, 0, 0 }));
    CHECK((fake.colors[1] == Levels{ 1, 0, 2 }));
    CHECK((fake.colors[0] == Levels{ 2, 0, 0 }));
    CHECK((fake.colors[2] == Levels{ 0, 0, 2 }));

    /*-----------------------------------------------------*\
    | 0-255 values from other software are read as they     |
    | are and replaced by levels                            |
    \*-----------------------------------------------------*/
    fake.colors[2] = { 255, 255, 0 };
    Levels raw;
    CHECK(mouse.LedColor(2, raw) == Status::Ok);
    CHECK((raw == Levels{ 255, 255, 0 }));
    CHECK(ByteFromLevel(raw[0]) == 255);
    CHECK(mouse.SetLedColor(2, Levels{ 2, 2, 0 }, written) == Status::Ok);
    CHECK(written);
    CHECK((fake.colors[2] == Levels{ 2, 2, 0 }));

    /*-----------------------------------------------------*\
    | The mouse did not take it                             |
    \*-----------------------------------------------------*/
    fake.ignore_writes = true;
    CHECK(mouse.SetLedColor(0, Levels{ 0, 0, 0 }, written) == Status::NotApplied);
    CHECK(written);
    fake.ignore_writes = false;

    /*-----------------------------------------------------*\
    | Out of range: no I/O at all                           |
    \*-----------------------------------------------------*/
    fake.sent.clear();
    CHECK(mouse.SetLedColor(0, Levels{ 3, 0, 0 }, written) == Status::Invalid);
    CHECK(mouse.SetLedColor(3, Levels{ 0, 0, 0 }, written) == Status::Invalid);
    CHECK(fake.sent.empty());
}

int main()
{
    TestLevels();
    TestIdentify();
    TestHandshake();
    TestSetLedColor();

    if(failed == 0)
    {
        printf("all tests passed\n");
        return 0;
    }

    printf("%d check(s) failed\n", failed);
    return 1;
}
