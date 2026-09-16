# Shared FreeRDP configuration for the QtRdp applications.

lessThan(QT_MAJOR_VERSION, 6) {
    error("QtRdp requires Qt 6 or newer.")
}

isEmpty(FREERDP_API_VERSION) {
    FREERDP_API_VERSION = 3
}

isEmpty(FREERDP_INSTALL_PREFIX) {
    FREERDP_INSTALL_PREFIX = $$(FREERDP_INSTALL_PREFIX)
}

isEmpty(FREERDP_INSTALL_PREFIX) {
    QTRDP_REPOSITORY_ROOT = $$clean_path($$_PRO_FILE_PWD_/..)
    FREERDP_INSTALL_PREFIX = $$QTRDP_REPOSITORY_ROOT/build/freerdp-install
}

FREERDP_INSTALL_PREFIX = $$clean_path($$FREERDP_INSTALL_PREFIX)
FREERDP_INCLUDE_DIR = $$FREERDP_INSTALL_PREFIX/include/freerdp$${FREERDP_API_VERSION}
WINPR_INCLUDE_DIR = $$FREERDP_INSTALL_PREFIX/include/winpr$${FREERDP_API_VERSION}
FREERDP_LIBRARY_DIR = $$FREERDP_INSTALL_PREFIX/lib

!exists($$FREERDP_INCLUDE_DIR/freerdp/freerdp.h) {
    error("FreeRDP headers were not found under $$FREERDP_INCLUDE_DIR. Build and install FreeRDP first or set FREERDP_INSTALL_PREFIX.")
}

!exists($$WINPR_INCLUDE_DIR/winpr/winpr.h) {
    error("WinPR headers were not found under $$WINPR_INCLUDE_DIR. Build and install FreeRDP first or set FREERDP_INSTALL_PREFIX.")
}

!exists($$FREERDP_LIBRARY_DIR) {
    error("FreeRDP library directory was not found at $$FREERDP_LIBRARY_DIR.")
}

isEmpty(FREERDP_COMPONENTS) {
    error("FREERDP_COMPONENTS must be set before including qmake/freerdp.pri.")
}

INCLUDEPATH += \
    $$quote($$FREERDP_INCLUDE_DIR) \
    $$quote($$WINPR_INCLUDE_DIR)

DEPENDPATH += \
    $$quote($$FREERDP_INCLUDE_DIR) \
    $$quote($$WINPR_INCLUDE_DIR)

LIBS += -L$$quote($$FREERDP_LIBRARY_DIR)

for(component, FREERDP_COMPONENTS) {
    versionedLibrary = $${component}$${FREERDP_API_VERSION}

    win32 {
        libraryFile = $$FREERDP_LIBRARY_DIR/$${versionedLibrary}.lib
        !exists($$libraryFile) {
            error("Required FreeRDP library was not found at $$libraryFile.")
        }
    }

    LIBS += -l$${versionedLibrary}
}

message("Using FreeRDP from $$FREERDP_INSTALL_PREFIX")
