seceda_add_edge_library(
    seceda_edge_local_models
    SOURCES
        llama_runtime.cpp
)

seceda_link_llama(seceda_edge_local_models)
