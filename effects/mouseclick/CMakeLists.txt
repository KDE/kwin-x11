##########################
## configurtion dialog 
##########################
set(kwin_mouseclick_config_SRCS mouseclick_config.cpp)
ki18n_wrap_ui(kwin_mouseclick_config_SRCS mouseclick_config.ui)
qt5_add_dbus_interface(kwin_mouseclick_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)
kconfig_add_kcfg_files(kwin_mouseclick_config_SRCS mouseclickconfig.kcfgc)

add_library(kwin_mouseclick_config MODULE ${kwin_mouseclick_config_SRCS})

target_link_libraries(kwin_mouseclick_config
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_mouseclick_config mouseclick_config.desktop)

install(
    TARGETS
        kwin_mouseclick_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
