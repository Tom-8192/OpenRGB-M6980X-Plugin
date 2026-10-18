/*---------------------------------------------------------*\
| M6980XPlugin.h                                            |
|                                                           |
|   OpenRGB 1.1 plugin for the Gigabyte GHOST M6980X mouse  |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <QLabel>
#include <QObject>
#include <QTimer>

#include "OpenRGBPluginInterface.h"
#include "HidTransport.h"
#include "M6980XProtocol.h"

class M6980XPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID FILE "../M6980XPlugin.json")
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    ~M6980XPlugin() override;

    OpenRGBPluginInfo       GetPluginInfo() override;
    unsigned int            GetPluginAPIVersion() override;

    void                    Load(OpenRGBPluginAPIInterface* plugin_api_ptr) override;
    QWidget*                GetWidget() override;
    QMenu*                  GetTrayMenu() override;
    void                    Unload() override;
    void                    OnProfileAboutToLoad() override;
    void                    OnProfileLoad(nlohmann::json profile_data) override;
    nlohmann::json          OnProfileSave() override;
    unsigned char*          OnSDKCommand(unsigned int pkt_id, unsigned char* pkt_data, unsigned int* pkt_size) override;

    void                    ProfileManagerUpdated(unsigned int update_reason) override;
    void                    ResourceManagerUpdated(unsigned int update_reason) override;
    void                    SettingsManagerUpdated(unsigned int update_reason) override;

private:
    /*-----------------------------------------------------*\
    | RGBController_Setup callbacks (called by OpenRGB with |
    | object_ptr = this); they only wake the worker         |
    \*-----------------------------------------------------*/
    static void             CallbackUpdateMode(void* object_ptr);
    static void             CallbackUpdateZoneMode(void* object_ptr, int zone);
    static void             CallbackUpdateLEDs(void* object_ptr);
    static void             CallbackUpdateZoneLEDs(void* object_ptr, int zone);
    static void             CallbackUpdateSingleLED(void* object_ptr, int led);

    void                    Wake();
    void                    WorkerThread();
    void                    Connect();
    void                    Disconnect();
    void                    Cycle();
    bool                    ApplyProfileRequest(unsigned char profile);
    void                    ApplyStatic(unsigned char profile);
    bool                    ShowStaticColor(unsigned char profile);
    void                    CyclePerProfile();
    bool                    ShowDeviceColors();
    int                     LoadModeSetting();
    void                    SaveModeSetting(int mode);
    void                    HandleError(m6980x::Status status, const char* action);
    void                    SetStatus(const std::string& text);
    void                    LogOnce(unsigned int level, const std::string& text);

    OpenRGBPluginAPIInterface*          api                 = nullptr;

    /*-----------------------------------------------------*\
    | Worker thread; all device I/O and (un)registering of  |
    | the controller happen there                           |
    \*-----------------------------------------------------*/
    std::thread                         worker;
    std::mutex                          wake_mutex;
    std::condition_variable             wake;
    bool                                stopping            = false;
    bool                                wake_pending        = false;
    std::atomic<bool>                   worker_done{false};

    /*-----------------------------------------------------*\
    | Set by the controller callbacks: OpenRGB applied a    |
    | mode/mode colour, or changed the per-profile colours  |
    \*-----------------------------------------------------*/
    std::atomic<bool>                   mode_dirty{false};
    std::atomic<bool>                   leds_dirty{false};

    /*-----------------------------------------------------*\
    | OpenRGB profiles: in client mode the server restores  |
    | only its own devices, so the plugin stores mode and   |
    | colours in the profile's plugin section itself. A     |
    | loaded profile waits here until the worker applies it |
    | (also when the mouse connects only after the load)    |
    \*-----------------------------------------------------*/
    std::mutex                          request_mutex;
    nlohmann::json                      profile_request;
    bool                                has_profile_request = false;
    /*-----------------------------------------------------*\
    | Guards the controller pointer against OnProfileSave   |
    | on the GUI thread; only the worker changes it         |
    \*-----------------------------------------------------*/
    std::mutex                          controller_mutex;

    /*-----------------------------------------------------*\
    | Worker-only state                                     |
    \*-----------------------------------------------------*/
    std::unique_ptr<m6980x::Transport>  transport;
    std::unique_ptr<m6980x::Mouse>      mouse;
    RGBControllerInterface*             controller          = nullptr;
    int                                 last_profile        = -1;
    int                                 last_mode           = -1;
    /*-----------------------------------------------------*\
    | Per-profile mode: colours waiting to become stable    |
    \*-----------------------------------------------------*/
    m6980x::Levels                      pending[m6980x::PROFILES];
    std::chrono::steady_clock::time_point pending_since;
    unsigned int                        failures            = 0;
    std::string                         last_logged;

    /*-----------------------------------------------------*\
    | Status page (GUI thread reads, worker writes)         |
    \*-----------------------------------------------------*/
    std::mutex                          status_mutex;
    std::string                         status_text;
    QWidget*                            widget              = nullptr;
    QLabel*                             status_label        = nullptr;
    QTimer*                             status_timer        = nullptr;
};
