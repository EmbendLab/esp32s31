cmake_minimum_required(VERSION 3.21)

project(usbip-qt-client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt6 路径：如果用命令行编译，请传入 -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/mingw_64
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)

qt_standard_project_setup()

qt_add_executable(usbip-qt-client
    src/main.cpp
    src/remote_client.h
    src/remote_client.cpp
    src/vhci.h
    src/vhci_client.h
    src/vhci_client.cpp
    src/mainwindow.h
    src/mainwindow.cpp
)

target_include_directories(usbip-qt-client PRIVATE src)

target_link_libraries(usbip-qt-client PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
)

if(WIN32)
    # SetupAPI / CfgMgr32 用于发现并打开内核驱动的设备接口
    target_link_libraries(usbip-qt-client PRIVATE setupapi cfgmgr32)
    target_compile_definitions(usbip-qt-client PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

set_target_properties(usbip-qt-client PROPERTIES
    WIN32_EXECUTABLE TRUE
)