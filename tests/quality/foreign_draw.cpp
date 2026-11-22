// SPDX-License-Identifier: GPL-3.0-or-later
#include <d3d11.h>

extern "C" __declspec(dllexport) unsigned WINAPI draw_foreign(ID3D11DeviceContext* context) {
    constexpr unsigned draws = 8;
    for (unsigned i = 0; i < draws; ++i) context->Draw(3, 0);
    return draws;
}
