idf_component_register(
    SRCS "sc_webfs.c" "web_assets.c"
    INCLUDE_DIRS "include"
    REQUIRES littlefs
)
