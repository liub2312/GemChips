cmake_minimum_required(VERSION 3.14)
project(WeixinMail VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── 依赖：libcurl ────────────────────────────────────────────────────────────
find_package(CURL REQUIRED)

# ── 共享库目标：libweixinmail.so ─────────────────────────────────────────────
add_library(weixinmail SHARED
    src/CWeixinMail.cpp
)

target_include_directories(weixinmail
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)

target_link_libraries(weixinmail
    PRIVATE
        CURL::libcurl
        Threads::Threads
)

set_target_properties(weixinmail PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION 1
    OUTPUT_NAME weixinmail
)

# ── 演示可执行文件 ─────────────────────────────────────────────────────────────
add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE weixinmail)

# ── 安装规则 ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS weixinmail
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ── 编译选项 ──────────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(weixinmail PRIVATE -Wall -Wextra -O2)
    target_compile_options(demo       PRIVATE -Wall -Wextra -O2)
endif()
