/*---------------------------------------------------------*\
| M6980XPlugin.cpp                                          |
|                                                           |
|   OpenRGB 1.0 plugin for the Gigabyte GHOST M6980X mouse  |
|                                                           |
|   The mouse has three RGB LEDs that always share one      |
|   colour (their count shows the DPI stage) and stores one |
|   colour per hardware profile, every channel with three   |
|   levels only (off, half, full). The plugin offers a      |
|   single "Static" mode and writes the colour into the     |
|   active profile; when the profile button switches to     |
|   another profile, the colour is written there as well.   |
|   There is no direct mode: the mouse keeps LED colours    |
|   without a save command, so every write presumably goes  |
|   to its flash, and the colour is only written when it    |
|   differs from what the mouse already has.                |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "M6980XPlugin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <QCoreApplication>
#include <QEventLoop>
#include <QVBoxLayout>

#include "LogManager.h"

#define M6980X_PLUGIN_NAME      "M6980X"
#define M6980X_PLUGIN_VERSION   "1.0.0"
#define M6980X_PLUGIN_URL       "https://github.com/Tom-8192/OpenRGB-M6980X-Plugin"
#define M6980X_DEVICE_NAME      "Gigabyte GHOST M6980X"

#define M6980X_LOG(level, ...)  do { if(api) api->LogEntry(__FILE__, __LINE__, level, "[" M6980X_PLUGIN_NAME "] " __VA_ARGS__); } while(0)

/*---------------------------------------------------------*\
| Worker timing: how often the active profile is checked    |
| while connected, and how often the plugin looks for the   |
| mouse while it is missing                                 |
\*---------------------------------------------------------*/
static const std::chrono::milliseconds  POLL_CONNECTED(500);
static const std::chrono::milliseconds  POLL_DISCONNECTED(2000);
/*---------------------------------------------------------*\
| Consecutive failures (with the mouse still present)       |
| before the device is closed and opened again              |
\*---------------------------------------------------------*/
static const unsigned int               MAX_FAILURES = 10;

M6980XPlugin::~M6980XPlugin()
{
    if(worker.joinable())
    {
        Unload();
    }
}

OpenRGBPluginInfo M6980XPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name               = M6980X_PLUGIN_NAME;
    info.Description        = "Static LED colour for the Gigabyte GHOST M6980X mouse (USB E0FF:0001)";
    info.Version            = M6980X_PLUGIN_VERSION;
    info.Commit             = "";
    info.URL                = M6980X_PLUGIN_URL;
    info.Location           = OPENRGB_PLUGIN_LOCATION_SETTINGS;
    info.Label              = M6980X_PLUGIN_NAME;
    info.ProtocolVersion    = OPENRGB_PLUGIN_API_VERSION;

    return info;
}

unsigned int M6980XPlugin::GetPluginAPIVersion()
{
    return OPENRGB_PLUGIN_API_VERSION;
}

void M6980XPlugin::Load(OpenRGBPluginAPIInterface* plugin_api_ptr)
{
    api             = plugin_api_ptr;
    stopping        = false;
    wake_pending    = false;
    worker_done     = false;

    SetStatus("Looking for the mouse...");

    worker = std::thread(&M6980XPlugin::WorkerThread, this);
}

QWidget* M6980XPlugin::GetWidget()
{
    if(widget == nullptr)
    {
        widget                  = new QWidget();
        QVBoxLayout* layout     = new QVBoxLayout(widget);
        status_label            = new QLabel(widget);
        QLabel* note            = new QLabel(widget);

        status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        note->setWordWrap(true);
        note->setText("The mouse has one colour per hardware profile and 3 levels per channel (27 colours); "
                      "OpenRGB colours are rounded to the nearest level. Only the active profile is written; "
                      "after a switch with the profile button the colour follows within half a second. "
                      "Effects are not supported on purpose: the mouse stores every colour change permanently.");

        layout->addWidget(status_label);
        layout->addWidget(note);
        layout->addStretch();

        status_timer = new QTimer();
        QObject::connect(status_timer, &QTimer::timeout, status_label, [this]()
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status_label->setText(QString::fromStdString(status_text));
        });
        status_timer->start(500);

        std::lock_guard<std::mutex> lock(status_mutex);
        status_label->setText(QString::fromStdString(status_text));
    }

    return widget;
}

QMenu* M6980XPlugin::GetTrayMenu()
{
    return nullptr;
}

void M6980XPlugin::Unload()
{
    /*-----------------------------------------------------*\
    | The page outlives the plugin (OpenRGB only removes    |
    | the tab), so stop the timer that runs our code        |
    \*-----------------------------------------------------*/
    if(status_timer != nullptr)
    {
        status_timer->stop();
        delete status_timer;
        status_timer = nullptr;
    }

    if(!worker.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        stopping = true;
    }
    wake.notify_one();

    /*-----------------------------------------------------*\
    | The worker unregisters the controller on its way out. |
    | OpenRGB passes device list changes to other plugins,  |
    | some of which block on the GUI thread, so keep events |
    | flowing while waiting instead of a bare join()        |
    \*-----------------------------------------------------*/
    while(!worker_done)
    {
        if(QCoreApplication::instance() != nullptr)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    worker.join();
}

void M6980XPlugin::OnProfileAboutToLoad()
{
}

void M6980XPlugin::OnProfileLoad(nlohmann::json /*profile_data*/)
{
}

nlohmann::json M6980XPlugin::OnProfileSave()
{
    return nlohmann::json();
}

unsigned char* M6980XPlugin::OnSDKCommand(unsigned int /*pkt_id*/, unsigned char* /*pkt_data*/, unsigned int* pkt_size)
{
    if(pkt_size != nullptr)
    {
        *pkt_size = 0;
    }

    return nullptr;
}

void M6980XPlugin::ProfileManagerUpdated(unsigned int /*update_reason*/)
{
}

void M6980XPlugin::ResourceManagerUpdated(unsigned int /*update_reason*/)
{
}

void M6980XPlugin::SettingsManagerUpdated(unsigned int /*update_reason*/)
{
}

/*---------------------------------------------------------*\
| Controller callbacks                                      |
\*---------------------------------------------------------*/
void M6980XPlugin::CallbackUpdateMode(void* object_ptr)
{
    static_cast<M6980XPlugin*>(object_ptr)->RequestApply();
}

void M6980XPlugin::CallbackUpdateZoneMode(void* object_ptr, int /*zone*/)
{
    static_cast<M6980XPlugin*>(object_ptr)->RequestApply();
}

void M6980XPlugin::RequestApply()
{
    owned = true;

    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        wake_pending = true;
    }
    wake.notify_one();
}

/*---------------------------------------------------------*\
| Worker                                                    |
\*---------------------------------------------------------*/
void M6980XPlugin::WorkerThread()
{
    while(true)
    {
        if(controller == nullptr)
        {
            Connect();
        }

        if(controller != nullptr)
        {
            Cycle();
        }

        std::unique_lock<std::mutex> lock(wake_mutex);
        wake.wait_for(lock, controller != nullptr ? POLL_CONNECTED : POLL_DISCONNECTED, [this]() { return stopping || wake_pending; });

        if(stopping)
        {
            break;
        }

        wake_pending = false;
    }

    Disconnect();
    worker_done = true;
}

void M6980XPlugin::Connect()
{
    std::vector<std::string> paths = m6980x::FindDevicePaths();

    if(paths.empty())
    {
        SetStatus("Mouse not found (USB E0FF:0001). Waiting for it to be plugged in.");
        LogOnce(LL_INFO, "mouse not found, waiting for it");
        return;
    }

    for(const std::string& path : paths)
    {
        std::string                         error;
        std::unique_ptr<m6980x::Transport>  candidate = m6980x::OpenTransport(path, error);

        if(!candidate)
        {
            SetStatus("Cannot open the mouse: " + error);
            LogOnce(LL_WARNING, error);
            continue;
        }

        std::unique_ptr<m6980x::Mouse>  candidate_mouse(new m6980x::Mouse(*candidate));
        m6980x::Identity                identity;
        m6980x::Status                  status = candidate_mouse->Identify(identity);

        if(status != m6980x::Status::Ok)
        {
            SetStatus(std::string("The mouse does not answer: ") + m6980x::StatusText(status));
            LogOnce(LL_WARNING, path + ": identify failed: " + m6980x::StatusText(status));
            continue;
        }

        if(!identity.is_6980x)
        {
            /*---------------------------------------------*\
            | Other GHOST models use the same USB ID but a  |
            | different memory layout                       |
            \*---------------------------------------------*/
            SetStatus("Unsupported device (identify " + identity.raw + "), only the M6980X is supported.");
            LogOnce(LL_WARNING, path + ": not an M6980X, identify reply " + identity.raw);
            continue;
        }

        unsigned char   profile = 0;
        m6980x::Levels  levels;

        status = candidate_mouse->ActiveProfile(profile);

        if(status == m6980x::Status::Ok)
        {
            status = candidate_mouse->LedColor(profile, levels);
        }

        if(status != m6980x::Status::Ok)
        {
            SetStatus(std::string("Reading the mouse failed: ") + m6980x::StatusText(status));
            LogOnce(LL_WARNING, path + ": reading profile/colour failed: " + m6980x::StatusText(status));
            continue;
        }

        /*-------------------------------------------------*\
        | Start with the colour the mouse shows, unless     |
        | OpenRGB already set one before a reconnect        |
        \*-------------------------------------------------*/
        RGBColor start_color = owned ? wanted_color
                                     : ToRGBColor(m6980x::ByteFromLevel(levels[0]),
                                                  m6980x::ByteFromLevel(levels[1]),
                                                  m6980x::ByteFromLevel(levels[2]));

        mode static_mode;
        static_mode.name        = "Static";
        static_mode.value       = 0;
        static_mode.flags       = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_AUTOMATIC_SAVE;
        static_mode.colors_min  = 1;
        static_mode.colors_max  = 1;
        static_mode.color_mode  = MODE_COLORS_MODE_SPECIFIC;
        static_mode.colors.push_back(start_color);

        zone mouse_zone;
        mouse_zone.name         = "Mouse";
        mouse_zone.type         = ZONE_TYPE_SINGLE;
        mouse_zone.leds_count   = 1;
        mouse_zone.leds_min     = 1;
        mouse_zone.leds_max     = 1;

        led mouse_led;
        mouse_led.name          = "LEDs";

        RGBController_Setup setup = {};
        setup.name              = M6980X_DEVICE_NAME;
        setup.vendor            = "Gigabyte";
        setup.description       = "Gigabyte GHOST M6980X mouse (OEM Areson), via the " M6980X_PLUGIN_NAME " plugin";
        setup.version           = M6980X_PLUGIN_VERSION;
        setup.location          = "HID: " + path;
        setup.type              = DEVICE_TYPE_MOUSE;
        setup.flags             = CONTROLLER_FLAG_VIRTUAL;
        setup.active_mode       = 0;
        setup.modes.push_back(static_mode);
        setup.zones.push_back(mouse_zone);
        setup.leds.push_back(mouse_led);
        setup.object_ptr        = this;
        setup.DeviceUpdateMode  = &M6980XPlugin::CallbackUpdateMode;
        setup.DeviceSaveMode    = &M6980XPlugin::CallbackUpdateMode;
        setup.DeviceUpdateZoneMode = &M6980XPlugin::CallbackUpdateZoneMode;

        transport       = std::move(candidate);
        mouse           = std::move(candidate_mouse);
        last_profile    = profile;
        failures        = 0;
        controller      = api->CreateVirtualRGBController(&setup);

        api->RegisterVirtualRGBController(controller);

        char text[160];
        snprintf(text, sizeof(text), "Connected (%s), profile %d, colour levels %u %u %u",
                 path.c_str(), profile + 1, levels[0], levels[1], levels[2]);
        SetStatus(text);
        last_logged.clear();
        M6980X_LOG(LL_INFO, "%s", text);
        return;
    }
}

void M6980XPlugin::Disconnect()
{
    if(controller != nullptr)
    {
        api->UnregisterVirtualRGBController(controller);
        api->DeleteVirtualRGBController(controller);
        controller = nullptr;
    }

    mouse.reset();
    transport.reset();
    last_profile    = -1;
    failures        = 0;
}

void M6980XPlugin::Cycle()
{
    unsigned char   profile = 0;
    m6980x::Status  status  = mouse->ActiveProfile(profile);

    if(status != m6980x::Status::Ok)
    {
        HandleError(status, "reading the active profile");
        return;
    }

    if((int)profile != last_profile)
    {
        M6980X_LOG(LL_VERBOSE, "active profile is now %d", profile + 1);
        last_profile = profile;
    }

    if(!owned)
    {
        failures = 0;
        return;
    }

    int active_mode = controller->GetActiveMode();

    if(active_mode < 0 || controller->GetModeColorsCount(active_mode) == 0)
    {
        return;
    }

    wanted_color = controller->GetModeColor(active_mode, 0);

    m6980x::Levels levels =
    {
        m6980x::LevelFromByte(RGBGetRValue(wanted_color)),
        m6980x::LevelFromByte(RGBGetGValue(wanted_color)),
        m6980x::LevelFromByte(RGBGetBValue(wanted_color)),
    };

    bool written = false;
    status = mouse->SetLedColor(profile, levels, written);

    if(status != m6980x::Status::Ok)
    {
        HandleError(status, "setting the colour");
        return;
    }

    failures = 0;

    if(written)
    {
        char text[128];
        snprintf(text, sizeof(text), "Connected (%s), profile %d set to colour levels %u %u %u",
                 transport->Path().c_str(), profile + 1, levels[0], levels[1], levels[2]);
        SetStatus(text);
        M6980X_LOG(LL_INFO, "profile %d set to colour levels %u %u %u", profile + 1, levels[0], levels[1], levels[2]);
    }
}

void M6980XPlugin::HandleError(m6980x::Status status, const char* action)
{
    /*-----------------------------------------------------*\
    | Another program (e.g. m6980x_configtool) is talking   |
    | to the mouse; simply try again next cycle             |
    \*-----------------------------------------------------*/
    if(status == m6980x::Status::Locked)
    {
        M6980X_LOG(LL_VERBOSE, "%s skipped: %s", action, m6980x::StatusText(status));
        return;
    }

    std::vector<std::string> paths = m6980x::FindDevicePaths();

    if(std::find(paths.begin(), paths.end(), transport->Path()) == paths.end())
    {
        M6980X_LOG(LL_INFO, "mouse disconnected");
        SetStatus("Mouse disconnected. Waiting for it to be plugged in.");
        Disconnect();
        return;
    }

    failures++;

    if(failures == 1)
    {
        M6980X_LOG(LL_WARNING, "%s failed: %s", action, m6980x::StatusText(status));
    }

    if(failures >= MAX_FAILURES)
    {
        M6980X_LOG(LL_WARNING, "%u failures in a row, reopening the mouse", failures);
        SetStatus(std::string("The mouse stopped answering (") + m6980x::StatusText(status) + "), reconnecting...");
        Disconnect();
    }
}

void M6980XPlugin::SetStatus(const std::string& text)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    status_text = text;
}

void M6980XPlugin::LogOnce(unsigned int level, const std::string& text)
{
    if(text != last_logged)
    {
        last_logged = text;
        M6980X_LOG(level, "%s", text.c_str());
    }
}
