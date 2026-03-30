if(BUILD_TESTING)
    seceda_add_edge_executable(
        seceda_edge_router_test
        SOURCES
            router_heuristic_test.cpp
        EXTRA_LIBS
            seceda_edge_router
    )
    add_test(NAME seceda_edge_router_test COMMAND seceda_edge_router_test)

    seceda_add_edge_executable(
        seceda_edge_request_executor_test
        SOURCES
            request_executor_test.cpp
        EXTRA_LIBS
            seceda_edge_runtime
            seceda_edge_telemetry
    )
    add_test(NAME seceda_edge_request_executor_test COMMAND seceda_edge_request_executor_test)

    seceda_add_edge_executable(
        seceda_edge_daemon_test
        SOURCES
            edge_daemon_test.cpp
        EXTRA_LIBS
            seceda_edge_runtime
    )
    add_test(NAME seceda_edge_daemon_test COMMAND seceda_edge_daemon_test)

    seceda_add_edge_executable(
        seceda_edge_runtime_config_test
        SOURCES
            runtime_config_test.cpp
        EXTRA_LIBS
            seceda_edge_runtime
    )
    add_test(NAME seceda_edge_runtime_config_test COMMAND seceda_edge_runtime_config_test)

    seceda_add_edge_executable(
        seceda_edge_cloud_client_test
        SOURCES
            cloud_client_test.cpp
        EXTRA_LIBS
            seceda_edge_cloud_bridge
            seceda_edge_httplib
    )
    add_test(NAME seceda_edge_cloud_client_test COMMAND seceda_edge_cloud_client_test)
endif()

if(SECEDA_BUILD_LLAMA_CPP)
    seceda_add_edge_executable(
        seceda_edge_llama_smoke
        SOURCES
            llama_runtime_smoke.cpp
        EXTRA_LIBS
            seceda_edge_local_models
    )
endif()
