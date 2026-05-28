qt_add_resources(ICON_RESOURCES ../icons/icons.qrc)

add_executable(kslack
    main.cpp
    mainwindow.cpp
    mainwindow.h
    titlebarcolor.cpp
    titlebarcolor.h
    ${ICON_RESOURCES}
)

target_link_libraries(kslack PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
    Qt6::WebEngineWidgets
    KF6::CoreAddons
    KF6::I18n
    KF6::XmlGui
    KF6::Crash
    KF6::DBusAddons
    KF6::Notifications
    KF6::StatusNotifierItem
    KF6::ColorScheme
    KF6::ConfigWidgets
)

install(TARGETS kslack DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES kslackui.rc DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/kslack)
