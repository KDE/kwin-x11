#######################################
# Config
set(kwin_magnifier_config_SRCS magnifier_config.cpp)
ki18n_wrap_ui(kwin_magnifier_config_SRCS magnifier_config.ui)
qt5_add_dbus_interface(kwin_magnifier_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)
kconfig_add_kcfg_files(kwin_magnifier_config_SRCS magnifierconfig.kcfgc)

add_library(kwin_magnifier_config MODULE ${kwin_magnifier_config_SRCS})

target_link_libraries(kwin_magnifier_config
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_magnifier_config magnifier_config.desktop)

install(
    TARGETS
        kwin_magnifier_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
