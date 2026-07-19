# Bridge codecs + sync engine, compiled unchanged from the host tree
# (../../bridge), plus the on-device DAV transport (dav_esp.c). The host build
# uses dav.c (curl) instead; everything else is shared, one source of truth.
set(BRIDGE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../bridge")

idf_component_register(
    SRCS
        "dav_esp.c"
        "${BRIDGE_DIR}/pdb.c"
        "${BRIDGE_DIR}/datebook.c"
        "${BRIDGE_DIR}/address.c"
        "${BRIDGE_DIR}/todo.c"
        "${BRIDGE_DIR}/ical.c"
        "${BRIDGE_DIR}/vcard.c"
        "${BRIDGE_DIR}/tz.c"
        "${BRIDGE_DIR}/charset.c"
        "${BRIDGE_DIR}/appinfo.c"
        "${BRIDGE_DIR}/dav_xml.c"
        "${BRIDGE_DIR}/sync.c"
        "${BRIDGE_DIR}/find.c"
        "${BRIDGE_DIR}/calc.c"
        "${BRIDGE_DIR}/config.c"
        "${BRIDGE_DIR}/rss.c"
        "${BRIDGE_DIR}/news.c"
        "${BRIDGE_DIR}/feeds.c"
    INCLUDE_DIRS
        "."
        "${BRIDGE_DIR}"
    REQUIRES
        esp_http_client
        mbedtls
)

# ESP_PLATFORM is predefined by IDF for every component; sync.c/keepFromServer
# key their no-PSRAM sizing off it (smaller arenas + fetch buffers).
