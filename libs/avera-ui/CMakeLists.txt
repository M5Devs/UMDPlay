# AveraUI — reusable Qt 6 Quick "10-foot" media shell.
#
# Deliberately player-agnostic: the host fills an AveraHub, implements
# AveraPlayerBackend and pushes decoded frames / PCM into AveraVideoItem and
# AveraAudioOutput. QML components ship in the library's resource bundle at
# :/avera/qml/AveraUI so any host can import the directory.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Multimedia)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_library(avera_ui SHARED
    include/avera_ui/avera_ui.h
    include/avera_ui/audio_output.h
    include/avera_ui/hub.h
    include/avera_ui/player_controller.h
    include/avera_ui/ui_sfx.h
    include/avera_ui/video_item.h
    src/avera_ui.cpp
    src/hub.cpp
    src/video_item.cpp
    src/player_controller.cpp
    src/audio_output.cpp
    src/ui_sfx.cpp
)

target_include_directories(avera_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(avera_ui PUBLIC
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Multimedia)

set_target_properties(avera_ui PROPERTIES
    OUTPUT_NAME avera_ui
    VERSION ${PROJECT_VERSION}
    SOVERSION 1)

qt_add_resources(avera_ui "avera_qml"
    PREFIX "/avera/qml"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml"
    FILES
        qml/AveraUI/qmldir
        qml/AveraUI/Theme.qml
        qml/AveraUI/Card.qml
        qml/AveraUI/HubView.qml
        qml/AveraUI/PlayerView.qml
        qml/AveraUI/TransportOsd.qml
)
