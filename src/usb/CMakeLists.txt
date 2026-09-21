cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK to the VitaSDK prefix")
  endif()
endif()
project(vcm_mtp_bridge C)
include("${VITASDK}/share/vita.cmake" REQUIRED)
option(VCM_ACTIVE_MTP "Intercept MTP commands after a valid VCM catalogue is available" OFF)

add_executable(vcm_mtp_bridge probe.c)
target_compile_options(vcm_mtp_bridge PRIVATE -Os -Wall -Wextra -Werror)
target_link_libraries(vcm_mtp_bridge taihen_stub SceLibKernel_stub_weak
  SceKernelThreadMgr_stub_weak SceIofilemgr_stub_weak gcc)
if(VCM_ACTIVE_MTP)
  target_compile_definitions(vcm_mtp_bridge PRIVATE VCM_ENABLE_ACTIVE_MTP=1)
  target_sources(vcm_mtp_bridge PRIVATE catalog_client.c mtpif_adapter.c
    ../vita/mtp_catalog.c ../vita/mtp_wire.c ../vita/mtp_server.c ../vita/mtp_engine.c)
  target_link_libraries(vcm_mtp_bridge SceSysmem_stub_weak SceNet_stub_weak
    SceSysmodule_stub_weak SceMtpIfDriver_stub_weak)
endif()
set_target_properties(vcm_mtp_bridge PROPERTIES LINK_FLAGS "-nostdlib")
vita_create_self(vcm_mtp_bridge.suprx vcm_mtp_bridge UNSAFE CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml)
