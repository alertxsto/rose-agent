include_guard(GLOBAL)
option(ROSE_ENABLE_PACKAGING "Enable Linux runtime deployment and DEB/RPM/TGZ packages" OFF)
if(NOT ROSE_ENABLE_PACKAGING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# Packages carry their own Qt. The distribution's older Qt must not satisfy or
# override the runtime used to build the application.
set(ROSE_QT_PREFIX "" CACHE PATH "Qt 6 installation prefix to redistribute")
set(ROSE_QT_SOURCE_DIR "" CACHE PATH "Matching official qt-everywhere source tree for license notices")
set(ROSE_QT_EXTRA_LICENSE_DIR "" CACHE PATH "Notices, source provenance and exact library manifest for non-Qt SDK shared libraries")
if(NOT TARGET Qt6::Core)
    message(FATAL_ERROR "Linux release packaging requires the modern Qt 6 build")
endif()
foreach(path IN ITEMS ROSE_QT_PREFIX ROSE_QT_SOURCE_DIR)
    if(NOT IS_ABSOLUTE "${${path}}" OR NOT IS_DIRECTORY "${${path}}")
        message(FATAL_ERROR "${path} must name an existing absolute directory")
    endif()
endforeach()
file(REAL_PATH "${ROSE_QT_PREFIX}" ROSE_QT_PREFIX)
file(REAL_PATH "${ROSE_QT_SOURCE_DIR}" ROSE_QT_SOURCE_DIR)
find_program(ROSE_PATCHELF_EXECUTABLE patchelf REQUIRED)
find_program(ROSE_OBJDUMP_EXECUTABLE objdump REQUIRED)
set(ROSE_PACKAGE_QT_VERSION "${Qt6Core_VERSION}")
if(NOT ROSE_PACKAGE_QT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Cannot determine the exact Qt 6 runtime version")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" ROSE_PACKAGE_QT_SERIES "${ROSE_PACKAGE_QT_VERSION}")
get_target_property(qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT qt_core_location)
    get_target_property(qt_core_location Qt6::Core IMPORTED_LOCATION)
endif()
if(NOT qt_core_location)
    message(FATAL_ERROR "Cannot identify the Qt Core library used for the release build")
endif()
file(REAL_PATH "${qt_core_location}" qt_core_location)
cmake_path(IS_PREFIX ROSE_QT_PREFIX "${qt_core_location}" NORMALIZE qt_matches_prefix)
if(NOT qt_matches_prefix)
    message(FATAL_ERROR "ROSE_QT_PREFIX does not contain the Qt Core library linked by the application")
endif()
set(ROSE_PACKAGING_ASSETS "${CMAKE_CURRENT_LIST_DIR}/../packaging/linux")
set(ROSE_PACKAGE_PRIVATE_DIR "lib/rose-agent")
set(ROSE_PACKAGE_DOC_DIR "share/doc/rose-agent")
set(ROSE_PACKAGE_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/packaging")
file(MAKE_DIRECTORY "${ROSE_PACKAGE_WORK_DIR}")

foreach(binary IN ITEMS rose-agent rose-cli)
    if(NOT TARGET "${binary}")
        message(FATAL_ERROR "Linux packaging must be included after both executable targets")
    endif()
    set_target_properties("${binary}" PROPERTIES
        INSTALL_RPATH "$ORIGIN/.." INSTALL_RPATH_USE_LINK_PATH FALSE)
    install(TARGETS "${binary}" RUNTIME DESTINATION "${ROSE_PACKAGE_PRIVATE_DIR}/bin")
    set(ROSE_LAUNCHER_BINARY "${binary}")
    configure_file("${ROSE_PACKAGING_ASSETS}/launcher.in"
        "${ROSE_PACKAGE_WORK_DIR}/${binary}" @ONLY)
    install(PROGRAMS "${ROSE_PACKAGE_WORK_DIR}/${binary}" DESTINATION bin)
endforeach()
install(FILES "${ROSE_PACKAGING_ASSETS}/qt.conf" DESTINATION "${ROSE_PACKAGE_PRIVATE_DIR}/bin")
install(FILES "${ROSE_PACKAGING_ASSETS}/rose-agent.desktop" DESTINATION share/applications)
install(FILES "${ROSE_PACKAGING_ASSETS}/rose-agent.svg" DESTINATION share/icons/hicolor/scalable/apps)
# These are required release inputs: no development plans, test fixtures,
# credentials, user settings or user model data are copied into the package.
install(FILES
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/README.md"
    "${PROJECT_SOURCE_DIR}/CHANGELOG.md"
    "${PROJECT_SOURCE_DIR}/ROADMAP.md"
    DESTINATION "${ROSE_PACKAGE_DOC_DIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/releases" DESTINATION "${ROSE_PACKAGE_DOC_DIR}/docs")
install(DIRECTORY
    "${PROJECT_SOURCE_DIR}/docs/usage"
    "${PROJECT_SOURCE_DIR}/docs/format"
    "${PROJECT_SOURCE_DIR}/docs/compatibility"
    DESTINATION "${ROSE_PACKAGE_DOC_DIR}/docs")
configure_file("${ROSE_PACKAGING_ASSETS}/QtRuntimeNotice.txt.in"
    "${ROSE_PACKAGE_WORK_DIR}/QtRuntimeNotice.txt" @ONLY)
install(FILES "${ROSE_PACKAGE_WORK_DIR}/QtRuntimeNotice.txt" DESTINATION "${ROSE_PACKAGE_DOC_DIR}")
set(ROSE_PACKAGE_GUI_EXECUTABLE "$<TARGET_FILE:rose-agent>")
set(ROSE_PACKAGE_CLI_EXECUTABLE "$<TARGET_FILE:rose-cli>")
configure_file("${ROSE_PACKAGING_ASSETS}/DeployQt.cmake.in"
    "${ROSE_PACKAGE_WORK_DIR}/DeployQt.configured.cmake" @ONLY)
file(GENERATE OUTPUT "${ROSE_PACKAGE_WORK_DIR}/DeployQt-$<CONFIG>.cmake"
    INPUT "${ROSE_PACKAGE_WORK_DIR}/DeployQt.configured.cmake")
install(SCRIPT "${ROSE_PACKAGE_WORK_DIR}/DeployQt-$<CONFIG>.cmake")

set(CPACK_GENERATOR "DEB;RPM;TGZ")
set(CPACK_PACKAGE_NAME "rose-agent")
set(CPACK_PACKAGE_VENDOR "Rose Agent contributors")
set(CPACK_PACKAGE_CONTACT "Rose Agent maintainers <alertxsto@users.noreply.github.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_VERBATIM_VARIABLES TRUE)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Rational Rose model editor with GUI, CLI and AI-assisted review")
set(CPACK_PACKAGE_DESCRIPTION "Rose Agent edits Rational Rose model workspaces, including controlled units, with a Qt desktop interface and command-line interface. AI-assisted changes use explicit review and workspace history.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/alertxsto/rose-agent")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "rose-agent-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_PACKAGE_RELOCATABLE FALSE)
set(CPACK_STRIP_FILES FALSE)
set(CPACK_DEBIAN_PACKAGE_NAME "rose-agent")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_SECTION "non-free/devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS TRUE)
# OpenSSL and xcb-cursor may be loaded dynamically rather than appear in ELF
# NEEDED entries. All other system dependencies are discovered from the ELF files.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.35), libssl3, libxcb-cursor0")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ca-certificates, fonts-dejavu-core")
set(CPACK_RPM_PACKAGE_NAME "rose-agent")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_LICENSE "LicenseRef-Proprietary")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
set(CPACK_RPM_PACKAGE_AUTOREQPROV TRUE)
# Private Qt libraries must not advertise distro-wide Qt capabilities, nor
# become dependencies on a second system Qt. System ELF dependencies remain automatic.
set(CPACK_RPM_PROVIDES_EXCLUDE_FROM "^/usr/lib/rose-agent/.*$")
file(GLOB qt_sdk_libraries "${ROSE_QT_PREFIX}/lib/lib*.so*")
set(private_requires_patterns "^libQt6.*")
foreach(library IN LISTS qt_sdk_libraries)
    get_filename_component(library_name "${library}" NAME)
    string(REGEX REPLACE "\\.so.*$" "" library_stem "${library_name}")
    list(APPEND private_requires_patterns "^${library_stem}\\.so")
endforeach()
list(REMOVE_DUPLICATES private_requires_patterns)
string(JOIN "|" CPACK_RPM_REQUIRES_EXCLUDE ${private_requires_patterns})
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.35, libssl.so.3()(64bit), libcrypto.so.3()(64bit), libxcb-cursor.so.0()(64bit)")
else()
    set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.35, libssl.so.3, libcrypto.so.3, libxcb-cursor.so.0")
endif()
set(CPACK_RPM_PACKAGE_RECOMMENDS "ca-certificates, dejavu-sans-fonts")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/share/applications" "/usr/share/icons" "/usr/share/icons/hicolor"
    "/usr/share/icons/hicolor/scalable" "/usr/share/icons/hicolor/scalable/apps")
configure_file("${ROSE_PACKAGING_ASSETS}/CPackOptions.cmake.in"
    "${ROSE_PACKAGE_WORK_DIR}/CPackOptions.cmake" @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${ROSE_PACKAGE_WORK_DIR}/CPackOptions.cmake")
include(CPack)
