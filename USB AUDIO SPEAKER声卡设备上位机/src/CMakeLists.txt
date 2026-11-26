cmake_minimum_required(VERSION 3.21)

project(USBAUDIOSPEAKER VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Multimedia)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(appUSBAUDIOSPEAKER
    main.cpp
    audiocontroller.cpp
    audiocontroller.h
    hidstatus.cpp
    hidstatus.h
)

qt_add_qml_module(appUSBAUDIOSPEAKER
    URI USBAUDIOSPEAKER
    QML_FILES
        Main.qml
)

set_target_properties(appUSBAUDIOSPEAKER PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appUSBAUDIOSPEAKER
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Multimedia
)

# WASAPI 调节 Windows 软件端点音量；HID 接口只读回传板载状态。
if(WIN32)
    target_link_libraries(appUSBAUDIOSPEAKER PRIVATE ole32 uuid hid hidparse setupapi)
endif()

install(TARGETS appUSBAUDIOSPEAKER
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
