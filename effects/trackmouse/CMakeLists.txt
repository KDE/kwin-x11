#######################################
# Effect
# Data files
install( FILES
    data/tm_inner.png
    data/tm_outer.png
    DESTINATION ${DATA_INSTALL_DIR}/kwin )

#######################################
# Config
set(kwin_trackmouse_config_SRCS trackmouse_config.cpp)
ki18n_wrap_ui(kwin_trackmouse_config_SRCS trackmouse_config.ui)
qt5_add_dbus_interface(kwin_trackmouse_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)
kconfig_add_kcfg_files(kwin_trackmouse_config_SRCS trackmouseconfig.kcfgc)

add_library(kwin_trackmouse_config MODULE ${kwin_trackmouse_config_SRCS})

target_link_libraries(kwin_trackmouse_config
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_trackmouse_config trackmouse_config.desktop)

install(
    TARGETS
        kwin_trackmouse_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
