// Contact shadows: where g_CameraParameter holds the three matrices the pass reads, in the register units the shaders address it with.
//
// The values come from the reflected layout of the engine's own light shader (D3D_SVC_MATRIX_ROWS, one register per matrix row):
//   m_ViewMatrix                +0x000  3x4   registers 0..2    (the view matrix is three rows, the fourth is not there)
//   m_InverseViewMatrix         +0x030  3x4   registers 3..5
//   m_ViewProjectionMatrix      +0x060  4x4   registers 6..9
//   m_InverseViewProjectionMatrix +0x0A0 4x4  registers 10..13
//   m_InverseProjectionMatrix   +0x0E0  4x4   registers 14..17
//   m_ProjectionMatrix          +0x120  4x4   registers 18..21
// The same table is included by ContactShadows.hlsl (which addresses the buffer) and by ContactShadowsPass.cpp (which copies it and
// checks it), so the two cannot drift apart, and by the host test that builds a camera buffer of this shape.
#pragma once

#define CS_CAMERA_VIEW_OFFSET 0x000
#define CS_CAMERA_INVERSE_VIEW_OFFSET 0x030
#define CS_CAMERA_VIEW_PROJECTION_OFFSET 0x060
#define CS_CAMERA_INVERSE_VIEW_PROJECTION_OFFSET 0x0A0
#define CS_CAMERA_INVERSE_PROJECTION_OFFSET 0x0E0
#define CS_CAMERA_PROJECTION_OFFSET 0x120

#define CS_CAMERA_VIEW_REGISTER 0
#define CS_CAMERA_INVERSE_VIEW_REGISTER 3
#define CS_CAMERA_VIEW_PROJECTION_REGISTER 6
#define CS_CAMERA_INVERSE_VIEW_PROJECTION_REGISTER 10
#define CS_CAMERA_INVERSE_PROJECTION_REGISTER 14
#define CS_CAMERA_PROJECTION_REGISTER 18

#define CS_CAMERA_VIEW_ROWS 3
#define CS_CAMERA_BYTES 0x160        // through the projection: what the shaders read
#define CS_CAMERA_CHECK_BYTES 0x160  // and what the one-time layout check reads
