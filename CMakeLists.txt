cmake_minimum_required(VERSION 3.22)

# PaperLibrary development version.
set(RELEASE_SERVICE_VERSION_MAJOR "26")
set(RELEASE_SERVICE_VERSION_MINOR "07")
set(RELEASE_SERVICE_VERSION_MICRO "70")
set(RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

project(PaperLibrary VERSION ${RELEASE_SERVICE_VERSION})

set(QT_REQUIRED_VERSION "6.6.0")
set(KF_REQUIRED_VERSION "5.240.0")

find_package(ECM ${KF_REQUIRED_VERSION} CONFIG REQUIRED)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})

include(ECMAddAppIcon)
include(ECMAddTests)
include(ECMInstallIcons)
include(FeatureSummary)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEInstallDirs)
include(KDECMakeSettings)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 ${QT_REQUIRED_VERSION} CONFIG REQUIRED COMPONENTS Core Gui Sql Test Widgets Svg Xml WebEngineWidgets PdfWidgets)

find_package(KF6 ${KF_REQUIRED_VERSION} REQUIRED COMPONENTS
    Archive
    Config
    ConfigWidgets
    CoreAddons
    Crash
    I18n
    IconThemes
    KIO
    WidgetsAddons
    WindowSystem
    XmlGui
)

# DBus keeps the "open in existing instance" path. It is enabled by default
# on Unix desktop platforms where a session bus is normally present.
set(USE_DBUS_DEFAULT OFF)
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT HAIKU)
    set(USE_DBUS_DEFAULT ON)
endif()
option(USE_DBUS "Build DBus interprocess-open support" ${USE_DBUS_DEFAULT})
if(USE_DBUS)
    find_package(Qt6DBus REQUIRED)
    set(HAVE_DBUS 1)
else()
    set(HAVE_DBUS 0)
endif()

if(NOT WIN32 AND NOT ANDROID AND NOT APPLE AND NOT HAIKU)
    if(Qt6Gui_VERSION VERSION_GREATER_EQUAL "6.10.0")
        find_package(Qt6GuiPrivate ${QT_REQUIRED_VERSION} REQUIRED)
    else()
        find_package(Qt6Gui REQUIRED COMPONENTS Private)
    endif()
    set(HAVE_X11 TRUE)
else()
    set(HAVE_X11 FALSE)
endif()

set(HAVE_PURPOSE 0)
set(HAVE_SPEECH 0)
set(HAVE_KWALLET 0)
set(HAVE_JS 0)
set(HAVE_PHONON 0)

add_definitions(-DTRANSLATION_DOMAIN="paperlibrary")
add_definitions(-DQT_DEPRECATED_WARNINGS_SINCE=0x060600)
add_definitions(-DKF_DEPRECATED_WARNINGS_SINCE=0x05F000)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

include(PaperLibraryConfigureChecks.cmake)

set(BUILD_DESKTOP ON)

add_subdirectory(shell)

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

install(FILES paperlibrary.categories DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR})

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

message("-- Building PaperLibrary desktop shell: ${BUILD_DESKTOP}")
