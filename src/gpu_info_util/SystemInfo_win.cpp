//
// Copyright 2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// SystemInfo_win.cpp: implementation of the Windows-specific parts of SystemInfo.h

#include "gpu_info_util/SystemInfo_internal.h"

#include "common/debug.h"
#include "common/string_utils.h"

// Windows.h needs to be included first
#include <windows.h>

#include <d3d10.h>
#include <dxgi.h>

#include <array>
#include <sstream>

namespace angle
{

namespace
{

bool GetDevicesFromDXGI(std::vector<GPUDeviceInfo> *devices)
{
    IDXGIFactory *factory;
    if (!SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&factory))))
    {
        return false;
    }

    UINT i                = 0;
    IDXGIAdapter *adapter = nullptr;
    while (factory->EnumAdapters(i++, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);

        LARGE_INTEGER umdVersion;
        if (adapter->CheckInterfaceSupport(__uuidof(ID3D10Device), &umdVersion) ==
            DXGI_ERROR_UNSUPPORTED)
        {
            adapter->Release();
            continue;
        }

        // The UMD driver version here is the same as in the registry except for the last number.
        uint64_t intVersion = umdVersion.QuadPart;
        std::ostringstream o;

        const uint64_t kMask = 0xFF;
        o << ((intVersion >> 48) & kMask) << ".";
        o << ((intVersion >> 32) & kMask) << ".";
        o << ((intVersion >> 16) & kMask) << ".";
        o << (intVersion & kMask);

        GPUDeviceInfo device;
        device.vendorId      = desc.VendorId;
        device.deviceId      = desc.DeviceId;
        device.driverVersion = o.str();

        devices->push_back(device);

        adapter->Release();
    }

    factory->Release();

    return (i > 0);
}

}  // anonymous namespace

bool GetSystemInfo(SystemInfo *info)
{
    if (!GetDevicesFromDXGI(&info->gpus))
    {
        return false;
    }

    if (info->gpus.size() == 0)
    {
        return false;
    }

    // Call GetDualGPUInfo to populate activeGPUIndex, isOptimus, and isAMDSwitchable.
    GetDualGPUInfo(info);

    // Override activeGPUIndex. The first index returned by EnumAdapters is the active GPU. We
    // can override the heuristic to find the active GPU
    info->activeGPUIndex = 0;

    // Override isOptimus. nvd3d9wrap.dll is loaded into all processes when Optimus is enabled.
    HMODULE nvd3d9wrap = GetModuleHandleW(L"nvd3d9wrap.dll");
    info->isOptimus    = nvd3d9wrap != nullptr;

    return true;
}

}  // namespace angle
