#cmakedefine KWIN_BUILD_DECORATIONS 1
#cmakedefine KWIN_BUILD_TABBOX 1
#cmakedefine KWIN_BUILD_DESKTOPCHANGEOSD 1
#cmakedefine KWIN_BUILD_SCREENEDGES 1
#cmakedefine KWIN_BUILD_KAPPMENU 1
#cmakedefine KWIN_BUILD_ACTIVITIES 1
#define KWIN_NAME "${KWIN_NAME}"
#define KWIN_INTERNAL_NAME_X11 "${KWIN_INTERNAL_NAME_X11}"
#define KWIN_CONFIG "${KWIN_NAME}rc"
#define KWIN_VERSION_STRING "${KWIN_VERSION}"
#define KWIN_KILLER_BIN "${CMAKE_INSTALL_PREFIX}/${LIBEXEC_INSTALL_DIR}/kwin_killer_helper"
#cmakedefine01 HAVE_WAYLAND
#cmakedefine01 HAVE_WAYLAND_EGL
#cmakedefine01 HAVE_XKB

/* Define to 1 if you have the <unistd.h> header file. */
#cmakedefine HAVE_UNISTD_H 1

/* Define to 1 if you have the <malloc.h> header file. */
#cmakedefine HAVE_MALLOC_H 1

#cmakedefine XCB_ICCCM_FOUND 1
#ifndef XCB_ICCCM_FOUND
#define XCB_ICCCM_WM_STATE_WITHDRAWN 0
#define XCB_ICCCM_WM_STATE_NORMAL 1
#define XCB_ICCCM_WM_STATE_ICONIC 3
#endif
