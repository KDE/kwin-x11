########### next target ###############
# KI18N Translation Domain for this library
add_definitions(-DTRANSLATION_DOMAIN=\"kcmkwm\")

set(kcm_kwinoptions_PART_SRCS windows.cpp mouse.cpp main.cpp )
ki18n_wrap_ui(kcm_kwinoptions_PART_SRCS actions.ui advanced.ui focus.ui mouse.ui moving.ui)
add_library(kcm_kwinoptions MODULE ${kcm_kwinoptions_PART_SRCS})
target_link_libraries(kcm_kwinoptions Qt5::DBus KF5::Completion KF5::I18n KF5::ConfigWidgets KF5::Service)
install(TARGETS kcm_kwinoptions  DESTINATION ${PLUGIN_INSTALL_DIR} )


########### install files ###############

install( FILES kwinoptions.desktop kwinactions.desktop kwinadvanced.desktop
    kwinfocus.desktop kwinmoving.desktop
    DESTINATION ${SERVICES_INSTALL_DIR})
