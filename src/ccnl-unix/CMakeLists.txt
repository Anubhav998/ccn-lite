cmake_minimum_required(VERSION 2.8)
project(ccnl-unix)
 
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include_directories(include ../ccnl-addons/include ../ccnl-pkt/include ../ccnl-fwd/include ../ccnl-core/include)
 
file(GLOB SOURCES "src/*.c")
file(GLOB HEADERS "include/*.h")

add_library(${PROJECT_NAME} STATIC ${SOURCES} ${HEADERS})
