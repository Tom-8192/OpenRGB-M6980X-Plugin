/*---------------------------------------------------------*\
| HidTransport_Linux.cpp                                    |
|                                                           |
|   Linux: /dev/hidraw* ioctls directly (the kernel driver  |
|   stays attached, the mouse keeps working), flock on the  |
|   device as the lock between processes                    |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#ifdef __linux__

#include "HidTransport.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace m6980x
{

namespace
{

class HidrawTransport : public Transport
{
public:
    HidrawTransport(int fd, const std::string& path)
        : fd(fd), path(path), locked(false)
    {
    }

    ~HidrawTransport() override
    {
        Unlock();
        close(fd);
    }

    bool SetFeature(const Report& data) override
    {
        unsigned char buf[REPORT_LEN + 1] = {0};
        memcpy(buf + 1, data.data(), REPORT_LEN);
        return ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf) >= 0;
    }

    bool GetFeature(Report& data) override
    {
        unsigned char buf[REPORT_LEN + 1] = {0};
        int n = ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf);

        /*-------------------------------------------------*\
        | Like strip_report_id in m6980x_configtool:        |
        | 9 bytes = report ID + data, 8 bytes = data only   |
        \*-------------------------------------------------*/
        if(n == (int)REPORT_LEN + 1)
        {
            memcpy(data.data(), buf + 1, REPORT_LEN);
            return true;
        }

        if(n == (int)REPORT_LEN)
        {
            memcpy(data.data(), buf, REPORT_LEN);
            return true;
        }

        return false;
    }

    bool Lock() override
    {
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::milliseconds(LOCK_TIMEOUT_MS);

        while(true)
        {
            if(flock(fd, LOCK_EX | LOCK_NB) == 0)
            {
                locked = true;
                return true;
            }

            if(errno != EWOULDBLOCK || std::chrono::steady_clock::now() >= end)
            {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void Unlock() override
    {
        if(locked)
        {
            flock(fd, LOCK_UN);
            locked = false;
        }
    }

    std::string Path() const override
    {
        return path;
    }

private:
    int             fd;
    std::string     path;
    bool            locked;
};

std::string ReadFile(const std::string& path)
{
    std::ifstream       file(path);
    std::stringstream   content;

    content << file.rdbuf();
    return content.str();
}

}

std::vector<std::string> FindDevicePaths()
{
    std::vector<std::string>    paths;
    std::vector<std::string>    nodes;
    char                        hid_id[64];

    snprintf(hid_id, sizeof(hid_id), "HID_ID=0003:%08X:%08X", VID, PID);

    DIR* dir = opendir("/sys/class/hidraw");

    if(dir == nullptr)
    {
        return paths;
    }

    while(struct dirent* entry = readdir(dir))
    {
        if(strncmp(entry->d_name, "hidraw", 6) == 0)
        {
            nodes.push_back(entry->d_name);
        }
    }

    closedir(dir);
    std::sort(nodes.begin(), nodes.end());

    for(const std::string& node : nodes)
    {
        std::string device = "/sys/class/hidraw/" + node + "/device";
        std::string uevent = ReadFile(device + "/uevent");

        std::transform(uevent.begin(), uevent.end(), uevent.begin(), [](unsigned char c) { return (char)std::toupper(c); });

        if(uevent.find(hid_id) == std::string::npos)
        {
            continue;
        }

        /*-------------------------------------------------*\
        | .../3-2:1.0/0003:E0FF:0001.0007 - the USB         |
        | interface directory ends with ".<ifnum>"          |
        \*-------------------------------------------------*/
        char real[PATH_MAX];

        if(realpath(device.c_str(), real) == nullptr)
        {
            continue;
        }

        std::string parent  = real;
        parent              = parent.substr(0, parent.rfind('/'));
        std::string suffix  = "." + std::to_string(INTERFACE);

        if(parent.size() >= suffix.size() && parent.compare(parent.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            paths.push_back("/dev/" + node);
        }
    }

    return paths;
}

std::unique_ptr<Transport> OpenTransport(const std::string& path, std::string& error)
{
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);

    if(fd < 0)
    {
        error = path + ": " + strerror(errno);

        if(errno == EACCES)
        {
            error += " (no access to the hidraw device, see the udev rule in the README)";
        }

        return nullptr;
    }

    return std::unique_ptr<Transport>(new HidrawTransport(fd, path));
}

}

#endif
