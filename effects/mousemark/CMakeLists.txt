#######################################
# Config
set(kwin_mousemark_config_SRCS mousemark_config.cpp)
ki18n_wrap_ui(kwin_mousemark_config_SRCS mousemark_config.ui)
qt5_add_dbus_interface(kwin_mousemark_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)
kconfig_add_kcfg_files(kwin_mousemark_config_SRCS mousemarkconfig.kcfgc)

add_library(kwin_mousemark_config MODULE ${kwin_mousemark_config_SRCS})

target_link_libraries(kwin_mousemark_config
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_mousemark_config mousemark_config.desktop)

install(
    TARGETS
        kwin_mousemark_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
