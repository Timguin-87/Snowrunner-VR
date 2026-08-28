; Forward-only exports: undocumented/opaque dxgi.dll entry points we never
; touch, forwarded to the real System32 dxgi.dll with registers untouched.
; g_dxgiForwards is filled by dxgi_proxy_init() before any of these can run.
; Slot order must match kForwardNames in dxgi_proxy.cpp.

EXTERN g_dxgiForwards:QWORD

.CODE

stub_ApplyCompatResolutionQuirking PROC
    jmp qword ptr [g_dxgiForwards + 0*8]
stub_ApplyCompatResolutionQuirking ENDP

stub_CompatString PROC
    jmp qword ptr [g_dxgiForwards + 1*8]
stub_CompatString ENDP

stub_CompatValue PROC
    jmp qword ptr [g_dxgiForwards + 2*8]
stub_CompatValue ENDP

stub_DXGID3D10CreateDevice PROC
    jmp qword ptr [g_dxgiForwards + 3*8]
stub_DXGID3D10CreateDevice ENDP

stub_DXGID3D10CreateLayeredDevice PROC
    jmp qword ptr [g_dxgiForwards + 4*8]
stub_DXGID3D10CreateLayeredDevice ENDP

stub_DXGID3D10ETWRundown PROC
    jmp qword ptr [g_dxgiForwards + 5*8]
stub_DXGID3D10ETWRundown ENDP

stub_DXGID3D10GetLayeredDeviceSize PROC
    jmp qword ptr [g_dxgiForwards + 6*8]
stub_DXGID3D10GetLayeredDeviceSize ENDP

stub_DXGID3D10RegisterLayers PROC
    jmp qword ptr [g_dxgiForwards + 7*8]
stub_DXGID3D10RegisterLayers ENDP

stub_DXGIDisableVBlankVirtualization PROC
    jmp qword ptr [g_dxgiForwards + 8*8]
stub_DXGIDisableVBlankVirtualization ENDP

stub_DXGIDumpJournal PROC
    jmp qword ptr [g_dxgiForwards + 9*8]
stub_DXGIDumpJournal ENDP

stub_DXGIReportAdapterConfiguration PROC
    jmp qword ptr [g_dxgiForwards + 10*8]
stub_DXGIReportAdapterConfiguration ENDP

stub_PIXBeginCapture PROC
    jmp qword ptr [g_dxgiForwards + 11*8]
stub_PIXBeginCapture ENDP

stub_PIXEndCapture PROC
    jmp qword ptr [g_dxgiForwards + 12*8]
stub_PIXEndCapture ENDP

stub_PIXGetCaptureState PROC
    jmp qword ptr [g_dxgiForwards + 13*8]
stub_PIXGetCaptureState ENDP

stub_SetAppCompatStringPointer PROC
    jmp qword ptr [g_dxgiForwards + 14*8]
stub_SetAppCompatStringPointer ENDP

stub_UpdateHMDEmulationStatus PROC
    jmp qword ptr [g_dxgiForwards + 15*8]
stub_UpdateHMDEmulationStatus ENDP

END
