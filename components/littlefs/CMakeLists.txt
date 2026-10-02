idf_component_register(
    SRCS
        "sc_littlefs.c"
        "littlefs/lfs.c"
        "littlefs/lfs_util.c"
    INCLUDE_DIRS
        "include"
        "littlefs"
    REQUIRES spi_flash
)
