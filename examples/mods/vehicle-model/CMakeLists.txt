cmake_minimum_required(VERSION 3.25)
project(vehicle_model C)
include(../../../native/mod-sdk/cmake/HellForgeMod.cmake)
hellforge_add_mod(vehicle_model src/vehicle_model.c)
