# Shared FreeRDP configuration for the QtRdp applications.

QTRDP_QT_VERSION = $$[QT_VERSION]
isEmpty(QTRDP_QT_VERSION) {
    QTRDP_QT_VERSION = $$QT_VERSION
}

isEmpty(QTRDP_QT_VERSION) {
    error("QtRdp could not determine the Qt version from qmake. Check the Kit selected in Qt Creator.")
}

lessThan(QTRDP_QT_VERSION, 6.0.0) {
    error("QtRdp requires Qt 6 or newer. The selected Kit reports Qt $${QTRDP_QT_VERSION} from $$[QT_INSTALL_PREFIX]. Select a Qt 6 MSVC2022 64-bit Kit in Qt Creator.")
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
FREERDP_RUNTIME_DIR = $$FREERDP_INSTALL_PREFIX/bin

!exists($$FREERDP_INCLUDE_DIR/freerdp/freerdp.h) {
    error("FreeRDP headers were not found under $${FREERDP_INCLUDE_DIR}. Run scripts/build-freerdp.ps1 or set FREERDP_INSTALL_PREFIX.")
}

!exists($$WINPR_INCLUDE_DIR/winpr/winpr.h) {
    error("WinPR headers were not found under $${WINPR_INCLUDE_DIR}. Run scripts/build-freerdp.ps1 or set FREERDP_INSTALL_PREFIX.")
}

!exists($$FREERDP_LIBRARY_DIR) {
    error("FreeRDP library directory was not found at $${FREERDP_LIBRARY_DIR}.")
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
            error("Required FreeRDP library was not found at $${libraryFile}.")
        }
    }

    LIBS += -l$${versionedLibrary}
}

win32 {
    FREERDP_RUNTIME_DLLS = $$files($$FREERDP_RUNTIME_DIR/*.dll, false)
    isEmpty(FREERDP_RUNTIME_DLLS) {
        error("FreeRDP runtime libraries were not found under $${FREERDP_RUNTIME_DIR}. Run scripts/build-freerdp.ps1 again.")
    }

    for(runtimeDll, FREERDP_RUNTIME_DLLS) {
        QMAKE_POST_LINK += $$QMAKE_COPY $$quote($$shell_path($$runtimeDll)) $$quote($(DESTDIR)) $$escape_expand(\\n\\t)
    }
}

message("Using FreeRDP from $$FREERDP_INSTALL_PREFIX")
