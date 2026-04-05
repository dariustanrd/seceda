seceda_add_edge_library(
    seceda_edge_local_models
    SOURCES
        llama_runtime.cpp
        local_execution_modes.cpp
        local_engine_resolve.cpp
        sidecar_local_engine_adapter.cpp
        stub_local_engine_adapter.cpp
        local_engine_registry.cpp
    EXTRA_LIBS
        seceda_edge_cloud_bridge
)

seceda_link_llama(seceda_edge_local_models)
