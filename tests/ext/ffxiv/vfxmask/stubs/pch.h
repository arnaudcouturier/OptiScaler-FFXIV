#pragma once
// Stand-in for the product pch.h in the standalone VFX mask probe tests: the production files built here (the mask texture
// module) need Windows, the D3D11 headers and the standard library, and nothing else of the product.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
