; Forward-only exports: undocumented/opaque dxgi.dll entry points we never
; touch, forwarded to the real System32 dxgi.dll with registers untouched.
; g_dxgiForwards is filled by dxgi_proxy_init() before any of these can run.
; Slot order must match kForwardNames in dxgi_proxy.cpp.
;
; stub_null_forward: returned for any slot the real dxgi.dll did not export.
; Returns E_NOTIMPL (0x80004001) through rax, leaves the stack intact.
; Callers that cannot tolerate E_NOTIMPL should check before calling, but in
; practice every caller of these undocumented exports either ignores the return
; value or treats any failing HRESULT gracefully -- and a clean E_NOTIMPL is far
; better than an ACCESS_VIOLATION from jumping through a null pointer.

EXTERN g_dxgiForwards:QWORD

.CODE

stub_null_forward PROC
    mov  eax, 80004001h     ; E_NOTIMPL
    ret
stub_null_forward ENDP

stub_ApplyCompatResolutionQuirking PROC
    mov  rax, qword ptr [g_dxgiForwards + 0*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_ApplyCompatResolutionQuirking ENDP

stub_CompatString PROC
    mov  rax, qword ptr [g_dxgiForwards + 1*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_CompatString ENDP

stub_CompatValue PROC
    mov  rax, qword ptr [g_dxgiForwards + 2*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_CompatValue ENDP

stub_DXGID3D10CreateDevice PROC
    mov  rax, qword ptr [g_dxgiForwards + 3*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGID3D10CreateDevice ENDP

stub_DXGID3D10CreateLayeredDevice PROC
    mov  rax, qword ptr [g_dxgiForwards + 4*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGID3D10CreateLayeredDevice ENDP

stub_DXGID3D10ETWRundown PROC
    mov  rax, qword ptr [g_dxgiForwards + 5*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGID3D10ETWRundown ENDP

stub_DXGID3D10GetLayeredDeviceSize PROC
    mov  rax, qword ptr [g_dxgiForwards + 6*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGID3D10GetLayeredDeviceSize ENDP

stub_DXGID3D10RegisterLayers PROC
    mov  rax, qword ptr [g_dxgiForwards + 7*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGID3D10RegisterLayers ENDP

stub_DXGIDisableVBlankVirtualization PROC
    mov  rax, qword ptr [g_dxgiForwards + 8*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGIDisableVBlankVirtualization ENDP

stub_DXGIDumpJournal PROC
    mov  rax, qword ptr [g_dxgiForwards + 9*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGIDumpJournal ENDP

stub_DXGIReportAdapterConfiguration PROC
    mov  rax, qword ptr [g_dxgiForwards + 10*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_DXGIReportAdapterConfiguration ENDP

stub_PIXBeginCapture PROC
    mov  rax, qword ptr [g_dxgiForwards + 11*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_PIXBeginCapture ENDP

stub_PIXEndCapture PROC
    mov  rax, qword ptr [g_dxgiForwards + 12*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_PIXEndCapture ENDP

stub_PIXGetCaptureState PROC
    mov  rax, qword ptr [g_dxgiForwards + 13*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_PIXGetCaptureState ENDP

stub_SetAppCompatStringPointer PROC
    mov  rax, qword ptr [g_dxgiForwards + 14*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_SetAppCompatStringPointer ENDP

stub_UpdateHMDEmulationStatus PROC
    mov  rax, qword ptr [g_dxgiForwards + 15*8]
    test rax, rax
    jz   stub_null_forward
    jmp  rax
stub_UpdateHMDEmulationStatus ENDP

END
