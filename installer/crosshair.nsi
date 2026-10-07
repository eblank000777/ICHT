; Crosshair installer (NSIS 3). Build with installer\build_installer.bat, or run
; "makensis crosshair.nsi" in this folder after building ..\crosshair.exe.
;
; Installs for the current user only (no administrator rights needed) to
; %LOCALAPPDATA%\Programs\Crosshair, so the app can keep its presets next to the exe.

Target amd64-unicode   ; 64-bit installer, like crosshair.exe itself
SetCompressor /SOLID lzma
RequestExecutionLevel user
ManifestDPIAware true

!define APP_NAME    "Crosshair"
!define APP_VERSION "2.2.0"
!define APP_EXE     "crosshair.exe"
!define UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\Crosshair"
!define RUN_KEY     "Software\Microsoft\Windows\CurrentVersion\Run"
!define WM_APP_QUIT 0x8003   ; asks a running crosshair.exe to quit (WM_APP + 3 in crosshair.cpp)

!pragma warning disable 9100   ; no LegalCopyright version key

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

Name "${APP_NAME}"
OutFile "../CrosshairSetup.exe"
InstallDir "$LOCALAPPDATA\Programs\Crosshair"
InstallDirRegKey HKCU "Software\Crosshair" "InstallDir"
BrandingText "${APP_NAME} ${APP_VERSION}"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} setup"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"

!define MUI_ICON "../crosshair.ico"
!define MUI_UNICON "../crosshair.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_WELCOMEPAGE_TEXT "This will install ${APP_NAME} ${APP_VERSION}, a pixel-precise crosshair overlay with a built-in editor.$\r$\n$\r$\nNo administrator rights are needed. If ${APP_NAME} is already running it is closed first. Existing presets and settings are kept."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME} now"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Ask a running copy to quit (it may offer to save unsaved presets), then wait for it to go.
!macro CLOSE_RUNNING UN
Function ${UN}CloseRunning
    StrCpy $1 0
    loop:
        FindWindow $0 "CrosshairEditorWnd"
        StrCmp $0 0 done
        ${If} $1 = 0
            SendMessage $0 ${WM_APP_QUIT} 0 0 /TIMEOUT=2000
        ${EndIf}
        Sleep 250
        IntOp $1 $1 + 1
        ${If} $1 > 40
            MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${APP_NAME} is still running. Quit it from its tray icon, then press Retry." /SD IDCANCEL IDRETRY retry
            Abort
            retry:
            StrCpy $1 0
        ${EndIf}
        Goto loop
    done:
FunctionEnd
!macroend
!insertmacro CLOSE_RUNNING ""
!insertmacro CLOSE_RUNNING "un."

Section "${APP_NAME}" SecApp
    SectionIn RO
    Call CloseRunning
    SetOutPath "$INSTDIR"
    File "../crosshair.exe"
    File "../crosshair.ico"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; the optional sections below re-create these when they are selected
    Delete "$SMPROGRAMS\${APP_NAME}.lnk"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    DeleteRegValue HKCU "${RUN_KEY}" "${APP_NAME}"

    WriteRegStr HKCU "Software\Crosshair" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Start menu shortcut" SecStartMenu
    CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

Section /o "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

Section /o "Start with Windows (in the tray)" SecAutostart
    WriteRegStr HKCU "${RUN_KEY}" "${APP_NAME}" '"$INSTDIR\${APP_EXE}" /tray'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "The crosshair overlay and editor (required)."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Add ${APP_NAME} to the Start menu."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Put a ${APP_NAME} shortcut on the desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} "Start ${APP_NAME} when you sign in, with the editor hidden in the tray."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    Call un.CloseRunning
    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\crosshair.ico"
    Delete "$INSTDIR\uninstall.exe"
    Delete "$SMPROGRAMS\${APP_NAME}.lnk"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    DeleteRegValue HKCU "${RUN_KEY}" "${APP_NAME}"
    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "Software\Crosshair"
    MessageBox MB_YESNO|MB_ICONQUESTION "Also delete your crosshair presets and settings?" /SD IDNO IDNO keep
        RMDir /r "$INSTDIR\presets"
        Delete "$INSTDIR\crosshair.ini"
        Delete "$INSTDIR\crosshair.ini.bak"
        RMDir /r "$APPDATA\Crosshair"
    keep:
    RMDir "$INSTDIR"
SectionEnd
