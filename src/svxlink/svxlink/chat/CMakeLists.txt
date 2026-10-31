# The optional service is installed as source; its Python environment and API
# credential are provisioned separately, never by the SvxLink build.
install(PROGRAMS svxlink_chat.py DESTINATION ${SVX_SHARE_INSTALL_DIR}/chat)
install(FILES requirements.txt requirements-wake.txt
        DESTINATION ${SVX_SHARE_INSTALL_DIR}/chat)
install_if_not_exists(${CMAKE_CURRENT_SOURCE_DIR}/chat.json ${SVX_SYSCONF_INSTALL_DIR})
