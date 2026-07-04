
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}/../
)

find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets PdfWidgets)

set(paperlibrary_SRCS
   main.cpp
   paperlibrary_main.cpp
   shell.cpp
   epubwebreader.cpp
   pdfview.cpp
   shellutils.cpp
   chrometabbar.cpp
   chrometabwidget.cpp
   chrometoolbar.cpp
   librarystore.cpp
   libraryview.cpp
   paperlibrarymodel.cpp
   coverheuristic.cpp
   covergenerator.cpp
   epubcover.cpp
   epubimporter.cpp
   claudeprocesshelper.cpp
   libraryautotagger.cpp
   applebooksprogress.cpp

   shell.qrc
)

if(APPLE)
    # PaperLibrary bundle icon; icons/paperlibrary.svg is the design source
    # (rendered via qlmanage, compiled with iconutil — see the SVG header).
    set(PAPERLIBRARY_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/icons/PaperLibrary.icns")
    set_source_files_properties(${PAPERLIBRARY_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    list(APPEND paperlibrary_SRCS ${PAPERLIBRARY_ICNS})

    # Chrome-style tabs-in-the-titlebar shim (Objective-C++). Enable the
    # Objective-C++ language and match the project's C++17 standard so the
    # shim can use Qt headers alongside AppKit.
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    list(APPEND paperlibrary_SRCS macostitlebar.mm macosscrolldebug.mm)
else()
    ecm_add_app_icon(paperlibrary_SRCS ICONS "${CMAKE_CURRENT_SOURCE_DIR}/icons/paperlibrary.svg")
endif()

add_executable(paperlibrary ${paperlibrary_SRCS})

target_link_libraries(paperlibrary
    KF6::Archive
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::CoreAddons
    KF6::Crash
    KF6::I18n
    KF6::IconThemes
    KF6::KIOGui
    KF6::WidgetsAddons
    KF6::WindowSystem
    KF6::XmlGui
    Qt6::Sql
    Qt6::WebEngineWidgets
    Qt6::PdfWidgets
)
if (TARGET Qt6::DBus)
    target_link_libraries(paperlibrary Qt6::DBus)
endif()
if (HAVE_X11)
    target_link_libraries(paperlibrary Qt6::GuiPrivate)
endif()

install(TARGETS paperlibrary ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

if(APPLE)
    # AppKit/Foundation back the titlebar-tabs shim (macostitlebar.mm).
    target_link_libraries(paperlibrary "-framework AppKit" "-framework Foundation")

    # CFBundleDisplayName comes from MACOSX_BUNDLE_BUNDLE_NAME via the plist
    # template (CMake has no display-name target property).
    set_target_properties(paperlibrary PROPERTIES
        OUTPUT_NAME "PaperLibrary"
        MACOSX_BUNDLE_BUNDLE_NAME "PaperLibrary"
        MACOSX_BUNDLE_ICON_FILE "PaperLibrary"
        MACOSX_BUNDLE_LONG_VERSION_STRING "PaperLibrary 1.0.0-dev (${RELEASE_SERVICE_VERSION})"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0-dev"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/MacOSXBundleInfo.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.todd866.paperlibrary"
        MACOSX_BUNDLE_COPYRIGHT "2026 Ian Todd. Based on Okular, 2002-2026 The Okular Developers.")
endif()

# PaperLibrary shell data files.

install( PROGRAMS io.github.todd866.paperlibrary.desktop  DESTINATION  ${KDE_INSTALL_APPDIR} )
install( FILES io.github.todd866.paperlibrary.appdata.xml DESTINATION  ${KDE_INSTALL_METAINFODIR} )
