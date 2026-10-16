#-----------------------------------------------------------------------------------------------#
# OpenRGB 1.0 plugin for the Gigabyte GHOST M6980X mouse                                        #
#                                                                                               #
# Build against the same Qt major version and compiler as the OpenRGB you load it into          #
# (official Windows 1.0 build: Qt 6.8.3, MSVC 2022, x64).                                       #
#-----------------------------------------------------------------------------------------------#

QT          += core gui widgets
TEMPLATE     = lib
CONFIG      += plugin c++17 silent
TARGET       = OpenRGB-M6980X

isEmpty(OPENRGB_SOURCE): OPENRGB_SOURCE = $$PWD/OpenRGB

INCLUDEPATH +=                                                                                  \
    src                                                                                         \
    $$OPENRGB_SOURCE                                                                            \
    $$OPENRGB_SOURCE/RGBController                                                              \
    $$OPENRGB_SOURCE/dependencies/json                                                          \

HEADERS +=                                                                                      \
    src/HidTransport.h                                                                          \
    src/M6980XProtocol.h                                                                        \
    src/M6980XPlugin.h                                                                          \

SOURCES +=                                                                                      \
    src/M6980XProtocol.cpp                                                                      \
    src/M6980XPlugin.cpp                                                                        \

DISTFILES += M6980XPlugin.json

win32 {
    SOURCES         += src/HidTransport_Windows.cpp
    LIBS            += -lsetupapi -lhid
    DEFINES         += _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    QMAKE_CXXFLAGS  += /utf-8
}

unix:!macx {
    SOURCES         += src/HidTransport_Linux.cpp
    target.path      = $$PREFIX/lib/openrgb/plugins
    INSTALLS        += target
}
