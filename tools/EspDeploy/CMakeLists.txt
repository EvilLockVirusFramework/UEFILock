cmake_minimum_required(VERSION 3.20)

project(EspDeploy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EspDeploy
  EspDeploy.cpp
  data.cpp
  data.h
  UEFILocker.cpp
  UEFILocker.h
)

target_compile_definitions(EspDeploy PRIVATE UNICODE _UNICODE)
target_compile_options(EspDeploy PRIVATE /W4 /permissive- /utf-8)

add_executable(EspDeployExample
  example.cpp
  data.cpp
  data.h
  UEFILocker.cpp
  UEFILocker.h
)

target_compile_definitions(EspDeployExample PRIVATE UNICODE _UNICODE)
target_compile_options(EspDeployExample PRIVATE /W4 /permissive- /utf-8)
