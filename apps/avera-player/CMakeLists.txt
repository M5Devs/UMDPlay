# avera-player — UMDPlay's Qt 6 Quick front end (AveraUI + the native core).

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Multimedia)

add_executable(avera-player main.cpp)

target_link_libraries(avera-player PRIVATE
    umdplay_core
    umdplay_logo
    avera_ui
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Multimedia)

qt_add_resources(avera-player "avera_app"
    PREFIX "/avera/app"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES Main.qml)
