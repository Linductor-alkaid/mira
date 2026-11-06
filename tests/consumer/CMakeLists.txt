cmake_minimum_required(VERSION 3.20)
project(MiraInstalledConsumer LANGUAGES CXX)

find_package(Mira 0.1 CONFIG REQUIRED)
find_package(kairo 0.6 CONFIG REQUIRED)

add_executable(mira_installed_consumer main.cpp)
target_link_libraries(mira_installed_consumer PRIVATE
    Mira::core Mira::workflow Mira::simulator_adapter Mira::state_store Mira::net_transport
    kairo::kairo)
if(TARGET Mira::mbedtls_transport)
    target_link_libraries(mira_installed_consumer PRIVATE Mira::mbedtls_transport)
    target_compile_definitions(mira_installed_consumer PRIVATE MIRA_CONSUMER_HAS_MBEDTLS)
    # Mira::mbedtls_transport is a shared library; Windows consumers need its
    # DLL next to the executable at runtime (installed packages have no
    # build-tree PATH injection).
    add_custom_command(TARGET mira_installed_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Mira::mbedtls_transport>
                $<TARGET_FILE_DIR:mira_installed_consumer>)
endif()
target_compile_features(mira_installed_consumer PRIVATE cxx_std_20)

# Compile the three composition examples strictly against installed targets.
foreach(host IN ITEMS workflow_runtime context_assembly recovery_planner)
    add_executable(mira_installed_${host}_host_consumer
        "${CMAKE_CURRENT_LIST_DIR}/../../examples/${host}_host_consumer.cpp")
    target_link_libraries(mira_installed_${host}_host_consumer PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter Mira::state_store kairo::kairo)
    target_compile_features(mira_installed_${host}_host_consumer PRIVATE cxx_std_20)
endforeach()
