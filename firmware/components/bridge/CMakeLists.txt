# The portable modules, compiled unchanged from the host tree (../../bridge):
# the PDB codec and category table (Memos), Find, the calculator, config.ini,
# the RSS reader, the weather and location parsers, the lock screen's rules,
# and the transport breaker; plus the device's HTTPS transport (dav_esp.c).
# The iCloud sync engine and the Date Book / Address / To Do codecs were
# archived on 2026-11-07 (git tag archive/pim-caldav).
set(BRIDGE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../bridge")

idf_component_register(
    SRCS
        "dav_esp.c"
        "${BRIDGE_DIR}/pdb.c"
        "${BRIDGE_DIR}/appinfo.c"
        "${BRIDGE_DIR}/dav_break.c"
        "${BRIDGE_DIR}/find.c"
        "${BRIDGE_DIR}/calc.c"
        "${BRIDGE_DIR}/config.c"
        "${BRIDGE_DIR}/rss.c"
        "${BRIDGE_DIR}/news.c"
        "${BRIDGE_DIR}/wxfetch.c"
        "${BRIDGE_DIR}/geoip.c"
        "${BRIDGE_DIR}/feeds.c"
        "${BRIDGE_DIR}/dash.c"
    INCLUDE_DIRS
        "."
        "${BRIDGE_DIR}"
    REQUIRES
        esp_http_client
        mbedtls
)

