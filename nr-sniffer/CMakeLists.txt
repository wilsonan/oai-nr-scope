# nr-sniffer: passive 5G SA telemetry tool built on the OAI nrUE receive chain.
#
# This directory is linked into an OAI 2026.w41 source tree as ${OPENAIR_DIR}/nr-sniffer
# by scripts/setup_oai.sh, and OAI's root CMakeLists.txt adds it with add_subdirectory()
# (see oai-patches/). It reuses the variables and libraries defined by OAI's root CMake.

add_executable(nr-sniffer
  ${rrc_h}
  ${s1ap_h}
  ${OPENAIR_DIR}/executables/nr-uesoftmodem.c
  ${OPENAIR_DIR}/executables/position_interface.c
  ${OPENAIR_DIR}/executables/nr-ue.c
  ${OPENAIR_DIR}/executables/nr-ue-ru.c
  ${OPENAIR1_DIR}/PHY/TOOLS/phy_scope_interface.c
  ${NFAPI_USER_DIR}/nfapi.c
  ${PHY_INTERFACE_DIR}/queue_t.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nr_sniffer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nr_sniffer_csv.c
)

target_include_directories(nr-sniffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(nr-sniffer PRIVATE
  -Wl,--start-group
  nr_rrc SECURITY UTIL SCHED_NR_UE_LIB
  PHY_NR_COMMON PHY_NR_UE NR_L2_UE MAC_NR_COMMON nfapi_nr_lib
  ITTI SIMU radio_common
  time_management softmodem_common
  -Wl,--end-group dl)

target_link_libraries(nr-sniffer PRIVATE pthread m CONFIG_LIB rt nr_ue_phy_meas)
target_link_libraries(nr-sniffer PRIVATE ${T_LIB})
target_link_libraries(nr-sniffer PRIVATE nr_nas lib_uicc usim_lib SECURITY)
target_link_libraries(nr-sniffer PRIVATE asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs)
target_link_libraries(nr-sniffer PRIVATE
                      asn1_lte_rrc asn1_nr_rrc asn1_s1ap asn1_ngap asn1_m2ap asn1_m3ap asn1_x2ap asn1_f1ap asn1_lpp)

if (${T_TRACER})
  add_dependencies(nr-sniffer generate_T)
endif()

# Build the sniffer together with the radio/runtime modules it loads.
add_custom_target(nr-sniffer-all DEPENDS nr-sniffer params_libconfig coding rfsimulator dfts)
