/*---------------------------------------------------------*\
| M6980XPlugin.h                                            |
|                                                           |
|   OpenRGB 1.0 plugin for the Gigabyte GHOST M6980X mouse  |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
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

    void                    RequestApply();
    void                    WorkerThread();
    void                    Connect();
    void                    Disconnect();
    void                    Cycle();
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
    | Set once OpenRGB applied a colour; until then the     |
    | plugin never writes, so starting OpenRGB alone does   |
    | not change the mouse                                  |
    \*-----------------------------------------------------*/
    std::atomic<bool>                   owned{false};

    /*-----------------------------------------------------*\
    | Worker-only state                                     |
    \*-----------------------------------------------------*/
    std::unique_ptr<m6980x::Transport>  transport;
    std::unique_ptr<m6980x::Mouse>      mouse;
    RGBControllerInterface*             controller          = nullptr;
    int                                 last_profile        = -1;
    RGBColor                            wanted_color        = 0;
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
