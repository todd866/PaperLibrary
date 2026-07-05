add_definitions(-DKDESRCDIR="${CMAKE_CURRENT_SOURCE_DIR}/")

include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

ecm_add_test(shelltest.cpp ../shell/shellutils.cpp
    TEST_NAME "shelltest"
    LINK_LIBRARIES Qt6::Test KF6::CoreAddons KF6::I18n
)

ecm_add_test(librarystoretest.cpp ../shell/librarystore.cpp ../shell/applebooksprogress.cpp
    TEST_NAME "librarystoretest"
    LINK_LIBRARIES Qt6::Test Qt6::Sql KF6::ConfigCore
)

ecm_add_test(libraryautotaggertest.cpp ../shell/libraryautotagger.cpp ../shell/claudeprocesshelper.cpp ../shell/librarystore.cpp ../shell/paperlibrarymodel.cpp
    TEST_NAME "libraryautotaggertest"
    LINK_LIBRARIES Qt6::Test Qt6::Sql KF6::ConfigCore
)

ecm_add_test(paperlibrarymodeltest.cpp ../shell/paperlibrarymodel.cpp
    TEST_NAME "paperlibrarymodeltest"
    LINK_LIBRARIES Qt6::Test Qt6::Sql KF6::ConfigCore
)

ecm_add_test(libraryviewtest.cpp
    ../shell/libraryview.cpp
    ../shell/librarystore.cpp
    ../shell/paperlibrarymodel.cpp
    ../shell/coverheuristic.cpp
    ../shell/covergenerator.cpp
    ../shell/epubcover.cpp
    ../shell/epubimporter.cpp
    ../shell/applebooksprogress.cpp
    TEST_NAME "libraryviewtest"
    LINK_LIBRARIES Qt6::Test Qt6::Widgets Qt6::Sql KF6::Archive KF6::ConfigCore KF6::I18n
)

ecm_add_test(coverheuristictest.cpp ../shell/coverheuristic.cpp
    TEST_NAME "coverheuristictest"
    LINK_LIBRARIES Qt6::Test Qt6::Gui
)

ecm_add_test(covergeneratortest.cpp ../shell/covergenerator.cpp
    TEST_NAME "covergeneratortest"
    LINK_LIBRARIES Qt6::Test Qt6::Gui
)

ecm_add_test(epubcovertest.cpp ../shell/epubcover.cpp
    TEST_NAME "epubcovertest"
    LINK_LIBRARIES Qt6::Test Qt6::Gui KF6::Archive
)

ecm_add_test(epubimportertest.cpp ../shell/epubimporter.cpp ../shell/applebooksprogress.cpp
    TEST_NAME "epubimportertest"
    LINK_LIBRARIES Qt6::Test Qt6::Gui Qt6::Sql KF6::Archive
)

ecm_add_test(epubwebreadertest.cpp ../shell/epubwebreader.cpp
    TEST_NAME "epubwebreadertest"
    LINK_LIBRARIES Qt6::Test Qt6::Xml KF6::Archive
)
target_compile_definitions(epubwebreadertest PRIVATE PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY)

ecm_add_test(epubreadercorpustest.cpp ../shell/epubwebreader.cpp
    TEST_NAME "epubreadercorpustest"
    LINK_LIBRARIES Qt6::Core Qt6::Xml KF6::Archive
)
target_compile_definitions(epubreadercorpustest PRIVATE PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY)

set(_shell_mac_srcs "")
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    list(APPEND _shell_mac_srcs ../shell/macostitlebar.mm)
endif()

ecm_add_test(chromestriptest.cpp ../shell/chrometabbar.cpp ${_shell_mac_srcs}
    TEST_NAME "chromestriptest"
    LINK_LIBRARIES Qt6::Test Qt6::Widgets KF6::WidgetsAddons KF6::I18n KF6::ConfigCore
)
if(APPLE)
    target_link_libraries(chromestriptest "-framework AppKit" "-framework Foundation")
endif()
