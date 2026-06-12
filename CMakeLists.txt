cmake_minimum_required(VERSION 3.20)

project(GemChips LANGUAGES CXX)

add_library(CESGatewayServer
    src/CESGatewayServer.cpp
)

target_include_directories(CESGatewayServer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(CESGatewayServer PUBLIC cxx_std_20)

if (WIN32)
    target_link_libraries(CESGatewayServer PRIVATE httpapi)
endif()

