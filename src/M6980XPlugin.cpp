/*---------------------------------------------------------*\
| M6980XPlugin.cpp                                          |
|                                                           |
|   OpenRGB 1.1 plugin for the Gigabyte GHOST M6980X mouse  |
|                                                           |
|   The mouse has three RGB LEDs that always share one      |
|   colour (their count shows the DPI stage) and stores one |
|   colour per hardware profile, every channel with three   |
|   levels only (off, half, full). Two modes:               |
|     "Per profile": three LEDs "Profile 1-3", each colour  |
|       goes into its own profile; switching profiles       |
|       writes nothing                                      |
|     "Static": one colour, written into the active profile |
|       once when it is applied (also by quick colours);    |
|       after a profile switch it shows that profile's      |
|       colour and writes nothing                           |
|   There is no direct mode: the mouse keeps LED colours    |
|   without a save command, so every write presumably goes  |
|   to its flash. Colours are only written when they differ |
|   from what the mouse already has, per-profile colours    |
|   only once they stayed unchanged for a second.           |
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
#define M6980X_PLUGIN_VERSION   "1.2.0"
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
/*---------------------------------------------------------*\
| Per-profile colours must stay unchanged this long before  |
| they are written (colour picker dragging, effects)        |
\*---------------------------------------------------------*/
static const std::chrono::milliseconds  SETTLE_TIME(1000);

/*---------------------------------------------------------*\
| Mode indices of the controller                            |
\*---------------------------------------------------------*/
enum
{
    MODE_PER_PROFILE    = 0,
    MODE_STATIC         = 1,
};

static const char* const SETTINGS_KEY = "M6980XPlugin";

static m6980x::Levels LevelsFromColor(RGBColor color)
{
    m6980x::Levels levels =
    {
        m6980x::LevelFromByte(RGBGetRValue(color)),
        m6980x::LevelFromByte(RGBGetGValue(color)),
        m6980x::LevelFromByte(RGBGetBValue(color)),
    };

    return levels;
}

static RGBColor ColorFromLevels(const m6980x::Levels& levels)
{
    return ToRGBColor(m6980x::ByteFromLevel(levels[0]), m6980x::ByteFromLevel(levels[1]), m6980x::ByteFromLevel(levels[2]));
}

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
                      "OpenRGB colours are rounded to the nearest level. Choose the behaviour with the mode in the "
                      "device view:\n"
                      "  Per profile: the LEDs \"Profile 1-3\" set the colour of each profile; switching profiles "
                      "writes nothing.\n"
                      "  Static: one colour (also from quick colours), written once into the active profile; after "
                      "a switch with the profile button it shows that profile's colour and writes nothing.\n"
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

void M6980XPlugin::OnProfileLoad(nlohmann::json profile_data)
{
    if(!profile_data.is_object() || !profile_data.contains("mode"))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(request_mutex);
        profile_request     = profile_data;
        has_profile_request = true;
    }

    Wake();
}

nlohmann::json M6980XPlugin::OnProfileSave()
{
    nlohmann::json data;

    std::lock_guard<std::mutex> lock(controller_mutex);

    if(controller == nullptr)
    {
        return data;
    }

    data["mode"] = (controller->GetActiveMode() == MODE_STATIC) ? "static" : "per_profile";

    if(controller->GetModeColorsCount(MODE_STATIC) > 0)
    {
        data["static_color"] = controller->GetModeColor(MODE_STATIC, 0);
    }

    for(unsigned int p = 0; p < m6980x::PROFILES; p++)
    {
        data["profile_colors"][p] = controller->GetColor(p);
    }

    return data;
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
    M6980XPlugin* plugin = static_cast<M6980XPlugin*>(object_ptr);
    plugin->mode_dirty = true;
    plugin->Wake();
}

void M6980XPlugin::CallbackUpdateZoneMode(void* object_ptr, int /*zone*/)
{
    CallbackUpdateMode(object_ptr);
}

void M6980XPlugin::CallbackUpdateLEDs(void* object_ptr)
{
    M6980XPlugin* plugin = static_cast<M6980XPlugin*>(object_ptr);
    plugin->leds_dirty = true;
    plugin->Wake();
}

void M6980XPlugin::CallbackUpdateZoneLEDs(void* object_ptr, int /*zone*/)
{
    CallbackUpdateLEDs(object_ptr);
}

void M6980XPlugin::CallbackUpdateSingleLED(void* object_ptr, int /*led*/)
{
    CallbackUpdateLEDs(object_ptr);
}

void M6980XPlugin::Wake()
{
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
        m6980x::Levels  levels[m6980x::PROFILES];

        status = candidate_mouse->ActiveProfile(profile);

        for(unsigned char p = 0; p < m6980x::PROFILES && status == m6980x::Status::Ok; p++)
        {
            status = candidate_mouse->LedColor(p, levels[p]);
        }

        if(status != m6980x::Status::Ok)
        {
            SetStatus(std::string("Reading the mouse failed: ") + m6980x::StatusText(status));
            LogOnce(LL_WARNING, path + ": reading profile/colours failed: " + m6980x::StatusText(status));
            continue;
        }

        /*-------------------------------------------------*\
        | "Per profile": one LED per hardware profile.      |
        | Not called "Direct"/"Custom", so SDK clients and  |
        | effects do not pick it by themselves              |
        \*-------------------------------------------------*/
        mode per_profile_mode;
        per_profile_mode.name       = "Per profile";
        per_profile_mode.value      = MODE_PER_PROFILE;
        per_profile_mode.flags      = MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_AUTOMATIC_SAVE;
        per_profile_mode.color_mode = MODE_COLORS_PER_LED;

        /*-------------------------------------------------*\
        | "Static": shows the colour of the active profile  |
        \*-------------------------------------------------*/
        mode static_mode;
        static_mode.name            = "Static";
        static_mode.value           = MODE_STATIC;
        static_mode.flags           = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_AUTOMATIC_SAVE;
        static_mode.colors_min      = 1;
        static_mode.colors_max      = 1;
        static_mode.color_mode      = MODE_COLORS_MODE_SPECIFIC;
        static_mode.colors.push_back(ColorFromLevels(levels[profile]));

        zone profiles_zone;
        profiles_zone.name          = "Profiles";
        profiles_zone.type          = ZONE_TYPE_LINEAR;
        profiles_zone.leds_count    = m6980x::PROFILES;
        profiles_zone.leds_min      = m6980x::PROFILES;
        profiles_zone.leds_max      = m6980x::PROFILES;

        RGBController_Setup setup = {};
        setup.name                  = M6980X_DEVICE_NAME;
        setup.vendor                = "Gigabyte";
        setup.description           = "Gigabyte GHOST M6980X mouse (OEM Areson), via the " M6980X_PLUGIN_NAME " plugin";
        setup.version               = M6980X_PLUGIN_VERSION;
        setup.location              = "HID: " + path;
        setup.type                  = DEVICE_TYPE_MOUSE;
        setup.flags                 = CONTROLLER_FLAG_VIRTUAL;
        setup.active_mode           = LoadModeSetting();
        setup.modes.push_back(per_profile_mode);
        setup.modes.push_back(static_mode);
        setup.zones.push_back(profiles_zone);

        for(unsigned int p = 0; p < m6980x::PROFILES; p++)
        {
            led profile_led;
            profile_led.name        = "Profile " + std::to_string(p + 1);
            profile_led.value       = p;
            setup.leds.push_back(profile_led);
        }

        setup.object_ptr            = this;
        setup.DeviceUpdateMode      = &M6980XPlugin::CallbackUpdateMode;
        setup.DeviceSaveMode        = &M6980XPlugin::CallbackUpdateMode;
        setup.DeviceUpdateZoneMode  = &M6980XPlugin::CallbackUpdateZoneMode;
        setup.DeviceUpdateLEDs      = &M6980XPlugin::CallbackUpdateLEDs;
        setup.DeviceUpdateZoneLEDs  = &M6980XPlugin::CallbackUpdateZoneLEDs;
        setup.DeviceUpdateSingleLED = &M6980XPlugin::CallbackUpdateSingleLED;

        transport       = std::move(candidate);
        mouse           = std::move(candidate_mouse);
        last_profile    = profile;
        last_mode       = setup.active_mode;
        failures        = 0;
        leds_dirty      = false;

        RGBControllerInterface* created = api->CreateVirtualRGBController(&setup);

        for(unsigned int p = 0; p < m6980x::PROFILES; p++)
        {
            created->SetColor(p, ColorFromLevels(levels[p]));
            pending[p] = levels[p];
        }

        {
            std::lock_guard<std::mutex> lock(controller_mutex);
            controller = created;
        }

        api->RegisterVirtualRGBController(controller);

        char text[256];
        snprintf(text, sizeof(text), "Connected (%s), profile %d active, colour levels %u %u %u / %u %u %u / %u %u %u",
                 path.c_str(), profile + 1,
                 levels[0][0], levels[0][1], levels[0][2],
                 levels[1][0], levels[1][1], levels[1][2],
                 levels[2][0], levels[2][1], levels[2][2]);
        SetStatus(text);
        last_logged.clear();
        M6980X_LOG(LL_INFO, "%s", text);
        return;
    }
}

void M6980XPlugin::Disconnect()
{
    RGBControllerInterface* removed = nullptr;

    {
        std::lock_guard<std::mutex> lock(controller_mutex);
        removed     = controller;
        controller  = nullptr;
    }

    if(removed != nullptr)
    {
        api->UnregisterVirtualRGBController(removed);
        api->DeleteVirtualRGBController(removed);
    }

    mouse.reset();
    transport.reset();
    last_profile    = -1;
    last_mode       = -1;
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

    failures = 0;

    bool profile_changed = (int)profile != last_profile;

    if(profile_changed)
    {
        M6980X_LOG(LL_VERBOSE, "active profile is now %d", profile + 1);
        last_profile = profile;
    }

    if(ApplyProfileRequest(profile))
    {
        return;
    }

    int     active_mode = controller->GetActiveMode();
    bool    apply       = mode_dirty.exchange(false);

    if(apply && active_mode != last_mode)
    {
        M6980X_LOG(LL_INFO, "mode is now %s", active_mode == MODE_STATIC ? "Static" : "Per profile");
        SaveModeSetting(active_mode);
        last_mode = active_mode;

        /*-------------------------------------------------*\
        | Switching the mode only shows what the mouse has; |
        | nothing is written until a colour is applied      |
        \*-------------------------------------------------*/
        if(active_mode == MODE_PER_PROFILE)
        {
            ShowDeviceColors();
        }
        else if(active_mode == MODE_STATIC)
        {
            ShowStaticColor(profile);
        }

        return;
    }

    if(active_mode == MODE_STATIC)
    {
        /*-------------------------------------------------*\
        | A colour applied in OpenRGB (also quick colours)  |
        | goes into the active profile once; switching      |
        | profiles writes nothing, OpenRGB then shows the   |
        | colour of the new profile                         |
        \*-------------------------------------------------*/
        if(apply)
        {
            ApplyStatic(profile);
        }
        else if(profile_changed)
        {
            ShowStaticColor(profile);
        }
    }
    else if(active_mode == MODE_PER_PROFILE)
    {
        if(apply)
        {
            leds_dirty = true;
        }

        CyclePerProfile();
    }
}

bool M6980XPlugin::ApplyProfileRequest(unsigned char profile)
{
    nlohmann::json data;

    {
        std::lock_guard<std::mutex> lock(request_mutex);

        if(!has_profile_request)
        {
            return false;
        }

        data                = profile_request;
        has_profile_request = false;
    }

    int mode = (data["mode"] == "static") ? MODE_STATIC : MODE_PER_PROFILE;

    if(data.contains("static_color") && data["static_color"].is_number_unsigned())
    {
        controller->SetModeColor(MODE_STATIC, 0, data["static_color"].get<RGBColor>());
    }

    if(data.contains("profile_colors") && data["profile_colors"].is_array() && data["profile_colors"].size() == m6980x::PROFILES)
    {
        for(unsigned int p = 0; p < m6980x::PROFILES; p++)
        {
            if(data["profile_colors"][p].is_number_unsigned())
            {
                controller->SetColor(p, data["profile_colors"][p].get<RGBColor>());
            }
        }
    }

    if(mode != last_mode)
    {
        SaveModeSetting(mode);
        last_mode = mode;
    }

    /*-----------------------------------------------------*\
    | SetActiveMode also signals the GUI; the UpdateMode    |
    | callback it triggers finds nothing left to write      |
    \*-----------------------------------------------------*/
    controller->SetActiveMode(mode);

    M6980X_LOG(LL_INFO, "OpenRGB profile loaded, mode %s", mode == MODE_STATIC ? "Static" : "Per profile");

    if(mode == MODE_STATIC)
    {
        ApplyStatic(profile);
    }
    else
    {
        /*-------------------------------------------------*\
        | Written by CyclePerProfile once the colours       |
        | stayed unchanged for SETTLE_TIME                  |
        \*-------------------------------------------------*/
        leds_dirty = true;
        CyclePerProfile();
    }

    return true;
}

bool M6980XPlugin::ShowStaticColor(unsigned char profile)
{
    m6980x::Levels levels;
    m6980x::Status status = mouse->LedColor(profile, levels);

    if(status != m6980x::Status::Ok)
    {
        HandleError(status, "reading the profile colour");
        return false;
    }

    controller->SetModeColor(MODE_STATIC, 0, ColorFromLevels(levels));
    controller->SignalUpdate(RGBCONTROLLER_UPDATE_REASON_UPDATEMODE);
    return true;
}

void M6980XPlugin::ApplyStatic(unsigned char profile)
{
    if(controller->GetModeColorsCount(MODE_STATIC) == 0)
    {
        return;
    }

    m6980x::Levels levels   = LevelsFromColor(controller->GetModeColor(MODE_STATIC, 0));
    bool written            = false;
    m6980x::Status status   = mouse->SetLedColor(profile, levels, written);

    if(status != m6980x::Status::Ok)
    {
        HandleError(status, "setting the colour");
        return;
    }

    if(written)
    {
        char text[256];
        snprintf(text, sizeof(text), "Connected (%s), Static: profile %d set to colour levels %u %u %u",
                 transport->Path().c_str(), profile + 1, levels[0], levels[1], levels[2]);
        SetStatus(text);
        M6980X_LOG(LL_INFO, "profile %d set to colour levels %u %u %u", profile + 1, levels[0], levels[1], levels[2]);
    }
}

void M6980XPlugin::CyclePerProfile()
{
    if(!leds_dirty)
    {
        return;
    }

    /*-----------------------------------------------------*\
    | Wait until the colours stayed unchanged for a while   |
    \*-----------------------------------------------------*/
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    bool changed = false;

    for(unsigned int p = 0; p < m6980x::PROFILES; p++)
    {
        m6980x::Levels levels = LevelsFromColor(controller->GetColor(p));

        if(levels != pending[p])
        {
            pending[p]  = levels;
            changed     = true;
        }
    }

    if(changed)
    {
        pending_since = now;
        return;
    }

    if(now - pending_since < SETTLE_TIME)
    {
        return;
    }

    leds_dirty = false;

    for(unsigned char p = 0; p < m6980x::PROFILES; p++)
    {
        bool            written = false;
        m6980x::Status  status  = mouse->SetLedColor(p, pending[p], written);

        if(status != m6980x::Status::Ok)
        {
            leds_dirty = true;
            HandleError(status, "setting a profile colour");
            return;
        }

        if(written)
        {
            char text[256];
            snprintf(text, sizeof(text), "Connected (%s), Per profile: profile %d set to colour levels %u %u %u",
                     transport->Path().c_str(), p + 1, pending[p][0], pending[p][1], pending[p][2]);
            SetStatus(text);
            M6980X_LOG(LL_INFO, "profile %d set to colour levels %u %u %u", p + 1, pending[p][0], pending[p][1], pending[p][2]);
        }
    }
}

bool M6980XPlugin::ShowDeviceColors()
{
    for(unsigned char p = 0; p < m6980x::PROFILES; p++)
    {
        m6980x::Levels  levels;
        m6980x::Status  status = mouse->LedColor(p, levels);

        if(status != m6980x::Status::Ok)
        {
            HandleError(status, "reading the profile colours");
            return false;
        }

        controller->SetColor(p, ColorFromLevels(levels));
        pending[p] = levels;
    }

    leds_dirty = false;
    controller->SignalUpdate(RGBCONTROLLER_UPDATE_REASON_UPDATELEDS);
    return true;
}

int M6980XPlugin::LoadModeSetting()
{
    nlohmann::json settings = api->GetSettings(SETTINGS_KEY);

    if(settings.contains("mode") && settings["mode"].is_string() && settings["mode"] == "static")
    {
        return MODE_STATIC;
    }

    return MODE_PER_PROFILE;
}

void M6980XPlugin::SaveModeSetting(int mode)
{
    nlohmann::json settings = api->GetSettings(SETTINGS_KEY);

    settings["mode"] = (mode == MODE_STATIC) ? "static" : "per_profile";
    api->SetSettings(SETTINGS_KEY, settings);
    api->SaveSettings();
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
