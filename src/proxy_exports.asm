; Pass-through exports of version.dll and winmm.dll (src\proxy_exports.cpp), for the standalone installed under one
; of those names. Each export jumps to the system DLL's function, which is looked up on its first call.
; Generated from the exports of Windows 11 24H2's System32\version.dll and winmm.dll.

EXTERN glass_proxy_resolve:PROC
EXTERN glass_proxy_targets:QWORD

.code

; r11d: the export's index. Keeps the argument registers, looks the function up and jumps to it.
proxy_resolve_common PROC
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 68h
    movdqu xmmword ptr [rsp + 20h], xmm0
    movdqu xmmword ptr [rsp + 30h], xmm1
    movdqu xmmword ptr [rsp + 40h], xmm2
    movdqu xmmword ptr [rsp + 50h], xmm3
    mov ecx, r11d
    call glass_proxy_resolve
    movdqu xmm0, xmmword ptr [rsp + 20h]
    movdqu xmm1, xmmword ptr [rsp + 30h]
    movdqu xmm2, xmmword ptr [rsp + 40h]
    movdqu xmm3, xmmword ptr [rsp + 50h]
    add rsp, 68h
    pop r9
    pop r8
    pop rdx
    pop rcx
    jmp rax
proxy_resolve_common ENDP

proxy_version_GetFileVersionInfoA PROC
    mov rax, qword ptr [glass_proxy_targets + 0]
    test rax, rax
    jz proxy_version_GetFileVersionInfoA_resolve
    jmp rax
proxy_version_GetFileVersionInfoA_resolve:
    mov r11d, 0
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoA ENDP

proxy_version_GetFileVersionInfoByHandle PROC
    mov rax, qword ptr [glass_proxy_targets + 8]
    test rax, rax
    jz proxy_version_GetFileVersionInfoByHandle_resolve
    jmp rax
proxy_version_GetFileVersionInfoByHandle_resolve:
    mov r11d, 1
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoByHandle ENDP

proxy_version_GetFileVersionInfoExA PROC
    mov rax, qword ptr [glass_proxy_targets + 16]
    test rax, rax
    jz proxy_version_GetFileVersionInfoExA_resolve
    jmp rax
proxy_version_GetFileVersionInfoExA_resolve:
    mov r11d, 2
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoExA ENDP

proxy_version_GetFileVersionInfoExW PROC
    mov rax, qword ptr [glass_proxy_targets + 24]
    test rax, rax
    jz proxy_version_GetFileVersionInfoExW_resolve
    jmp rax
proxy_version_GetFileVersionInfoExW_resolve:
    mov r11d, 3
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoExW ENDP

proxy_version_GetFileVersionInfoSizeA PROC
    mov rax, qword ptr [glass_proxy_targets + 32]
    test rax, rax
    jz proxy_version_GetFileVersionInfoSizeA_resolve
    jmp rax
proxy_version_GetFileVersionInfoSizeA_resolve:
    mov r11d, 4
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoSizeA ENDP

proxy_version_GetFileVersionInfoSizeExA PROC
    mov rax, qword ptr [glass_proxy_targets + 40]
    test rax, rax
    jz proxy_version_GetFileVersionInfoSizeExA_resolve
    jmp rax
proxy_version_GetFileVersionInfoSizeExA_resolve:
    mov r11d, 5
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoSizeExA ENDP

proxy_version_GetFileVersionInfoSizeExW PROC
    mov rax, qword ptr [glass_proxy_targets + 48]
    test rax, rax
    jz proxy_version_GetFileVersionInfoSizeExW_resolve
    jmp rax
proxy_version_GetFileVersionInfoSizeExW_resolve:
    mov r11d, 6
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoSizeExW ENDP

proxy_version_GetFileVersionInfoSizeW PROC
    mov rax, qword ptr [glass_proxy_targets + 56]
    test rax, rax
    jz proxy_version_GetFileVersionInfoSizeW_resolve
    jmp rax
proxy_version_GetFileVersionInfoSizeW_resolve:
    mov r11d, 7
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoSizeW ENDP

proxy_version_GetFileVersionInfoW PROC
    mov rax, qword ptr [glass_proxy_targets + 64]
    test rax, rax
    jz proxy_version_GetFileVersionInfoW_resolve
    jmp rax
proxy_version_GetFileVersionInfoW_resolve:
    mov r11d, 8
    jmp proxy_resolve_common
proxy_version_GetFileVersionInfoW ENDP

proxy_version_VerFindFileA PROC
    mov rax, qword ptr [glass_proxy_targets + 72]
    test rax, rax
    jz proxy_version_VerFindFileA_resolve
    jmp rax
proxy_version_VerFindFileA_resolve:
    mov r11d, 9
    jmp proxy_resolve_common
proxy_version_VerFindFileA ENDP

proxy_version_VerFindFileW PROC
    mov rax, qword ptr [glass_proxy_targets + 80]
    test rax, rax
    jz proxy_version_VerFindFileW_resolve
    jmp rax
proxy_version_VerFindFileW_resolve:
    mov r11d, 10
    jmp proxy_resolve_common
proxy_version_VerFindFileW ENDP

proxy_version_VerInstallFileA PROC
    mov rax, qword ptr [glass_proxy_targets + 88]
    test rax, rax
    jz proxy_version_VerInstallFileA_resolve
    jmp rax
proxy_version_VerInstallFileA_resolve:
    mov r11d, 11
    jmp proxy_resolve_common
proxy_version_VerInstallFileA ENDP

proxy_version_VerInstallFileW PROC
    mov rax, qword ptr [glass_proxy_targets + 96]
    test rax, rax
    jz proxy_version_VerInstallFileW_resolve
    jmp rax
proxy_version_VerInstallFileW_resolve:
    mov r11d, 12
    jmp proxy_resolve_common
proxy_version_VerInstallFileW ENDP

proxy_version_VerLanguageNameA PROC
    mov rax, qword ptr [glass_proxy_targets + 104]
    test rax, rax
    jz proxy_version_VerLanguageNameA_resolve
    jmp rax
proxy_version_VerLanguageNameA_resolve:
    mov r11d, 13
    jmp proxy_resolve_common
proxy_version_VerLanguageNameA ENDP

proxy_version_VerLanguageNameW PROC
    mov rax, qword ptr [glass_proxy_targets + 112]
    test rax, rax
    jz proxy_version_VerLanguageNameW_resolve
    jmp rax
proxy_version_VerLanguageNameW_resolve:
    mov r11d, 14
    jmp proxy_resolve_common
proxy_version_VerLanguageNameW ENDP

proxy_version_VerQueryValueA PROC
    mov rax, qword ptr [glass_proxy_targets + 120]
    test rax, rax
    jz proxy_version_VerQueryValueA_resolve
    jmp rax
proxy_version_VerQueryValueA_resolve:
    mov r11d, 15
    jmp proxy_resolve_common
proxy_version_VerQueryValueA ENDP

proxy_version_VerQueryValueW PROC
    mov rax, qword ptr [glass_proxy_targets + 128]
    test rax, rax
    jz proxy_version_VerQueryValueW_resolve
    jmp rax
proxy_version_VerQueryValueW_resolve:
    mov r11d, 16
    jmp proxy_resolve_common
proxy_version_VerQueryValueW ENDP

proxy_winmm_CloseDriver PROC
    mov rax, qword ptr [glass_proxy_targets + 136]
    test rax, rax
    jz proxy_winmm_CloseDriver_resolve
    jmp rax
proxy_winmm_CloseDriver_resolve:
    mov r11d, 17
    jmp proxy_resolve_common
proxy_winmm_CloseDriver ENDP

proxy_winmm_DefDriverProc PROC
    mov rax, qword ptr [glass_proxy_targets + 144]
    test rax, rax
    jz proxy_winmm_DefDriverProc_resolve
    jmp rax
proxy_winmm_DefDriverProc_resolve:
    mov r11d, 18
    jmp proxy_resolve_common
proxy_winmm_DefDriverProc ENDP

proxy_winmm_DriverCallback PROC
    mov rax, qword ptr [glass_proxy_targets + 152]
    test rax, rax
    jz proxy_winmm_DriverCallback_resolve
    jmp rax
proxy_winmm_DriverCallback_resolve:
    mov r11d, 19
    jmp proxy_resolve_common
proxy_winmm_DriverCallback ENDP

proxy_winmm_DrvGetModuleHandle PROC
    mov rax, qword ptr [glass_proxy_targets + 160]
    test rax, rax
    jz proxy_winmm_DrvGetModuleHandle_resolve
    jmp rax
proxy_winmm_DrvGetModuleHandle_resolve:
    mov r11d, 20
    jmp proxy_resolve_common
proxy_winmm_DrvGetModuleHandle ENDP

proxy_winmm_GetDriverModuleHandle PROC
    mov rax, qword ptr [glass_proxy_targets + 168]
    test rax, rax
    jz proxy_winmm_GetDriverModuleHandle_resolve
    jmp rax
proxy_winmm_GetDriverModuleHandle_resolve:
    mov r11d, 21
    jmp proxy_resolve_common
proxy_winmm_GetDriverModuleHandle ENDP

proxy_winmm_OpenDriver PROC
    mov rax, qword ptr [glass_proxy_targets + 176]
    test rax, rax
    jz proxy_winmm_OpenDriver_resolve
    jmp rax
proxy_winmm_OpenDriver_resolve:
    mov r11d, 22
    jmp proxy_resolve_common
proxy_winmm_OpenDriver ENDP

proxy_winmm_PlaySound PROC
    mov rax, qword ptr [glass_proxy_targets + 184]
    test rax, rax
    jz proxy_winmm_PlaySound_resolve
    jmp rax
proxy_winmm_PlaySound_resolve:
    mov r11d, 23
    jmp proxy_resolve_common
proxy_winmm_PlaySound ENDP

proxy_winmm_PlaySoundA PROC
    mov rax, qword ptr [glass_proxy_targets + 192]
    test rax, rax
    jz proxy_winmm_PlaySoundA_resolve
    jmp rax
proxy_winmm_PlaySoundA_resolve:
    mov r11d, 24
    jmp proxy_resolve_common
proxy_winmm_PlaySoundA ENDP

proxy_winmm_PlaySoundW PROC
    mov rax, qword ptr [glass_proxy_targets + 200]
    test rax, rax
    jz proxy_winmm_PlaySoundW_resolve
    jmp rax
proxy_winmm_PlaySoundW_resolve:
    mov r11d, 25
    jmp proxy_resolve_common
proxy_winmm_PlaySoundW ENDP

proxy_winmm_SendDriverMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 208]
    test rax, rax
    jz proxy_winmm_SendDriverMessage_resolve
    jmp rax
proxy_winmm_SendDriverMessage_resolve:
    mov r11d, 26
    jmp proxy_resolve_common
proxy_winmm_SendDriverMessage ENDP

proxy_winmm_WOWAppExit PROC
    mov rax, qword ptr [glass_proxy_targets + 216]
    test rax, rax
    jz proxy_winmm_WOWAppExit_resolve
    jmp rax
proxy_winmm_WOWAppExit_resolve:
    mov r11d, 27
    jmp proxy_resolve_common
proxy_winmm_WOWAppExit ENDP

proxy_winmm_auxGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 224]
    test rax, rax
    jz proxy_winmm_auxGetDevCapsA_resolve
    jmp rax
proxy_winmm_auxGetDevCapsA_resolve:
    mov r11d, 28
    jmp proxy_resolve_common
proxy_winmm_auxGetDevCapsA ENDP

proxy_winmm_auxGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 232]
    test rax, rax
    jz proxy_winmm_auxGetDevCapsW_resolve
    jmp rax
proxy_winmm_auxGetDevCapsW_resolve:
    mov r11d, 29
    jmp proxy_resolve_common
proxy_winmm_auxGetDevCapsW ENDP

proxy_winmm_auxGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 240]
    test rax, rax
    jz proxy_winmm_auxGetNumDevs_resolve
    jmp rax
proxy_winmm_auxGetNumDevs_resolve:
    mov r11d, 30
    jmp proxy_resolve_common
proxy_winmm_auxGetNumDevs ENDP

proxy_winmm_auxGetVolume PROC
    mov rax, qword ptr [glass_proxy_targets + 248]
    test rax, rax
    jz proxy_winmm_auxGetVolume_resolve
    jmp rax
proxy_winmm_auxGetVolume_resolve:
    mov r11d, 31
    jmp proxy_resolve_common
proxy_winmm_auxGetVolume ENDP

proxy_winmm_auxOutMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 256]
    test rax, rax
    jz proxy_winmm_auxOutMessage_resolve
    jmp rax
proxy_winmm_auxOutMessage_resolve:
    mov r11d, 32
    jmp proxy_resolve_common
proxy_winmm_auxOutMessage ENDP

proxy_winmm_auxSetVolume PROC
    mov rax, qword ptr [glass_proxy_targets + 264]
    test rax, rax
    jz proxy_winmm_auxSetVolume_resolve
    jmp rax
proxy_winmm_auxSetVolume_resolve:
    mov r11d, 33
    jmp proxy_resolve_common
proxy_winmm_auxSetVolume ENDP

proxy_winmm_joyConfigChanged PROC
    mov rax, qword ptr [glass_proxy_targets + 272]
    test rax, rax
    jz proxy_winmm_joyConfigChanged_resolve
    jmp rax
proxy_winmm_joyConfigChanged_resolve:
    mov r11d, 34
    jmp proxy_resolve_common
proxy_winmm_joyConfigChanged ENDP

proxy_winmm_joyGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 280]
    test rax, rax
    jz proxy_winmm_joyGetDevCapsA_resolve
    jmp rax
proxy_winmm_joyGetDevCapsA_resolve:
    mov r11d, 35
    jmp proxy_resolve_common
proxy_winmm_joyGetDevCapsA ENDP

proxy_winmm_joyGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 288]
    test rax, rax
    jz proxy_winmm_joyGetDevCapsW_resolve
    jmp rax
proxy_winmm_joyGetDevCapsW_resolve:
    mov r11d, 36
    jmp proxy_resolve_common
proxy_winmm_joyGetDevCapsW ENDP

proxy_winmm_joyGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 296]
    test rax, rax
    jz proxy_winmm_joyGetNumDevs_resolve
    jmp rax
proxy_winmm_joyGetNumDevs_resolve:
    mov r11d, 37
    jmp proxy_resolve_common
proxy_winmm_joyGetNumDevs ENDP

proxy_winmm_joyGetPos PROC
    mov rax, qword ptr [glass_proxy_targets + 304]
    test rax, rax
    jz proxy_winmm_joyGetPos_resolve
    jmp rax
proxy_winmm_joyGetPos_resolve:
    mov r11d, 38
    jmp proxy_resolve_common
proxy_winmm_joyGetPos ENDP

proxy_winmm_joyGetPosEx PROC
    mov rax, qword ptr [glass_proxy_targets + 312]
    test rax, rax
    jz proxy_winmm_joyGetPosEx_resolve
    jmp rax
proxy_winmm_joyGetPosEx_resolve:
    mov r11d, 39
    jmp proxy_resolve_common
proxy_winmm_joyGetPosEx ENDP

proxy_winmm_joyGetThreshold PROC
    mov rax, qword ptr [glass_proxy_targets + 320]
    test rax, rax
    jz proxy_winmm_joyGetThreshold_resolve
    jmp rax
proxy_winmm_joyGetThreshold_resolve:
    mov r11d, 40
    jmp proxy_resolve_common
proxy_winmm_joyGetThreshold ENDP

proxy_winmm_joyReleaseCapture PROC
    mov rax, qword ptr [glass_proxy_targets + 328]
    test rax, rax
    jz proxy_winmm_joyReleaseCapture_resolve
    jmp rax
proxy_winmm_joyReleaseCapture_resolve:
    mov r11d, 41
    jmp proxy_resolve_common
proxy_winmm_joyReleaseCapture ENDP

proxy_winmm_joySetCapture PROC
    mov rax, qword ptr [glass_proxy_targets + 336]
    test rax, rax
    jz proxy_winmm_joySetCapture_resolve
    jmp rax
proxy_winmm_joySetCapture_resolve:
    mov r11d, 42
    jmp proxy_resolve_common
proxy_winmm_joySetCapture ENDP

proxy_winmm_joySetThreshold PROC
    mov rax, qword ptr [glass_proxy_targets + 344]
    test rax, rax
    jz proxy_winmm_joySetThreshold_resolve
    jmp rax
proxy_winmm_joySetThreshold_resolve:
    mov r11d, 43
    jmp proxy_resolve_common
proxy_winmm_joySetThreshold ENDP

proxy_winmm_mciDriverNotify PROC
    mov rax, qword ptr [glass_proxy_targets + 352]
    test rax, rax
    jz proxy_winmm_mciDriverNotify_resolve
    jmp rax
proxy_winmm_mciDriverNotify_resolve:
    mov r11d, 44
    jmp proxy_resolve_common
proxy_winmm_mciDriverNotify ENDP

proxy_winmm_mciDriverYield PROC
    mov rax, qword ptr [glass_proxy_targets + 360]
    test rax, rax
    jz proxy_winmm_mciDriverYield_resolve
    jmp rax
proxy_winmm_mciDriverYield_resolve:
    mov r11d, 45
    jmp proxy_resolve_common
proxy_winmm_mciDriverYield ENDP

proxy_winmm_mciExecute PROC
    mov rax, qword ptr [glass_proxy_targets + 368]
    test rax, rax
    jz proxy_winmm_mciExecute_resolve
    jmp rax
proxy_winmm_mciExecute_resolve:
    mov r11d, 46
    jmp proxy_resolve_common
proxy_winmm_mciExecute ENDP

proxy_winmm_mciFreeCommandResource PROC
    mov rax, qword ptr [glass_proxy_targets + 376]
    test rax, rax
    jz proxy_winmm_mciFreeCommandResource_resolve
    jmp rax
proxy_winmm_mciFreeCommandResource_resolve:
    mov r11d, 47
    jmp proxy_resolve_common
proxy_winmm_mciFreeCommandResource ENDP

proxy_winmm_mciGetCreatorTask PROC
    mov rax, qword ptr [glass_proxy_targets + 384]
    test rax, rax
    jz proxy_winmm_mciGetCreatorTask_resolve
    jmp rax
proxy_winmm_mciGetCreatorTask_resolve:
    mov r11d, 48
    jmp proxy_resolve_common
proxy_winmm_mciGetCreatorTask ENDP

proxy_winmm_mciGetDeviceIDA PROC
    mov rax, qword ptr [glass_proxy_targets + 392]
    test rax, rax
    jz proxy_winmm_mciGetDeviceIDA_resolve
    jmp rax
proxy_winmm_mciGetDeviceIDA_resolve:
    mov r11d, 49
    jmp proxy_resolve_common
proxy_winmm_mciGetDeviceIDA ENDP

proxy_winmm_mciGetDeviceIDFromElementIDA PROC
    mov rax, qword ptr [glass_proxy_targets + 400]
    test rax, rax
    jz proxy_winmm_mciGetDeviceIDFromElementIDA_resolve
    jmp rax
proxy_winmm_mciGetDeviceIDFromElementIDA_resolve:
    mov r11d, 50
    jmp proxy_resolve_common
proxy_winmm_mciGetDeviceIDFromElementIDA ENDP

proxy_winmm_mciGetDeviceIDFromElementIDW PROC
    mov rax, qword ptr [glass_proxy_targets + 408]
    test rax, rax
    jz proxy_winmm_mciGetDeviceIDFromElementIDW_resolve
    jmp rax
proxy_winmm_mciGetDeviceIDFromElementIDW_resolve:
    mov r11d, 51
    jmp proxy_resolve_common
proxy_winmm_mciGetDeviceIDFromElementIDW ENDP

proxy_winmm_mciGetDeviceIDW PROC
    mov rax, qword ptr [glass_proxy_targets + 416]
    test rax, rax
    jz proxy_winmm_mciGetDeviceIDW_resolve
    jmp rax
proxy_winmm_mciGetDeviceIDW_resolve:
    mov r11d, 52
    jmp proxy_resolve_common
proxy_winmm_mciGetDeviceIDW ENDP

proxy_winmm_mciGetDriverData PROC
    mov rax, qword ptr [glass_proxy_targets + 424]
    test rax, rax
    jz proxy_winmm_mciGetDriverData_resolve
    jmp rax
proxy_winmm_mciGetDriverData_resolve:
    mov r11d, 53
    jmp proxy_resolve_common
proxy_winmm_mciGetDriverData ENDP

proxy_winmm_mciGetErrorStringA PROC
    mov rax, qword ptr [glass_proxy_targets + 432]
    test rax, rax
    jz proxy_winmm_mciGetErrorStringA_resolve
    jmp rax
proxy_winmm_mciGetErrorStringA_resolve:
    mov r11d, 54
    jmp proxy_resolve_common
proxy_winmm_mciGetErrorStringA ENDP

proxy_winmm_mciGetErrorStringW PROC
    mov rax, qword ptr [glass_proxy_targets + 440]
    test rax, rax
    jz proxy_winmm_mciGetErrorStringW_resolve
    jmp rax
proxy_winmm_mciGetErrorStringW_resolve:
    mov r11d, 55
    jmp proxy_resolve_common
proxy_winmm_mciGetErrorStringW ENDP

proxy_winmm_mciGetYieldProc PROC
    mov rax, qword ptr [glass_proxy_targets + 448]
    test rax, rax
    jz proxy_winmm_mciGetYieldProc_resolve
    jmp rax
proxy_winmm_mciGetYieldProc_resolve:
    mov r11d, 56
    jmp proxy_resolve_common
proxy_winmm_mciGetYieldProc ENDP

proxy_winmm_mciLoadCommandResource PROC
    mov rax, qword ptr [glass_proxy_targets + 456]
    test rax, rax
    jz proxy_winmm_mciLoadCommandResource_resolve
    jmp rax
proxy_winmm_mciLoadCommandResource_resolve:
    mov r11d, 57
    jmp proxy_resolve_common
proxy_winmm_mciLoadCommandResource ENDP

proxy_winmm_mciSendCommandA PROC
    mov rax, qword ptr [glass_proxy_targets + 464]
    test rax, rax
    jz proxy_winmm_mciSendCommandA_resolve
    jmp rax
proxy_winmm_mciSendCommandA_resolve:
    mov r11d, 58
    jmp proxy_resolve_common
proxy_winmm_mciSendCommandA ENDP

proxy_winmm_mciSendCommandW PROC
    mov rax, qword ptr [glass_proxy_targets + 472]
    test rax, rax
    jz proxy_winmm_mciSendCommandW_resolve
    jmp rax
proxy_winmm_mciSendCommandW_resolve:
    mov r11d, 59
    jmp proxy_resolve_common
proxy_winmm_mciSendCommandW ENDP

proxy_winmm_mciSendStringA PROC
    mov rax, qword ptr [glass_proxy_targets + 480]
    test rax, rax
    jz proxy_winmm_mciSendStringA_resolve
    jmp rax
proxy_winmm_mciSendStringA_resolve:
    mov r11d, 60
    jmp proxy_resolve_common
proxy_winmm_mciSendStringA ENDP

proxy_winmm_mciSendStringW PROC
    mov rax, qword ptr [glass_proxy_targets + 488]
    test rax, rax
    jz proxy_winmm_mciSendStringW_resolve
    jmp rax
proxy_winmm_mciSendStringW_resolve:
    mov r11d, 61
    jmp proxy_resolve_common
proxy_winmm_mciSendStringW ENDP

proxy_winmm_mciSetDriverData PROC
    mov rax, qword ptr [glass_proxy_targets + 496]
    test rax, rax
    jz proxy_winmm_mciSetDriverData_resolve
    jmp rax
proxy_winmm_mciSetDriverData_resolve:
    mov r11d, 62
    jmp proxy_resolve_common
proxy_winmm_mciSetDriverData ENDP

proxy_winmm_mciSetYieldProc PROC
    mov rax, qword ptr [glass_proxy_targets + 504]
    test rax, rax
    jz proxy_winmm_mciSetYieldProc_resolve
    jmp rax
proxy_winmm_mciSetYieldProc_resolve:
    mov r11d, 63
    jmp proxy_resolve_common
proxy_winmm_mciSetYieldProc ENDP

proxy_winmm_midiConnect PROC
    mov rax, qword ptr [glass_proxy_targets + 512]
    test rax, rax
    jz proxy_winmm_midiConnect_resolve
    jmp rax
proxy_winmm_midiConnect_resolve:
    mov r11d, 64
    jmp proxy_resolve_common
proxy_winmm_midiConnect ENDP

proxy_winmm_midiDisconnect PROC
    mov rax, qword ptr [glass_proxy_targets + 520]
    test rax, rax
    jz proxy_winmm_midiDisconnect_resolve
    jmp rax
proxy_winmm_midiDisconnect_resolve:
    mov r11d, 65
    jmp proxy_resolve_common
proxy_winmm_midiDisconnect ENDP

proxy_winmm_midiInAddBuffer PROC
    mov rax, qword ptr [glass_proxy_targets + 528]
    test rax, rax
    jz proxy_winmm_midiInAddBuffer_resolve
    jmp rax
proxy_winmm_midiInAddBuffer_resolve:
    mov r11d, 66
    jmp proxy_resolve_common
proxy_winmm_midiInAddBuffer ENDP

proxy_winmm_midiInClose PROC
    mov rax, qword ptr [glass_proxy_targets + 536]
    test rax, rax
    jz proxy_winmm_midiInClose_resolve
    jmp rax
proxy_winmm_midiInClose_resolve:
    mov r11d, 67
    jmp proxy_resolve_common
proxy_winmm_midiInClose ENDP

proxy_winmm_midiInGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 544]
    test rax, rax
    jz proxy_winmm_midiInGetDevCapsA_resolve
    jmp rax
proxy_winmm_midiInGetDevCapsA_resolve:
    mov r11d, 68
    jmp proxy_resolve_common
proxy_winmm_midiInGetDevCapsA ENDP

proxy_winmm_midiInGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 552]
    test rax, rax
    jz proxy_winmm_midiInGetDevCapsW_resolve
    jmp rax
proxy_winmm_midiInGetDevCapsW_resolve:
    mov r11d, 69
    jmp proxy_resolve_common
proxy_winmm_midiInGetDevCapsW ENDP

proxy_winmm_midiInGetErrorTextA PROC
    mov rax, qword ptr [glass_proxy_targets + 560]
    test rax, rax
    jz proxy_winmm_midiInGetErrorTextA_resolve
    jmp rax
proxy_winmm_midiInGetErrorTextA_resolve:
    mov r11d, 70
    jmp proxy_resolve_common
proxy_winmm_midiInGetErrorTextA ENDP

proxy_winmm_midiInGetErrorTextW PROC
    mov rax, qword ptr [glass_proxy_targets + 568]
    test rax, rax
    jz proxy_winmm_midiInGetErrorTextW_resolve
    jmp rax
proxy_winmm_midiInGetErrorTextW_resolve:
    mov r11d, 71
    jmp proxy_resolve_common
proxy_winmm_midiInGetErrorTextW ENDP

proxy_winmm_midiInGetID PROC
    mov rax, qword ptr [glass_proxy_targets + 576]
    test rax, rax
    jz proxy_winmm_midiInGetID_resolve
    jmp rax
proxy_winmm_midiInGetID_resolve:
    mov r11d, 72
    jmp proxy_resolve_common
proxy_winmm_midiInGetID ENDP

proxy_winmm_midiInGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 584]
    test rax, rax
    jz proxy_winmm_midiInGetNumDevs_resolve
    jmp rax
proxy_winmm_midiInGetNumDevs_resolve:
    mov r11d, 73
    jmp proxy_resolve_common
proxy_winmm_midiInGetNumDevs ENDP

proxy_winmm_midiInMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 592]
    test rax, rax
    jz proxy_winmm_midiInMessage_resolve
    jmp rax
proxy_winmm_midiInMessage_resolve:
    mov r11d, 74
    jmp proxy_resolve_common
proxy_winmm_midiInMessage ENDP

proxy_winmm_midiInOpen PROC
    mov rax, qword ptr [glass_proxy_targets + 600]
    test rax, rax
    jz proxy_winmm_midiInOpen_resolve
    jmp rax
proxy_winmm_midiInOpen_resolve:
    mov r11d, 75
    jmp proxy_resolve_common
proxy_winmm_midiInOpen ENDP

proxy_winmm_midiInPrepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 608]
    test rax, rax
    jz proxy_winmm_midiInPrepareHeader_resolve
    jmp rax
proxy_winmm_midiInPrepareHeader_resolve:
    mov r11d, 76
    jmp proxy_resolve_common
proxy_winmm_midiInPrepareHeader ENDP

proxy_winmm_midiInReset PROC
    mov rax, qword ptr [glass_proxy_targets + 616]
    test rax, rax
    jz proxy_winmm_midiInReset_resolve
    jmp rax
proxy_winmm_midiInReset_resolve:
    mov r11d, 77
    jmp proxy_resolve_common
proxy_winmm_midiInReset ENDP

proxy_winmm_midiInStart PROC
    mov rax, qword ptr [glass_proxy_targets + 624]
    test rax, rax
    jz proxy_winmm_midiInStart_resolve
    jmp rax
proxy_winmm_midiInStart_resolve:
    mov r11d, 78
    jmp proxy_resolve_common
proxy_winmm_midiInStart ENDP

proxy_winmm_midiInStop PROC
    mov rax, qword ptr [glass_proxy_targets + 632]
    test rax, rax
    jz proxy_winmm_midiInStop_resolve
    jmp rax
proxy_winmm_midiInStop_resolve:
    mov r11d, 79
    jmp proxy_resolve_common
proxy_winmm_midiInStop ENDP

proxy_winmm_midiInUnprepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 640]
    test rax, rax
    jz proxy_winmm_midiInUnprepareHeader_resolve
    jmp rax
proxy_winmm_midiInUnprepareHeader_resolve:
    mov r11d, 80
    jmp proxy_resolve_common
proxy_winmm_midiInUnprepareHeader ENDP

proxy_winmm_midiOutCacheDrumPatches PROC
    mov rax, qword ptr [glass_proxy_targets + 648]
    test rax, rax
    jz proxy_winmm_midiOutCacheDrumPatches_resolve
    jmp rax
proxy_winmm_midiOutCacheDrumPatches_resolve:
    mov r11d, 81
    jmp proxy_resolve_common
proxy_winmm_midiOutCacheDrumPatches ENDP

proxy_winmm_midiOutCachePatches PROC
    mov rax, qword ptr [glass_proxy_targets + 656]
    test rax, rax
    jz proxy_winmm_midiOutCachePatches_resolve
    jmp rax
proxy_winmm_midiOutCachePatches_resolve:
    mov r11d, 82
    jmp proxy_resolve_common
proxy_winmm_midiOutCachePatches ENDP

proxy_winmm_midiOutClose PROC
    mov rax, qword ptr [glass_proxy_targets + 664]
    test rax, rax
    jz proxy_winmm_midiOutClose_resolve
    jmp rax
proxy_winmm_midiOutClose_resolve:
    mov r11d, 83
    jmp proxy_resolve_common
proxy_winmm_midiOutClose ENDP

proxy_winmm_midiOutGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 672]
    test rax, rax
    jz proxy_winmm_midiOutGetDevCapsA_resolve
    jmp rax
proxy_winmm_midiOutGetDevCapsA_resolve:
    mov r11d, 84
    jmp proxy_resolve_common
proxy_winmm_midiOutGetDevCapsA ENDP

proxy_winmm_midiOutGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 680]
    test rax, rax
    jz proxy_winmm_midiOutGetDevCapsW_resolve
    jmp rax
proxy_winmm_midiOutGetDevCapsW_resolve:
    mov r11d, 85
    jmp proxy_resolve_common
proxy_winmm_midiOutGetDevCapsW ENDP

proxy_winmm_midiOutGetErrorTextA PROC
    mov rax, qword ptr [glass_proxy_targets + 688]
    test rax, rax
    jz proxy_winmm_midiOutGetErrorTextA_resolve
    jmp rax
proxy_winmm_midiOutGetErrorTextA_resolve:
    mov r11d, 86
    jmp proxy_resolve_common
proxy_winmm_midiOutGetErrorTextA ENDP

proxy_winmm_midiOutGetErrorTextW PROC
    mov rax, qword ptr [glass_proxy_targets + 696]
    test rax, rax
    jz proxy_winmm_midiOutGetErrorTextW_resolve
    jmp rax
proxy_winmm_midiOutGetErrorTextW_resolve:
    mov r11d, 87
    jmp proxy_resolve_common
proxy_winmm_midiOutGetErrorTextW ENDP

proxy_winmm_midiOutGetID PROC
    mov rax, qword ptr [glass_proxy_targets + 704]
    test rax, rax
    jz proxy_winmm_midiOutGetID_resolve
    jmp rax
proxy_winmm_midiOutGetID_resolve:
    mov r11d, 88
    jmp proxy_resolve_common
proxy_winmm_midiOutGetID ENDP

proxy_winmm_midiOutGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 712]
    test rax, rax
    jz proxy_winmm_midiOutGetNumDevs_resolve
    jmp rax
proxy_winmm_midiOutGetNumDevs_resolve:
    mov r11d, 89
    jmp proxy_resolve_common
proxy_winmm_midiOutGetNumDevs ENDP

proxy_winmm_midiOutGetVolume PROC
    mov rax, qword ptr [glass_proxy_targets + 720]
    test rax, rax
    jz proxy_winmm_midiOutGetVolume_resolve
    jmp rax
proxy_winmm_midiOutGetVolume_resolve:
    mov r11d, 90
    jmp proxy_resolve_common
proxy_winmm_midiOutGetVolume ENDP

proxy_winmm_midiOutLongMsg PROC
    mov rax, qword ptr [glass_proxy_targets + 728]
    test rax, rax
    jz proxy_winmm_midiOutLongMsg_resolve
    jmp rax
proxy_winmm_midiOutLongMsg_resolve:
    mov r11d, 91
    jmp proxy_resolve_common
proxy_winmm_midiOutLongMsg ENDP

proxy_winmm_midiOutMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 736]
    test rax, rax
    jz proxy_winmm_midiOutMessage_resolve
    jmp rax
proxy_winmm_midiOutMessage_resolve:
    mov r11d, 92
    jmp proxy_resolve_common
proxy_winmm_midiOutMessage ENDP

proxy_winmm_midiOutOpen PROC
    mov rax, qword ptr [glass_proxy_targets + 744]
    test rax, rax
    jz proxy_winmm_midiOutOpen_resolve
    jmp rax
proxy_winmm_midiOutOpen_resolve:
    mov r11d, 93
    jmp proxy_resolve_common
proxy_winmm_midiOutOpen ENDP

proxy_winmm_midiOutPrepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 752]
    test rax, rax
    jz proxy_winmm_midiOutPrepareHeader_resolve
    jmp rax
proxy_winmm_midiOutPrepareHeader_resolve:
    mov r11d, 94
    jmp proxy_resolve_common
proxy_winmm_midiOutPrepareHeader ENDP

proxy_winmm_midiOutReset PROC
    mov rax, qword ptr [glass_proxy_targets + 760]
    test rax, rax
    jz proxy_winmm_midiOutReset_resolve
    jmp rax
proxy_winmm_midiOutReset_resolve:
    mov r11d, 95
    jmp proxy_resolve_common
proxy_winmm_midiOutReset ENDP

proxy_winmm_midiOutSetVolume PROC
    mov rax, qword ptr [glass_proxy_targets + 768]
    test rax, rax
    jz proxy_winmm_midiOutSetVolume_resolve
    jmp rax
proxy_winmm_midiOutSetVolume_resolve:
    mov r11d, 96
    jmp proxy_resolve_common
proxy_winmm_midiOutSetVolume ENDP

proxy_winmm_midiOutShortMsg PROC
    mov rax, qword ptr [glass_proxy_targets + 776]
    test rax, rax
    jz proxy_winmm_midiOutShortMsg_resolve
    jmp rax
proxy_winmm_midiOutShortMsg_resolve:
    mov r11d, 97
    jmp proxy_resolve_common
proxy_winmm_midiOutShortMsg ENDP

proxy_winmm_midiOutUnprepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 784]
    test rax, rax
    jz proxy_winmm_midiOutUnprepareHeader_resolve
    jmp rax
proxy_winmm_midiOutUnprepareHeader_resolve:
    mov r11d, 98
    jmp proxy_resolve_common
proxy_winmm_midiOutUnprepareHeader ENDP

proxy_winmm_midiStreamClose PROC
    mov rax, qword ptr [glass_proxy_targets + 792]
    test rax, rax
    jz proxy_winmm_midiStreamClose_resolve
    jmp rax
proxy_winmm_midiStreamClose_resolve:
    mov r11d, 99
    jmp proxy_resolve_common
proxy_winmm_midiStreamClose ENDP

proxy_winmm_midiStreamOpen PROC
    mov rax, qword ptr [glass_proxy_targets + 800]
    test rax, rax
    jz proxy_winmm_midiStreamOpen_resolve
    jmp rax
proxy_winmm_midiStreamOpen_resolve:
    mov r11d, 100
    jmp proxy_resolve_common
proxy_winmm_midiStreamOpen ENDP

proxy_winmm_midiStreamOut PROC
    mov rax, qword ptr [glass_proxy_targets + 808]
    test rax, rax
    jz proxy_winmm_midiStreamOut_resolve
    jmp rax
proxy_winmm_midiStreamOut_resolve:
    mov r11d, 101
    jmp proxy_resolve_common
proxy_winmm_midiStreamOut ENDP

proxy_winmm_midiStreamPause PROC
    mov rax, qword ptr [glass_proxy_targets + 816]
    test rax, rax
    jz proxy_winmm_midiStreamPause_resolve
    jmp rax
proxy_winmm_midiStreamPause_resolve:
    mov r11d, 102
    jmp proxy_resolve_common
proxy_winmm_midiStreamPause ENDP

proxy_winmm_midiStreamPosition PROC
    mov rax, qword ptr [glass_proxy_targets + 824]
    test rax, rax
    jz proxy_winmm_midiStreamPosition_resolve
    jmp rax
proxy_winmm_midiStreamPosition_resolve:
    mov r11d, 103
    jmp proxy_resolve_common
proxy_winmm_midiStreamPosition ENDP

proxy_winmm_midiStreamProperty PROC
    mov rax, qword ptr [glass_proxy_targets + 832]
    test rax, rax
    jz proxy_winmm_midiStreamProperty_resolve
    jmp rax
proxy_winmm_midiStreamProperty_resolve:
    mov r11d, 104
    jmp proxy_resolve_common
proxy_winmm_midiStreamProperty ENDP

proxy_winmm_midiStreamRestart PROC
    mov rax, qword ptr [glass_proxy_targets + 840]
    test rax, rax
    jz proxy_winmm_midiStreamRestart_resolve
    jmp rax
proxy_winmm_midiStreamRestart_resolve:
    mov r11d, 105
    jmp proxy_resolve_common
proxy_winmm_midiStreamRestart ENDP

proxy_winmm_midiStreamStop PROC
    mov rax, qword ptr [glass_proxy_targets + 848]
    test rax, rax
    jz proxy_winmm_midiStreamStop_resolve
    jmp rax
proxy_winmm_midiStreamStop_resolve:
    mov r11d, 106
    jmp proxy_resolve_common
proxy_winmm_midiStreamStop ENDP

proxy_winmm_mixerClose PROC
    mov rax, qword ptr [glass_proxy_targets + 856]
    test rax, rax
    jz proxy_winmm_mixerClose_resolve
    jmp rax
proxy_winmm_mixerClose_resolve:
    mov r11d, 107
    jmp proxy_resolve_common
proxy_winmm_mixerClose ENDP

proxy_winmm_mixerGetControlDetailsA PROC
    mov rax, qword ptr [glass_proxy_targets + 864]
    test rax, rax
    jz proxy_winmm_mixerGetControlDetailsA_resolve
    jmp rax
proxy_winmm_mixerGetControlDetailsA_resolve:
    mov r11d, 108
    jmp proxy_resolve_common
proxy_winmm_mixerGetControlDetailsA ENDP

proxy_winmm_mixerGetControlDetailsW PROC
    mov rax, qword ptr [glass_proxy_targets + 872]
    test rax, rax
    jz proxy_winmm_mixerGetControlDetailsW_resolve
    jmp rax
proxy_winmm_mixerGetControlDetailsW_resolve:
    mov r11d, 109
    jmp proxy_resolve_common
proxy_winmm_mixerGetControlDetailsW ENDP

proxy_winmm_mixerGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 880]
    test rax, rax
    jz proxy_winmm_mixerGetDevCapsA_resolve
    jmp rax
proxy_winmm_mixerGetDevCapsA_resolve:
    mov r11d, 110
    jmp proxy_resolve_common
proxy_winmm_mixerGetDevCapsA ENDP

proxy_winmm_mixerGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 888]
    test rax, rax
    jz proxy_winmm_mixerGetDevCapsW_resolve
    jmp rax
proxy_winmm_mixerGetDevCapsW_resolve:
    mov r11d, 111
    jmp proxy_resolve_common
proxy_winmm_mixerGetDevCapsW ENDP

proxy_winmm_mixerGetID PROC
    mov rax, qword ptr [glass_proxy_targets + 896]
    test rax, rax
    jz proxy_winmm_mixerGetID_resolve
    jmp rax
proxy_winmm_mixerGetID_resolve:
    mov r11d, 112
    jmp proxy_resolve_common
proxy_winmm_mixerGetID ENDP

proxy_winmm_mixerGetLineControlsA PROC
    mov rax, qword ptr [glass_proxy_targets + 904]
    test rax, rax
    jz proxy_winmm_mixerGetLineControlsA_resolve
    jmp rax
proxy_winmm_mixerGetLineControlsA_resolve:
    mov r11d, 113
    jmp proxy_resolve_common
proxy_winmm_mixerGetLineControlsA ENDP

proxy_winmm_mixerGetLineControlsW PROC
    mov rax, qword ptr [glass_proxy_targets + 912]
    test rax, rax
    jz proxy_winmm_mixerGetLineControlsW_resolve
    jmp rax
proxy_winmm_mixerGetLineControlsW_resolve:
    mov r11d, 114
    jmp proxy_resolve_common
proxy_winmm_mixerGetLineControlsW ENDP

proxy_winmm_mixerGetLineInfoA PROC
    mov rax, qword ptr [glass_proxy_targets + 920]
    test rax, rax
    jz proxy_winmm_mixerGetLineInfoA_resolve
    jmp rax
proxy_winmm_mixerGetLineInfoA_resolve:
    mov r11d, 115
    jmp proxy_resolve_common
proxy_winmm_mixerGetLineInfoA ENDP

proxy_winmm_mixerGetLineInfoW PROC
    mov rax, qword ptr [glass_proxy_targets + 928]
    test rax, rax
    jz proxy_winmm_mixerGetLineInfoW_resolve
    jmp rax
proxy_winmm_mixerGetLineInfoW_resolve:
    mov r11d, 116
    jmp proxy_resolve_common
proxy_winmm_mixerGetLineInfoW ENDP

proxy_winmm_mixerGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 936]
    test rax, rax
    jz proxy_winmm_mixerGetNumDevs_resolve
    jmp rax
proxy_winmm_mixerGetNumDevs_resolve:
    mov r11d, 117
    jmp proxy_resolve_common
proxy_winmm_mixerGetNumDevs ENDP

proxy_winmm_mixerMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 944]
    test rax, rax
    jz proxy_winmm_mixerMessage_resolve
    jmp rax
proxy_winmm_mixerMessage_resolve:
    mov r11d, 118
    jmp proxy_resolve_common
proxy_winmm_mixerMessage ENDP

proxy_winmm_mixerOpen PROC
    mov rax, qword ptr [glass_proxy_targets + 952]
    test rax, rax
    jz proxy_winmm_mixerOpen_resolve
    jmp rax
proxy_winmm_mixerOpen_resolve:
    mov r11d, 119
    jmp proxy_resolve_common
proxy_winmm_mixerOpen ENDP

proxy_winmm_mixerSetControlDetails PROC
    mov rax, qword ptr [glass_proxy_targets + 960]
    test rax, rax
    jz proxy_winmm_mixerSetControlDetails_resolve
    jmp rax
proxy_winmm_mixerSetControlDetails_resolve:
    mov r11d, 120
    jmp proxy_resolve_common
proxy_winmm_mixerSetControlDetails ENDP

proxy_winmm_mmDrvInstall PROC
    mov rax, qword ptr [glass_proxy_targets + 968]
    test rax, rax
    jz proxy_winmm_mmDrvInstall_resolve
    jmp rax
proxy_winmm_mmDrvInstall_resolve:
    mov r11d, 121
    jmp proxy_resolve_common
proxy_winmm_mmDrvInstall ENDP

proxy_winmm_mmGetCurrentTask PROC
    mov rax, qword ptr [glass_proxy_targets + 976]
    test rax, rax
    jz proxy_winmm_mmGetCurrentTask_resolve
    jmp rax
proxy_winmm_mmGetCurrentTask_resolve:
    mov r11d, 122
    jmp proxy_resolve_common
proxy_winmm_mmGetCurrentTask ENDP

proxy_winmm_mmTaskBlock PROC
    mov rax, qword ptr [glass_proxy_targets + 984]
    test rax, rax
    jz proxy_winmm_mmTaskBlock_resolve
    jmp rax
proxy_winmm_mmTaskBlock_resolve:
    mov r11d, 123
    jmp proxy_resolve_common
proxy_winmm_mmTaskBlock ENDP

proxy_winmm_mmTaskCreate PROC
    mov rax, qword ptr [glass_proxy_targets + 992]
    test rax, rax
    jz proxy_winmm_mmTaskCreate_resolve
    jmp rax
proxy_winmm_mmTaskCreate_resolve:
    mov r11d, 124
    jmp proxy_resolve_common
proxy_winmm_mmTaskCreate ENDP

proxy_winmm_mmTaskSignal PROC
    mov rax, qword ptr [glass_proxy_targets + 1000]
    test rax, rax
    jz proxy_winmm_mmTaskSignal_resolve
    jmp rax
proxy_winmm_mmTaskSignal_resolve:
    mov r11d, 125
    jmp proxy_resolve_common
proxy_winmm_mmTaskSignal ENDP

proxy_winmm_mmTaskYield PROC
    mov rax, qword ptr [glass_proxy_targets + 1008]
    test rax, rax
    jz proxy_winmm_mmTaskYield_resolve
    jmp rax
proxy_winmm_mmTaskYield_resolve:
    mov r11d, 126
    jmp proxy_resolve_common
proxy_winmm_mmTaskYield ENDP

proxy_winmm_mmioAdvance PROC
    mov rax, qword ptr [glass_proxy_targets + 1016]
    test rax, rax
    jz proxy_winmm_mmioAdvance_resolve
    jmp rax
proxy_winmm_mmioAdvance_resolve:
    mov r11d, 127
    jmp proxy_resolve_common
proxy_winmm_mmioAdvance ENDP

proxy_winmm_mmioAscend PROC
    mov rax, qword ptr [glass_proxy_targets + 1024]
    test rax, rax
    jz proxy_winmm_mmioAscend_resolve
    jmp rax
proxy_winmm_mmioAscend_resolve:
    mov r11d, 128
    jmp proxy_resolve_common
proxy_winmm_mmioAscend ENDP

proxy_winmm_mmioClose PROC
    mov rax, qword ptr [glass_proxy_targets + 1032]
    test rax, rax
    jz proxy_winmm_mmioClose_resolve
    jmp rax
proxy_winmm_mmioClose_resolve:
    mov r11d, 129
    jmp proxy_resolve_common
proxy_winmm_mmioClose ENDP

proxy_winmm_mmioCreateChunk PROC
    mov rax, qword ptr [glass_proxy_targets + 1040]
    test rax, rax
    jz proxy_winmm_mmioCreateChunk_resolve
    jmp rax
proxy_winmm_mmioCreateChunk_resolve:
    mov r11d, 130
    jmp proxy_resolve_common
proxy_winmm_mmioCreateChunk ENDP

proxy_winmm_mmioDescend PROC
    mov rax, qword ptr [glass_proxy_targets + 1048]
    test rax, rax
    jz proxy_winmm_mmioDescend_resolve
    jmp rax
proxy_winmm_mmioDescend_resolve:
    mov r11d, 131
    jmp proxy_resolve_common
proxy_winmm_mmioDescend ENDP

proxy_winmm_mmioFlush PROC
    mov rax, qword ptr [glass_proxy_targets + 1056]
    test rax, rax
    jz proxy_winmm_mmioFlush_resolve
    jmp rax
proxy_winmm_mmioFlush_resolve:
    mov r11d, 132
    jmp proxy_resolve_common
proxy_winmm_mmioFlush ENDP

proxy_winmm_mmioGetInfo PROC
    mov rax, qword ptr [glass_proxy_targets + 1064]
    test rax, rax
    jz proxy_winmm_mmioGetInfo_resolve
    jmp rax
proxy_winmm_mmioGetInfo_resolve:
    mov r11d, 133
    jmp proxy_resolve_common
proxy_winmm_mmioGetInfo ENDP

proxy_winmm_mmioInstallIOProcA PROC
    mov rax, qword ptr [glass_proxy_targets + 1072]
    test rax, rax
    jz proxy_winmm_mmioInstallIOProcA_resolve
    jmp rax
proxy_winmm_mmioInstallIOProcA_resolve:
    mov r11d, 134
    jmp proxy_resolve_common
proxy_winmm_mmioInstallIOProcA ENDP

proxy_winmm_mmioInstallIOProcW PROC
    mov rax, qword ptr [glass_proxy_targets + 1080]
    test rax, rax
    jz proxy_winmm_mmioInstallIOProcW_resolve
    jmp rax
proxy_winmm_mmioInstallIOProcW_resolve:
    mov r11d, 135
    jmp proxy_resolve_common
proxy_winmm_mmioInstallIOProcW ENDP

proxy_winmm_mmioOpenA PROC
    mov rax, qword ptr [glass_proxy_targets + 1088]
    test rax, rax
    jz proxy_winmm_mmioOpenA_resolve
    jmp rax
proxy_winmm_mmioOpenA_resolve:
    mov r11d, 136
    jmp proxy_resolve_common
proxy_winmm_mmioOpenA ENDP

proxy_winmm_mmioOpenW PROC
    mov rax, qword ptr [glass_proxy_targets + 1096]
    test rax, rax
    jz proxy_winmm_mmioOpenW_resolve
    jmp rax
proxy_winmm_mmioOpenW_resolve:
    mov r11d, 137
    jmp proxy_resolve_common
proxy_winmm_mmioOpenW ENDP

proxy_winmm_mmioRead PROC
    mov rax, qword ptr [glass_proxy_targets + 1104]
    test rax, rax
    jz proxy_winmm_mmioRead_resolve
    jmp rax
proxy_winmm_mmioRead_resolve:
    mov r11d, 138
    jmp proxy_resolve_common
proxy_winmm_mmioRead ENDP

proxy_winmm_mmioRenameA PROC
    mov rax, qword ptr [glass_proxy_targets + 1112]
    test rax, rax
    jz proxy_winmm_mmioRenameA_resolve
    jmp rax
proxy_winmm_mmioRenameA_resolve:
    mov r11d, 139
    jmp proxy_resolve_common
proxy_winmm_mmioRenameA ENDP

proxy_winmm_mmioRenameW PROC
    mov rax, qword ptr [glass_proxy_targets + 1120]
    test rax, rax
    jz proxy_winmm_mmioRenameW_resolve
    jmp rax
proxy_winmm_mmioRenameW_resolve:
    mov r11d, 140
    jmp proxy_resolve_common
proxy_winmm_mmioRenameW ENDP

proxy_winmm_mmioSeek PROC
    mov rax, qword ptr [glass_proxy_targets + 1128]
    test rax, rax
    jz proxy_winmm_mmioSeek_resolve
    jmp rax
proxy_winmm_mmioSeek_resolve:
    mov r11d, 141
    jmp proxy_resolve_common
proxy_winmm_mmioSeek ENDP

proxy_winmm_mmioSendMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 1136]
    test rax, rax
    jz proxy_winmm_mmioSendMessage_resolve
    jmp rax
proxy_winmm_mmioSendMessage_resolve:
    mov r11d, 142
    jmp proxy_resolve_common
proxy_winmm_mmioSendMessage ENDP

proxy_winmm_mmioSetBuffer PROC
    mov rax, qword ptr [glass_proxy_targets + 1144]
    test rax, rax
    jz proxy_winmm_mmioSetBuffer_resolve
    jmp rax
proxy_winmm_mmioSetBuffer_resolve:
    mov r11d, 143
    jmp proxy_resolve_common
proxy_winmm_mmioSetBuffer ENDP

proxy_winmm_mmioSetInfo PROC
    mov rax, qword ptr [glass_proxy_targets + 1152]
    test rax, rax
    jz proxy_winmm_mmioSetInfo_resolve
    jmp rax
proxy_winmm_mmioSetInfo_resolve:
    mov r11d, 144
    jmp proxy_resolve_common
proxy_winmm_mmioSetInfo ENDP

proxy_winmm_mmioStringToFOURCCA PROC
    mov rax, qword ptr [glass_proxy_targets + 1160]
    test rax, rax
    jz proxy_winmm_mmioStringToFOURCCA_resolve
    jmp rax
proxy_winmm_mmioStringToFOURCCA_resolve:
    mov r11d, 145
    jmp proxy_resolve_common
proxy_winmm_mmioStringToFOURCCA ENDP

proxy_winmm_mmioStringToFOURCCW PROC
    mov rax, qword ptr [glass_proxy_targets + 1168]
    test rax, rax
    jz proxy_winmm_mmioStringToFOURCCW_resolve
    jmp rax
proxy_winmm_mmioStringToFOURCCW_resolve:
    mov r11d, 146
    jmp proxy_resolve_common
proxy_winmm_mmioStringToFOURCCW ENDP

proxy_winmm_mmioWrite PROC
    mov rax, qword ptr [glass_proxy_targets + 1176]
    test rax, rax
    jz proxy_winmm_mmioWrite_resolve
    jmp rax
proxy_winmm_mmioWrite_resolve:
    mov r11d, 147
    jmp proxy_resolve_common
proxy_winmm_mmioWrite ENDP

proxy_winmm_mmsystemGetVersion PROC
    mov rax, qword ptr [glass_proxy_targets + 1184]
    test rax, rax
    jz proxy_winmm_mmsystemGetVersion_resolve
    jmp rax
proxy_winmm_mmsystemGetVersion_resolve:
    mov r11d, 148
    jmp proxy_resolve_common
proxy_winmm_mmsystemGetVersion ENDP

proxy_winmm_sndPlaySoundA PROC
    mov rax, qword ptr [glass_proxy_targets + 1192]
    test rax, rax
    jz proxy_winmm_sndPlaySoundA_resolve
    jmp rax
proxy_winmm_sndPlaySoundA_resolve:
    mov r11d, 149
    jmp proxy_resolve_common
proxy_winmm_sndPlaySoundA ENDP

proxy_winmm_sndPlaySoundW PROC
    mov rax, qword ptr [glass_proxy_targets + 1200]
    test rax, rax
    jz proxy_winmm_sndPlaySoundW_resolve
    jmp rax
proxy_winmm_sndPlaySoundW_resolve:
    mov r11d, 150
    jmp proxy_resolve_common
proxy_winmm_sndPlaySoundW ENDP

proxy_winmm_timeBeginPeriod PROC
    mov rax, qword ptr [glass_proxy_targets + 1208]
    test rax, rax
    jz proxy_winmm_timeBeginPeriod_resolve
    jmp rax
proxy_winmm_timeBeginPeriod_resolve:
    mov r11d, 151
    jmp proxy_resolve_common
proxy_winmm_timeBeginPeriod ENDP

proxy_winmm_timeEndPeriod PROC
    mov rax, qword ptr [glass_proxy_targets + 1216]
    test rax, rax
    jz proxy_winmm_timeEndPeriod_resolve
    jmp rax
proxy_winmm_timeEndPeriod_resolve:
    mov r11d, 152
    jmp proxy_resolve_common
proxy_winmm_timeEndPeriod ENDP

proxy_winmm_timeGetDevCaps PROC
    mov rax, qword ptr [glass_proxy_targets + 1224]
    test rax, rax
    jz proxy_winmm_timeGetDevCaps_resolve
    jmp rax
proxy_winmm_timeGetDevCaps_resolve:
    mov r11d, 153
    jmp proxy_resolve_common
proxy_winmm_timeGetDevCaps ENDP

proxy_winmm_timeGetSystemTime PROC
    mov rax, qword ptr [glass_proxy_targets + 1232]
    test rax, rax
    jz proxy_winmm_timeGetSystemTime_resolve
    jmp rax
proxy_winmm_timeGetSystemTime_resolve:
    mov r11d, 154
    jmp proxy_resolve_common
proxy_winmm_timeGetSystemTime ENDP

proxy_winmm_timeGetTime PROC
    mov rax, qword ptr [glass_proxy_targets + 1240]
    test rax, rax
    jz proxy_winmm_timeGetTime_resolve
    jmp rax
proxy_winmm_timeGetTime_resolve:
    mov r11d, 155
    jmp proxy_resolve_common
proxy_winmm_timeGetTime ENDP

proxy_winmm_timeKillEvent PROC
    mov rax, qword ptr [glass_proxy_targets + 1248]
    test rax, rax
    jz proxy_winmm_timeKillEvent_resolve
    jmp rax
proxy_winmm_timeKillEvent_resolve:
    mov r11d, 156
    jmp proxy_resolve_common
proxy_winmm_timeKillEvent ENDP

proxy_winmm_timeSetEvent PROC
    mov rax, qword ptr [glass_proxy_targets + 1256]
    test rax, rax
    jz proxy_winmm_timeSetEvent_resolve
    jmp rax
proxy_winmm_timeSetEvent_resolve:
    mov r11d, 157
    jmp proxy_resolve_common
proxy_winmm_timeSetEvent ENDP

proxy_winmm_waveInAddBuffer PROC
    mov rax, qword ptr [glass_proxy_targets + 1264]
    test rax, rax
    jz proxy_winmm_waveInAddBuffer_resolve
    jmp rax
proxy_winmm_waveInAddBuffer_resolve:
    mov r11d, 158
    jmp proxy_resolve_common
proxy_winmm_waveInAddBuffer ENDP

proxy_winmm_waveInClose PROC
    mov rax, qword ptr [glass_proxy_targets + 1272]
    test rax, rax
    jz proxy_winmm_waveInClose_resolve
    jmp rax
proxy_winmm_waveInClose_resolve:
    mov r11d, 159
    jmp proxy_resolve_common
proxy_winmm_waveInClose ENDP

proxy_winmm_waveInGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 1280]
    test rax, rax
    jz proxy_winmm_waveInGetDevCapsA_resolve
    jmp rax
proxy_winmm_waveInGetDevCapsA_resolve:
    mov r11d, 160
    jmp proxy_resolve_common
proxy_winmm_waveInGetDevCapsA ENDP

proxy_winmm_waveInGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 1288]
    test rax, rax
    jz proxy_winmm_waveInGetDevCapsW_resolve
    jmp rax
proxy_winmm_waveInGetDevCapsW_resolve:
    mov r11d, 161
    jmp proxy_resolve_common
proxy_winmm_waveInGetDevCapsW ENDP

proxy_winmm_waveInGetErrorTextA PROC
    mov rax, qword ptr [glass_proxy_targets + 1296]
    test rax, rax
    jz proxy_winmm_waveInGetErrorTextA_resolve
    jmp rax
proxy_winmm_waveInGetErrorTextA_resolve:
    mov r11d, 162
    jmp proxy_resolve_common
proxy_winmm_waveInGetErrorTextA ENDP

proxy_winmm_waveInGetErrorTextW PROC
    mov rax, qword ptr [glass_proxy_targets + 1304]
    test rax, rax
    jz proxy_winmm_waveInGetErrorTextW_resolve
    jmp rax
proxy_winmm_waveInGetErrorTextW_resolve:
    mov r11d, 163
    jmp proxy_resolve_common
proxy_winmm_waveInGetErrorTextW ENDP

proxy_winmm_waveInGetID PROC
    mov rax, qword ptr [glass_proxy_targets + 1312]
    test rax, rax
    jz proxy_winmm_waveInGetID_resolve
    jmp rax
proxy_winmm_waveInGetID_resolve:
    mov r11d, 164
    jmp proxy_resolve_common
proxy_winmm_waveInGetID ENDP

proxy_winmm_waveInGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 1320]
    test rax, rax
    jz proxy_winmm_waveInGetNumDevs_resolve
    jmp rax
proxy_winmm_waveInGetNumDevs_resolve:
    mov r11d, 165
    jmp proxy_resolve_common
proxy_winmm_waveInGetNumDevs ENDP

proxy_winmm_waveInGetPosition PROC
    mov rax, qword ptr [glass_proxy_targets + 1328]
    test rax, rax
    jz proxy_winmm_waveInGetPosition_resolve
    jmp rax
proxy_winmm_waveInGetPosition_resolve:
    mov r11d, 166
    jmp proxy_resolve_common
proxy_winmm_waveInGetPosition ENDP

proxy_winmm_waveInMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 1336]
    test rax, rax
    jz proxy_winmm_waveInMessage_resolve
    jmp rax
proxy_winmm_waveInMessage_resolve:
    mov r11d, 167
    jmp proxy_resolve_common
proxy_winmm_waveInMessage ENDP

proxy_winmm_waveInOpen PROC
    mov rax, qword ptr [glass_proxy_targets + 1344]
    test rax, rax
    jz proxy_winmm_waveInOpen_resolve
    jmp rax
proxy_winmm_waveInOpen_resolve:
    mov r11d, 168
    jmp proxy_resolve_common
proxy_winmm_waveInOpen ENDP

proxy_winmm_waveInPrepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 1352]
    test rax, rax
    jz proxy_winmm_waveInPrepareHeader_resolve
    jmp rax
proxy_winmm_waveInPrepareHeader_resolve:
    mov r11d, 169
    jmp proxy_resolve_common
proxy_winmm_waveInPrepareHeader ENDP

proxy_winmm_waveInReset PROC
    mov rax, qword ptr [glass_proxy_targets + 1360]
    test rax, rax
    jz proxy_winmm_waveInReset_resolve
    jmp rax
proxy_winmm_waveInReset_resolve:
    mov r11d, 170
    jmp proxy_resolve_common
proxy_winmm_waveInReset ENDP

proxy_winmm_waveInStart PROC
    mov rax, qword ptr [glass_proxy_targets + 1368]
    test rax, rax
    jz proxy_winmm_waveInStart_resolve
    jmp rax
proxy_winmm_waveInStart_resolve:
    mov r11d, 171
    jmp proxy_resolve_common
proxy_winmm_waveInStart ENDP

proxy_winmm_waveInStop PROC
    mov rax, qword ptr [glass_proxy_targets + 1376]
    test rax, rax
    jz proxy_winmm_waveInStop_resolve
    jmp rax
proxy_winmm_waveInStop_resolve:
    mov r11d, 172
    jmp proxy_resolve_common
proxy_winmm_waveInStop ENDP

proxy_winmm_waveInUnprepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 1384]
    test rax, rax
    jz proxy_winmm_waveInUnprepareHeader_resolve
    jmp rax
proxy_winmm_waveInUnprepareHeader_resolve:
    mov r11d, 173
    jmp proxy_resolve_common
proxy_winmm_waveInUnprepareHeader ENDP

proxy_winmm_waveOutBreakLoop PROC
    mov rax, qword ptr [glass_proxy_targets + 1392]
    test rax, rax
    jz proxy_winmm_waveOutBreakLoop_resolve
    jmp rax
proxy_winmm_waveOutBreakLoop_resolve:
    mov r11d, 174
    jmp proxy_resolve_common
proxy_winmm_waveOutBreakLoop ENDP

proxy_winmm_waveOutClose PROC
    mov rax, qword ptr [glass_proxy_targets + 1400]
    test rax, rax
    jz proxy_winmm_waveOutClose_resolve
    jmp rax
proxy_winmm_waveOutClose_resolve:
    mov r11d, 175
    jmp proxy_resolve_common
proxy_winmm_waveOutClose ENDP

proxy_winmm_waveOutGetDevCapsA PROC
    mov rax, qword ptr [glass_proxy_targets + 1408]
    test rax, rax
    jz proxy_winmm_waveOutGetDevCapsA_resolve
    jmp rax
proxy_winmm_waveOutGetDevCapsA_resolve:
    mov r11d, 176
    jmp proxy_resolve_common
proxy_winmm_waveOutGetDevCapsA ENDP

proxy_winmm_waveOutGetDevCapsW PROC
    mov rax, qword ptr [glass_proxy_targets + 1416]
    test rax, rax
    jz proxy_winmm_waveOutGetDevCapsW_resolve
    jmp rax
proxy_winmm_waveOutGetDevCapsW_resolve:
    mov r11d, 177
    jmp proxy_resolve_common
proxy_winmm_waveOutGetDevCapsW ENDP

proxy_winmm_waveOutGetErrorTextA PROC
    mov rax, qword ptr [glass_proxy_targets + 1424]
    test rax, rax
    jz proxy_winmm_waveOutGetErrorTextA_resolve
    jmp rax
proxy_winmm_waveOutGetErrorTextA_resolve:
    mov r11d, 178
    jmp proxy_resolve_common
proxy_winmm_waveOutGetErrorTextA ENDP

proxy_winmm_waveOutGetErrorTextW PROC
    mov rax, qword ptr [glass_proxy_targets + 1432]
    test rax, rax
    jz proxy_winmm_waveOutGetErrorTextW_resolve
    jmp rax
proxy_winmm_waveOutGetErrorTextW_resolve:
    mov r11d, 179
    jmp proxy_resolve_common
proxy_winmm_waveOutGetErrorTextW ENDP

proxy_winmm_waveOutGetID PROC
    mov rax, qword ptr [glass_proxy_targets + 1440]
    test rax, rax
    jz proxy_winmm_waveOutGetID_resolve
    jmp rax
proxy_winmm_waveOutGetID_resolve:
    mov r11d, 180
    jmp proxy_resolve_common
proxy_winmm_waveOutGetID ENDP

proxy_winmm_waveOutGetNumDevs PROC
    mov rax, qword ptr [glass_proxy_targets + 1448]
    test rax, rax
    jz proxy_winmm_waveOutGetNumDevs_resolve
    jmp rax
proxy_winmm_waveOutGetNumDevs_resolve:
    mov r11d, 181
    jmp proxy_resolve_common
proxy_winmm_waveOutGetNumDevs ENDP

proxy_winmm_waveOutGetPitch PROC
    mov rax, qword ptr [glass_proxy_targets + 1456]
    test rax, rax
    jz proxy_winmm_waveOutGetPitch_resolve
    jmp rax
proxy_winmm_waveOutGetPitch_resolve:
    mov r11d, 182
    jmp proxy_resolve_common
proxy_winmm_waveOutGetPitch ENDP

proxy_winmm_waveOutGetPlaybackRate PROC
    mov rax, qword ptr [glass_proxy_targets + 1464]
    test rax, rax
    jz proxy_winmm_waveOutGetPlaybackRate_resolve
    jmp rax
proxy_winmm_waveOutGetPlaybackRate_resolve:
    mov r11d, 183
    jmp proxy_resolve_common
proxy_winmm_waveOutGetPlaybackRate ENDP

proxy_winmm_waveOutGetPosition PROC
    mov rax, qword ptr [glass_proxy_targets + 1472]
    test rax, rax
    jz proxy_winmm_waveOutGetPosition_resolve
    jmp rax
proxy_winmm_waveOutGetPosition_resolve:
    mov r11d, 184
    jmp proxy_resolve_common
proxy_winmm_waveOutGetPosition ENDP

proxy_winmm_waveOutGetVolume PROC
    mov rax, qword ptr [glass_proxy_targets + 1480]
    test rax, rax
    jz proxy_winmm_waveOutGetVolume_resolve
    jmp rax
proxy_winmm_waveOutGetVolume_resolve:
    mov r11d, 185
    jmp proxy_resolve_common
proxy_winmm_waveOutGetVolume ENDP

proxy_winmm_waveOutMessage PROC
    mov rax, qword ptr [glass_proxy_targets + 1488]
    test rax, rax
    jz proxy_winmm_waveOutMessage_resolve
    jmp rax
proxy_winmm_waveOutMessage_resolve:
    mov r11d, 186
    jmp proxy_resolve_common
proxy_winmm_waveOutMessage ENDP

proxy_winmm_waveOutOpen PROC
    mov rax, qword ptr [glass_proxy_targets + 1496]
    test rax, rax
    jz proxy_winmm_waveOutOpen_resolve
    jmp rax
proxy_winmm_waveOutOpen_resolve:
    mov r11d, 187
    jmp proxy_resolve_common
proxy_winmm_waveOutOpen ENDP

proxy_winmm_waveOutPause PROC
    mov rax, qword ptr [glass_proxy_targets + 1504]
    test rax, rax
    jz proxy_winmm_waveOutPause_resolve
    jmp rax
proxy_winmm_waveOutPause_resolve:
    mov r11d, 188
    jmp proxy_resolve_common
proxy_winmm_waveOutPause ENDP

proxy_winmm_waveOutPrepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 1512]
    test rax, rax
    jz proxy_winmm_waveOutPrepareHeader_resolve
    jmp rax
proxy_winmm_waveOutPrepareHeader_resolve:
    mov r11d, 189
    jmp proxy_resolve_common
proxy_winmm_waveOutPrepareHeader ENDP

proxy_winmm_waveOutReset PROC
    mov rax, qword ptr [glass_proxy_targets + 1520]
    test rax, rax
    jz proxy_winmm_waveOutReset_resolve
    jmp rax
proxy_winmm_waveOutReset_resolve:
    mov r11d, 190
    jmp proxy_resolve_common
proxy_winmm_waveOutReset ENDP

proxy_winmm_waveOutRestart PROC
    mov rax, qword ptr [glass_proxy_targets + 1528]
    test rax, rax
    jz proxy_winmm_waveOutRestart_resolve
    jmp rax
proxy_winmm_waveOutRestart_resolve:
    mov r11d, 191
    jmp proxy_resolve_common
proxy_winmm_waveOutRestart ENDP

proxy_winmm_waveOutSetPitch PROC
    mov rax, qword ptr [glass_proxy_targets + 1536]
    test rax, rax
    jz proxy_winmm_waveOutSetPitch_resolve
    jmp rax
proxy_winmm_waveOutSetPitch_resolve:
    mov r11d, 192
    jmp proxy_resolve_common
proxy_winmm_waveOutSetPitch ENDP

proxy_winmm_waveOutSetPlaybackRate PROC
    mov rax, qword ptr [glass_proxy_targets + 1544]
    test rax, rax
    jz proxy_winmm_waveOutSetPlaybackRate_resolve
    jmp rax
proxy_winmm_waveOutSetPlaybackRate_resolve:
    mov r11d, 193
    jmp proxy_resolve_common
proxy_winmm_waveOutSetPlaybackRate ENDP

proxy_winmm_waveOutSetVolume PROC
    mov rax, qword ptr [glass_proxy_targets + 1552]
    test rax, rax
    jz proxy_winmm_waveOutSetVolume_resolve
    jmp rax
proxy_winmm_waveOutSetVolume_resolve:
    mov r11d, 194
    jmp proxy_resolve_common
proxy_winmm_waveOutSetVolume ENDP

proxy_winmm_waveOutUnprepareHeader PROC
    mov rax, qword ptr [glass_proxy_targets + 1560]
    test rax, rax
    jz proxy_winmm_waveOutUnprepareHeader_resolve
    jmp rax
proxy_winmm_waveOutUnprepareHeader_resolve:
    mov r11d, 195
    jmp proxy_resolve_common
proxy_winmm_waveOutUnprepareHeader ENDP

proxy_winmm_waveOutWrite PROC
    mov rax, qword ptr [glass_proxy_targets + 1568]
    test rax, rax
    jz proxy_winmm_waveOutWrite_resolve
    jmp rax
proxy_winmm_waveOutWrite_resolve:
    mov r11d, 196
    jmp proxy_resolve_common
proxy_winmm_waveOutWrite ENDP

proxy_winmm_ordinal2 PROC
    mov rax, qword ptr [glass_proxy_targets + 1576]
    test rax, rax
    jz proxy_winmm_ordinal2_resolve
    jmp rax
proxy_winmm_ordinal2_resolve:
    mov r11d, 197
    jmp proxy_resolve_common
proxy_winmm_ordinal2 ENDP

END
