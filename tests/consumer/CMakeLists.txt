cmake_minimum_required(VERSION 3.20)
project(MiraInstalledConsumer LANGUAGES CXX)

find_package(Mira 0.1 CONFIG REQUIRED)

add_executable(mira_installed_consumer main.cpp)
target_link_libraries(mira_installed_consumer PRIVATE Mira::core Mira::simulator_adapter)
target_compile_features(mira_installed_consumer PRIVATE cxx_std_20)
