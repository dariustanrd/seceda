seceda_add_edge_library(
    seceda_edge_config_catalog
    SOURCES
        simple_toml.cpp
        config_catalog.cpp
    EXTRA_INCLUDES
        ${tomlplusplus_SOURCE_DIR}
)
