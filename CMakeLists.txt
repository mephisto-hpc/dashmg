CMAKE_MINIMUM_REQUIRED(VERSION 3.6.0)

PROJECT(dashmg)

SET(CMAKE_CXX_STANDARD 14)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

FIND_PACKAGE(dash-mpi REQUIRED)

ADD_EXECUTABLE(
    multigrid3d
    "multigrid3d.cpp")
TARGET_LINK_LIBRARIES(
    multigrid3d
    PUBLIC "${DASH_LIBRARIES}")
