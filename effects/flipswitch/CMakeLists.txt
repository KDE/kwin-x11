#######################################
# Config
set(kwin_flipswitch_config_SRCS flipswitch_config.cpp)
ki18n_wrap_ui(kwin_flipswitch_config_SRCS flipswitch_config.ui)
qt5_add_dbus_interface(kwin_flipswitch_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)
kconfig_add_kcfg_files(kwin_flipswitch_config_SRCS flipswitchconfig.kcfgc)

add_library(kwin_flipswitch_config MODULE ${kwin_flipswitch_config_SRCS})

target_link_libraries(kwin_flipswitch_config
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_flipswitch_config flipswitch_config.desktop)

install(
    TARGETS
        kwin_flipswitch_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
