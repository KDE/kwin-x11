#######################################
# Effect
install( FILES
    main.qml
    DESTINATION ${DATA_INSTALL_DIR}/kwin/effects/presentwindows/
)

#######################################
# Config
set(kwin_presentwindows_config_SRCS presentwindows_config.cpp)
ki18n_wrap_ui(kwin_presentwindows_config_SRCS presentwindows_config.ui)
qt5_add_dbus_interface(kwin_presentwindows_config_SRCS ${kwin_effects_dbus_xml} kwineffects_interface)
kconfig_add_kcfg_files(kwin_presentwindows_config_SRCS presentwindowsconfig.kcfgc)

add_library(kwin_presentwindows_config MODULE ${kwin_presentwindows_config_SRCS})

target_link_libraries(kwin_presentwindows_config
    KF5::Completion
    KF5::ConfigWidgets
    KF5::GlobalAccel
    KF5::I18n
    KF5::Service
    KF5::XmlGui
)

kservice_desktop_to_json(kwin_presentwindows_config presentwindows_config.desktop)

install(
    TARGETS
        kwin_presentwindows_config
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kwin/effects/configs
)
