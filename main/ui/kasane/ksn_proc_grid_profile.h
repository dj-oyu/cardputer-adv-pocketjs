#ifndef KSN_PROC_GRID_PROFILE_H
#define KSN_PROC_GRID_PROFILE_H

/* Generated from repeated same-binary ESP32-S3 measurements.
 * Entries rank only candidates already proved legal at bind. */
/* Initial one-term calibration: binary SHA256 b58c8c7d0eea826e547c18dae72e52a7712f49aa4fffd7f3cdb3b03253d4f9ed;
 * ELF ff4e63339ac1c041e054372143c5cb2a8b602e24fb90d035d059a6eb3c55afce.
 * Two-term measurements are generated separately from GRID LAB records.
 * CPU 240000000 Hz; IDF v6.0.1; chip v0.2; target esp32s3; opt size. */
#define KSN_GRID_PROFILE_CPU_MHZ 240u
#define KSN_GRID_PROFILE_IDF_MAJOR 6u
#define KSN_GRID_PROFILE_IDF_MINOR 0u
#define KSN_GRID_PROFILE_IDF_PATCH 1u
#define KSN_GRID_PROFILE_OPT_SIZE 1u
#define KSN_GRID_PROFILE_BACKEND_REV 1u
static const ksn_grid_profile_entry ksn_grid_profile[] = {
    {0, KSN_GRID_PIE_LOAD_AUTO},
    /* GRID_TIME 16x12 taps=2x2: gather=80032us, affine=86619us, fused=61546us; gain=23.1% */
    {UINT64_C(0x0a123287ac2f3390), KSN_GRID_PIE_LOAD_FUSED},
    /* GRID_ALIGNED_TIME 16x12 taps=2x2: gather=80073us, fused=61590us; gain=23.1% */
    {UINT64_C(0x51df87624d6af998), KSN_GRID_PIE_LOAD_FUSED},
    /* GRID_TIME 8x1 taps=2x2: gather=5489us, affine=5761us, fused=4768us; gain=13.1% */
    {UINT64_C(0x5ed3e107219e6365), KSN_GRID_PIE_LOAD_FUSED},
    /* GRID_TIME 9x1 taps=2x2: gather=8131us, affine=8417us, fused=7417us; gain=8.8% */
    {UINT64_C(0x7c2674467e77c184), KSN_GRID_PIE_LOAD_FUSED},
    /* GRID_BROADCAST_TIME 16x12 taps=3x3: gather=252576us, affine=182162us; gain=27.9% */
    {UINT64_C(0x805f0efc3b1595d8), KSN_GRID_PIE_LOAD_AFFINE},
    /* GRID_DYNAMIC_TIME 8x1 taps=2x2: gather=7114us, affine=7189us; gain=0.0% */
    {UINT64_C(0x90f9dd8e1e2bdaac), KSN_GRID_PIE_LOAD_GATHER},
    /* GRID_DYNAMIC_TIME 16x12 taps=2x2: gather=124079us, affine=125333us; gain=0.0% */
    {UINT64_C(0x96b4bff5b8b94419), KSN_GRID_PIE_LOAD_GATHER},
    /* GRID_DYNAMIC_TIME 8x5 taps=2x2: gather=27705us, affine=27989us; gain=0.0% */
    {UINT64_C(0x9b5cf6f9925ce428), KSN_GRID_PIE_LOAD_GATHER},
    /* GRID_DYNAMIC_TIME 9x1 taps=2x2: gather=10022us, affine=10102us; gain=0.0% */
    {UINT64_C(0xd40edbfd9bd137ad), KSN_GRID_PIE_LOAD_GATHER},
    /* GRID_DYNAMIC_TIME 16x1 taps=2x2: gather=12135us, affine=12265us; gain=0.0% */
    {UINT64_C(0xdd5a23d7744ba6b4), KSN_GRID_PIE_LOAD_GATHER},
    /* GRID_TIME 8x5 taps=2x2: gather=18700us, affine=20084us, fused=14892us; gain=20.4% */
    {UINT64_C(0xf84cb4f253d4f0e1), KSN_GRID_PIE_LOAD_FUSED},
    /* GRID_TIME 16x1 taps=2x2: gather=8672us, affine=9230us, fused=7180us; gain=17.2% */
    {UINT64_C(0xfae22e0f8032fbdd), KSN_GRID_PIE_LOAD_FUSED},
#include "ksn_proc_grid_dual_profile.inc"
};

#endif
