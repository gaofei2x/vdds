# SSAS - Simple Smart Automotive Software
# Copyright (C) 2024 Parai Wang <parai@foxmail.com>


add_executable(VDDSPub publisher.cpp)
target_link_libraries(VDDSPub PRIVATE VDDS)

add_executable(VDDSSub subscriber.cpp)
target_link_libraries(VDDSSub PRIVATE VDDS)
