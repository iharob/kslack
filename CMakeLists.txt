cmake_minimum_required(VERSION 3.20)

project(kslack VERSION 1.4.0 LANGUAGES CXX) # x-release-please-version

set(QT_MIN_VERSION "6.6.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMInstallIcons)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Core
    Widgets
    WebEngineWidgets
)

find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons
    I18n
    XmlGui
    Crash
    DBusAddons
    Notifications
    StatusNotifierItem
    ColorScheme
    ConfigWidgets
)

add_subdirectory(src)

ecm_install_icons(ICONS
    icons/sc-apps-kslack.svg
    icons/sc-status-kslack-tray.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}
)

install(FILES org.kde.kslack.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES kslack.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
