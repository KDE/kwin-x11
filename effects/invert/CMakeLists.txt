#######################################
# Effect

# Data files
install( FILES
    data/1.10/invert.frag
    DESTINATION ${DATA_INSTALL_DIR}/kwin/shaders/1.10 )
install( FILES
    data/1.40/invert.frag
    DESTINATION ${DATA_INSTALL_DIR}/kwin/shaders/1.40 )

#######################################
# Config
set(kwin_invert_config_SRCS invert_config.cpp)
qt5_add_dbus_interface(kwin_invert_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)

add_library(kwin_invert_config MODULE ${kwin_invert_config_SRCS})

target_link_libraries(kwin_invert_config
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_invert_config invert_config.desktop)

install(
    TARGETS
        kwin_invert_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
