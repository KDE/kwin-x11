project(KWIN)

cmake_minimum_required(VERSION 2.8.12 FATAL_ERROR)
set(QT_MIN_VERSION "5.3.0")

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules ${CMAKE_MODULE_PATH} )

find_package(ECM 0.0.11 REQUIRED NO_MODULE)

include(FeatureSummary)
include(WriteBasicConfigVersionFile)
include(GenerateExportHeader)

# where to look first for cmake modules, before ${CMAKE_ROOT}/Modules/ is checked
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})

find_package(Qt5 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS
    Concurrent
    Core
    DBus
    Quick
    Script
    UiTools
    Widgets
    X11Extras
)

find_package(Qt5Test ${QT_MIN_VERSION} CONFIG QUIET)
set_package_properties(Qt5Test PROPERTIES
         PURPOSE "Required for tests"
         TYPE OPTIONAL
         )
add_feature_info("Qt5Test" Qt5Test_FOUND "Required for building tests")
if (NOT Qt5Test_FOUND)
    set(BUILD_TESTING OFF CACHE BOOL "Build the testing tree.")
endif()

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings)

include(ECMInstallIcons)

add_definitions(-DQT_DISABLE_DEPRECATED_BEFORE=0)

# require at least gcc 4.8
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if ("${CMAKE_CXX_COMPILER_VERSION}" VERSION_LESS "4.8")
        message(SEND_ERROR "Version ${CMAKE_CXX_COMPILER_VERSION} of the ${CMAKE_CXX_COMPILER_ID} C++ compiler is not supported. Please use version 4.8 or later.")
    endif()
endif()

find_package(Qt5Multimedia QUIET)
set_package_properties(Qt5Multimedia PROPERTIES
         PURPOSE "Runtime-only dependency for effect video playback"
         TYPE RUNTIME
         )


# required frameworks by Core
find_package(KF5 REQUIRED COMPONENTS
    Config
    ConfigWidgets
    CoreAddons
    Crash
    GlobalAccel
    I18n
    Init
    Notifications
    Service
    Plasma
    WidgetsAddons
    WindowSystem
)
# required frameworks by config modules
find_package(KF5 REQUIRED COMPONENTS
    Completion
    Declarative
    KCMUtils
    KIO
    NewStuff
    XmlGui
)

# optional frameworks
find_package(KF5Activities CONFIG)
set_package_properties(KF5Activities PROPERTIES
         PURPOSE "Enable building of KWin with kactivities support"
         TYPE OPTIONAL
         )
add_feature_info("KF5Activities" KF5Activities_FOUND "Enable building of KWin with kactivities support")

find_package(KF5DocTools CONFIG)
set_package_properties(KF5DocTools PROPERTIES
         PURPOSE "Enable building documentation"
         TYPE OPTIONAL
         )
add_feature_info("KF5DocTools" KF5DocTools_FOUND "Enable building documentation")

if(${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GL")
    find_package(OpenGL)
    set_package_properties(OpenGL PROPERTIES DESCRIPTION "The OpenGL libraries"
                        URL "http://www.opengl.org"
                        TYPE REQUIRED
                        )
else()
    find_package(OpenGLES)
    set_package_properties(OpenGLES PROPERTIES DESCRIPTION "The OpenGLES libraries"
                        URL "http://www.khronos.org/opengles"
                        TYPE REQUIRED
                        )
endif()

find_package(EGL)
set_package_properties(EGL PROPERTIES
                       TYPE REQUIRED
                       PURPOSE "Required to build KWin with EGL support"
                      )

find_package(Wayland 1.2 COMPONENTS Client Egl Cursor)
set_package_properties(Wayland PROPERTIES
                       TYPE OPTIONAL
                       PURPOSE "Required for building KWin with Wayland support"
                      )
add_feature_info("Wayland-Client" Wayland_Client_FOUND "Required for building the Wayland backend in KWin")
add_feature_info("Wayland-EGL" Wayland_Egl_FOUND "Required for building the Wayland EGL compositing backend in KWin")

find_package(XKB 0.4.1)
set_package_properties(XKB PROPERTIES
                       TYPE OPTIONAL
                       PURPOSE "Required for building KWin with Wayland support"
                      )

find_package(X11)
set_package_properties(X11 PROPERTIES DESCRIPTION "X11 libraries"
                        URL "http://www.x.org"
                        TYPE REQUIRED
                    )

# All the required XCB components
find_package(XCB
    REQUIRED COMPONENTS
        XCB
        XFIXES
        DAMAGE
        COMPOSITE
        SHAPE
        SYNC
        RENDER
        RANDR
        KEYSYMS
        IMAGE
        SHM
        XTEST
    OPTIONAL_COMPONENTS
        ICCCM
)
set_package_properties(XCB PROPERTIES TYPE REQUIRED)

# and the optional XCB dependencies
if (XCB_ICCCM_VERSION VERSION_LESS "0.4")
    set(XCB_ICCCM_FOUND FALSE)
endif()
add_feature_info("XCB-ICCCM" XCB_ICCCM_FOUND "Required for building test applications for KWin")

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)

########### configure tests ###############
include(CMakeDependentOption)

option(KWIN_BUILD_DECORATIONS "Enable building of KWin decorations." ON)
option(KWIN_BUILD_KCMS "Enable building of KWin configuration modules." ON)
option(KWIN_BUILD_TABBOX "Enable building of KWin Tabbox functionality" ON)
option(KWIN_BUILD_SCREENEDGES "Enable building of KWin with screen edge support" ON)
option(KWIN_BUILD_KAPPMENU "Enable building of KWin with application menu support" OFF)
option(KWIN_BUILD_XRENDER_COMPOSITING "Enable building of KWin with XRender Compositing support" ON)
cmake_dependent_option(KWIN_BUILD_ACTIVITIES "Enable building of KWin with kactivities support" ON "KF5Activities_FOUND" OFF)
option(KWIN_PLASMA_ACTIVE "Enable building KWin for Plasma Active." OFF)

option(KWIN_BUILD_COVERAGE "Build KWin with gcov support" OFF)

if(KWIN_BUILD_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lgcov")
endif()

# Binary name of KWin
set(KWIN_NAME "kwin")
set(KWIN_INTERNAL_NAME_X11 "kwin_x11")
set(KWIN_INTERNAL_NAME_WAYLAND "kwin_wayland")
set(KWIN_VERSION_MAJOR 5)
set(KWIN_VERSION_MINOR 0)
set(KWIN_VERSION_PATCH 2)
set(KWIN_VERSION ${KWIN_VERSION_MAJOR}.${KWIN_VERSION_MINOR}.${KWIN_VERSION_PATCH} )
set(KWIN_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(GENERIC_LIB_SOVERSION ${KWIN_VERSION_MAJOR})

set(GENERIC_LIB_VERSION "${KWIN_VERSION_MAJOR}.${KWIN_VERSION_MINOR}.${KWIN_VERSION_PATCH}")

if(KWIN_PLASMA_ACTIVE)
    set(KWIN_BUILD_DECORATIONS OFF)
    set(KWIN_BUILD_KCMS OFF)
    set(KWIN_BUILD_SCREENEDGES OFF)
    set(KWIN_BUILD_XRENDER_COMPOSITING OFF)
    set(KWIN_BUILD_WITH_OPENGLES ON)
    set(KWIN_NAME "kwinactive")
endif()

cmake_dependent_option(KWIN_BUILD_KAPPMENU "Build without appmenu support" ON "KWIN_BUILD_DECORATIONS" FALSE)

set(KWIN_BUILD_OPENGL FALSE)
set(KWIN_BUILD_OPENGLES FALSE)
if(OPENGL_FOUND AND (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GL"))
    set(KWIN_BUILD_OPENGL TRUE)
    message("Building KWin with OpenGL support")
endif()
if(OPENGLES_FOUND AND (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GLESv2"))
    set(KWIN_BUILD_OPENGLES TRUE)
    message("Building KWin with OpenGL ES 2 support")
endif()

if(NOT KWIN_BUILD_OPENGL AND NOT KWIN_BUILD_OPENGLES)
    message(FATAL_ERROR "KWin needs to be built against either OpenGL or OpenGL ES 2. Check your Qt config!")
endif()

# KWIN_HAVE_XRENDER_COMPOSITING - whether XRender-based compositing support is available: may be disabled
if( KWIN_BUILD_XRENDER_COMPOSITING )
    set( KWIN_HAVE_XRENDER_COMPOSITING 1 )
endif()

if(KWIN_BUILD_OPENGL)
  include_directories(${OPENGL_INCLUDE_DIR})
endif()

set(KWIN_HAVE_EGL ${EGL_FOUND})

if(KWIN_BUILD_OPENGLES)
  include_directories(${OPENGLES_INCLUDE_DIR})
endif()

if (XKB_FOUND)
  include_directories(${XKB_INCLUDE_DIR})
endif()

# for things that are also used by kwin libraries
configure_file(libkwineffects/kwinconfig.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/libkwineffects/kwinconfig.h )
# for kwin internal things
if(Wayland_Client_FOUND AND XKB_FOUND)
    set(HAVE_WAYLAND ${Wayland_Client_FOUND})
    set(HAVE_XKB ${XKB_FOUND})
    set(HAVE_WAYLAND_EGL ${Wayland_Egl_FOUND})
else()
    set(HAVE_WAYLAND FALSE)
    set(HAVE_XKB FALSE)
    set(HAVE_WAYLAND_EGL FALSE)
endif()

include(CheckIncludeFiles)
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(malloc.h HAVE_MALLOC_H)
configure_file(config-kwin.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-kwin.h )


########### global ###############
set(kwin_effects_dbus_xml ${CMAKE_CURRENT_SOURCE_DIR}/org.kde.kwin.Effects.xml)

include_directories(BEFORE
    ${CMAKE_CURRENT_BINARY_DIR}/libkdecorations
    ${CMAKE_CURRENT_BINARY_DIR}/libkwineffects
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libkwineffects
    ${CMAKE_CURRENT_SOURCE_DIR}/libkdecorations
    ${CMAKE_CURRENT_SOURCE_DIR}/effects
    ${CMAKE_CURRENT_SOURCE_DIR}/tabbox
    )

add_subdirectory( libkdecorations )
add_subdirectory( libkwineffects )
add_subdirectory( killer )
if(KWIN_BUILD_KCMS)
    add_subdirectory( kcmkwin )
endif()

if( KWIN_BUILD_DECORATIONS )
    add_subdirectory( clients )
endif()
add_subdirectory( data )

add_subdirectory( effects )
add_subdirectory( scripts )
add_subdirectory( tabbox )
add_subdirectory(scripting)

add_definitions(-DKDE_DEFAULT_DEBUG_AREA=1212)

########### next target ###############

set(kwin_KDEINIT_SRCS
   workspace.cpp 
   dbusinterface.cpp
   client.cpp 
   client_machine.cpp
   cursor.cpp
   tabgroup.cpp
   focuschain.cpp
   globalshortcuts.cpp
   input.cpp
   netinfo.cpp
   placement.cpp 
   atoms.cpp 
   utils.cpp 
   layers.cpp 
   main.cpp 
   options.cpp 
   outline.cpp
   decorations.cpp
   events.cpp 
   killwindow.cpp 
   geometrytip.cpp 
   screens.cpp
   shadow.cpp
   sm.cpp 
   group.cpp 
   bridge.cpp 
   manage.cpp 
   overlaywindow.cpp
   activation.cpp 
   useractions.cpp 
   geometry.cpp 
   rules.cpp
   composite.cpp
   toplevel.cpp
   unmanaged.cpp
   scene.cpp
   scene_xrender.cpp
   scene_opengl.cpp
   scene_qpainter.cpp
   glxbackend.cpp
   thumbnailitem.cpp
   lanczosfilter.cpp
   deleted.cpp
   effects.cpp
   effectloader.cpp
   compositingprefs.cpp
   paintredirector.cpp
   virtualdesktops.cpp
   xcbutils.cpp
    scripting/scripting.cpp
    scripting/workspace_wrapper.cpp
    scripting/meta.cpp
    scripting/scriptedeffect.cpp
    scripting/scriptingutils.cpp
    scripting/timer.cpp
    scripting/scripting_model.cpp
    scripting/dbuscall.cpp
    scripting/screenedgeitem.cpp
   )

if(KWIN_BUILD_TABBOX)
    set(
        kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS}
        tabbox/tabbox.cpp
        tabbox/clientmodel.cpp
        tabbox/desktopchain.cpp
        tabbox/desktopmodel.cpp
        tabbox/switcheritem.cpp
        tabbox/tabboxconfig.cpp
        tabbox/tabboxhandler.cpp
  )
endif()

if(KWIN_BUILD_SCREENEDGES)
    set(
        kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS}
        screenedge.cpp
    )
endif()

if(KWIN_BUILD_KAPPMENU)
    set(
        kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS}
        appmenu.cpp
    )
    qt5_add_dbus_interface(kwin_KDEINIT_SRCS
        ${CMAKE_SOURCE_DIR}/plasma-workspace/appmenu/org.kde.kappmenu.xml appmenu_interface)
endif()

if(KWIN_BUILD_ACTIVITIES)
    set(
        kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS}
        activities.cpp
    )
endif()

if(KWIN_HAVE_EGL)
    set(kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS} eglonxbackend.cpp)
endif()

if(Wayland_Client_FOUND AND XKB_FOUND)
    set(kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS} wayland_backend.cpp)
    if(KWIN_HAVE_EGL AND Wayland_Egl_FOUND)
        set(kwin_KDEINIT_SRCS ${kwin_KDEINIT_SRCS} egl_wayland_backend.cpp)
    endif()
endif()

kconfig_add_kcfg_files(kwin_KDEINIT_SRCS settings.kcfgc)

qt5_add_dbus_adaptor( kwin_KDEINIT_SRCS org.kde.KWin.xml dbusinterface.h KWin::DBusInterface )
qt5_add_dbus_adaptor( kwin_KDEINIT_SRCS org.kde.kwin.Compositing.xml dbusinterface.h KWin::CompositorDBusInterface )
qt5_add_dbus_adaptor( kwin_KDEINIT_SRCS ${kwin_effects_dbus_xml} effects.h KWin::EffectsHandlerImpl )

qt5_add_dbus_interface( kwin_KDEINIT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/org.freedesktop.ScreenSaver.xml screenlocker_interface)

qt5_add_resources( kwin_KDEINIT_SRCS resources.qrc )

qt5_wrap_ui(kwin_KDEINIT_SRCS
    shortcutdialog.ui
)

########### target link libraries ###############

set(kwin_OWN_LIBS
    kdecorations
    kwineffects
    kwinxrenderutils
    kwin4_effect_builtins
)

set(kwin_QT_LIBS
    Qt5::Concurrent
    Qt5::DBus
    Qt5::Quick
    Qt5::Script
    Qt5::X11Extras
)

set(kwin_KDE_LIBS
    KF5::ConfigCore
    KF5::CoreAddons
    KF5::ConfigWidgets
    KF5::Crash
    KF5::GlobalAccel
    KF5::I18n
    KF5::Notifications
    KF5::Service
    KF5::Plasma
    KF5::WindowSystem
)

set(kwin_XLIB_LIBS
    ${X11_X11_LIB}
    ${X11_Xext_LIB}
    ${X11_Xcursor_LIB}
    ${X11_ICE_LIB}
    ${X11_SM_LIB}
)

set(kwin_XCB_LIBS
    XCB::XCB
    XCB::XFIXES
    XCB::DAMAGE
    XCB::COMPOSITE
    XCB::SHAPE
    XCB::SYNC
    XCB::RENDER
    XCB::RANDR
    XCB::KEYSYMS
    XCB::SHM
    XCB::XTEST
)

set(kwin_WAYLAND_LIBS
    Wayland::Client
    Wayland::Cursor
    XCB::XTEST
    XKB::XKB
)

set(kwin_WAYLAND_EGL_LIBS
    Wayland::Egl
)

set(kwin_OPENGL_LIBS )

find_library(XF86VM_LIBRARY Xxf86vm)
if (XF86VM_LIBRARY)
    set(kwin_XLIB_LIBS ${kwin_XLIB_LIBS} ${XF86VM_LIBRARY})
else()
    add_definitions(-DKWIN_NO_XF86VM)
endif()

if(KWIN_BUILD_ACTIVITIES)
    set(kwin_KDE_LIBS ${kwin_KDE_LIBS} KF5::Activities)
endif()

if(KWIN_HAVE_EGL)
    set(kwin_OPENGL_LIBS ${kwin_OPENGL_LIBS} EGL::EGL)
endif()

set(kwinLibs
    ${kwin_OWN_LIBS}
    ${kwin_QT_LIBS}
    ${kwin_KDE_LIBS}
    ${kwin_XLIB_LIBS}
    ${kwin_XCB_LIBS}
    ${kwin_OPENGL_LIBS}
)

if(Wayland_Client_FOUND AND XKB_FOUND)
    set(kwinLibs ${kwinLibs} ${kwin_WAYLAND_LIBS})
    if(KWIN_HAVE_EGL AND Wayland_Egl_FOUND)
        set(kwinLibs ${kwinLibs} ${kwin_WAYLAND_EGL_LIBS})
    endif()
endif()

kf5_add_kdeinit_executable( kwin ${kwin_KDEINIT_SRCS})

target_link_libraries(kdeinit_kwin ${kwinLibs})
set_target_properties(kwin PROPERTIES OUTPUT_NAME ${KWIN_INTERNAL_NAME_X11})
generate_export_header(kdeinit_kwin EXPORT_FILE_NAME kwin_export.h)

if(KWIN_BUILD_OPENGL)
  target_link_libraries(kdeinit_kwin kwinglutils ${OPENGL_gl_LIBRARY})
  # -ldl used by OpenGL code
  find_library(DL_LIBRARY dl)
  if (DL_LIBRARY)
    target_link_libraries(kdeinit_kwin ${DL_LIBRARY})
  endif()
elseif(KWIN_BUILD_OPENGLES)
  target_link_libraries(kdeinit_kwin ${kwinLibs} kwinglesutils ${OPENGLES_LIBRARIES})
  set_target_properties(kdeinit_kwin PROPERTIES COMPILE_FLAGS "-DKWIN_HAVE_OPENGLES")
endif()

install(TARGETS kdeinit_kwin ${INSTALL_TARGETS_DEFAULT_ARGS} )
install(TARGETS kwin         ${INSTALL_TARGETS_DEFAULT_ARGS} )

########### install files ###############

install( FILES kwin.kcfg  DESTINATION  ${KCFG_INSTALL_DIR} RENAME ${KWIN_NAME}.kcfg )
install( FILES kwin.notifyrc  DESTINATION  ${KNOTIFYRC_INSTALL_DIR} RENAME ${KWIN_NAME}.notifyrc)
install(
    FILES
        org.kde.KWin.xml
        org.kde.kwin.Compositing.xml
        org.kde.kwin.Effects.xml
    DESTINATION
        ${DBUS_INTERFACES_INSTALL_DIR}
)
install( FILES ${CMAKE_CURRENT_BINARY_DIR}/kwin_export.h DESTINATION ${INCLUDE_INSTALL_DIR} COMPONENT Devel)

# Install the KWin/Script service type
install( FILES scripting/kwinscript.desktop DESTINATION ${SERVICETYPES_INSTALL_DIR} )

ecm_install_icons(
    ICONS
        16-apps-kwin.png
        32-apps-kwin.png
        48-apps-kwin.png
        sc-apps-kwin.svgz
    DESTINATION
        ${ICON_INSTALL_DIR}
    THEME
        hicolor
)

add_subdirectory(qml)
add_subdirectory(autotests)
add_subdirectory(tests)

if (KF5DocTools_FOUND)
    add_subdirectory(doc)
endif()

set(CMAKECONFIG_INSTALL_DIR "${CMAKECONFIG_INSTALL_PREFIX}/KWinDBusInterface")
ecm_configure_package_config_file(KWinDBusInterfaceConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/KWinDBusInterfaceConfig.cmake"
    INSTALL_DESTINATION ${CMAKECONFIG_INSTALL_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/KWinDBusInterfaceConfig.cmake
    DESTINATION ${CMAKECONFIG_INSTALL_DIR})
