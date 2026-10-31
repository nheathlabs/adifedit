; adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
; Copyright (C) 2026 Nicholas Heath
; SPDX-License-Identifier: GPL-3.0-or-later
;
; The Windows installer: installs the bundle scripts/build-windows.sh
; builds, for all users or just the current one, registers adifedit for
; .adi and .adif, and writes an uninstaller listed in Apps & features.
; scripts/build-windows.sh runs makensis on it with:
;
;   VERSION     adifedit's version, x.y.z
;   SRCDIR      the repository root (for the icon)
;   BUNDLE      the bundle directory (build/windows/package/adifedit)
;   UNINSTLIST  an include of the bundle's top-level entries, so the
;               uninstaller removes what was installed and nothing else
;   OUTFILE     the setup .exe to write

Unicode true
SetCompressor /SOLID lzma

!define APPNAME "adifedit"
!define PROGID "adifedit.adif"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\adifedit"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 Nicholas Heath"

; Install mode: the user picks all users (Program Files, HKLM, UAC prompt)
; or just themselves ($LOCALAPPDATA\Programs, HKCU, no prompt). MultiUser
; sets SetShellVarContext and SHCTX to match, so the sections below serve
; both. The uninstaller finds the mode from where the Uninstall key is.
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_INSTDIR "${APPNAME}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "${UNINSTKEY}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "InstallMode"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "${UNINSTKEY}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallLocation"
!define MULTIUSER_INSTALLMODE_FUNCTION OnInstallMode
!include MultiUser.nsh
!include MUI2.nsh
!include FileFunc.nsh
!include LogicLib.nsh
!include x64.nsh

!define MUI_ICON "${SRCDIR}/assets/adifedit.ico"
!define MUI_UNICON "${SRCDIR}/assets/adifedit.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${BUNDLE}/share/doc/adifedit/COPYING"
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; A 64-bit program: its keys belong in the 64-bit registry view.
Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "${APPNAME} needs 64-bit Windows."
        Abort
    ${EndIf}
    SetRegView 64
    !insertmacro MULTIUSER_INIT
FunctionEnd

Function un.onInit
    SetRegView 64
    !insertmacro MULTIUSER_UNINIT
FunctionEnd

; MultiUser's per-user default is $LOCALAPPDATA\adifedit; Windows' own
; place for per-user programs is $LOCALAPPDATA\Programs.
Function OnInstallMode
    ${If} $MultiUser.InstallMode == "CurrentUser"
    ${AndIf} $INSTDIR == "$LOCALAPPDATA\${APPNAME}"
        StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${APPNAME}"
    ${EndIf}
FunctionEnd

; Tell Explorer the associations changed (SHCNE_ASSOCCHANGED).
!macro NOTIFY_ASSOC_CHANGED
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
!macroend

; Offer adifedit for an extension: always in its Open with list, and as
; the default only when nothing else has claimed the extension.
!macro REGISTER_EXT EXT
    WriteRegStr SHCTX "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
    ReadRegStr $0 SHCTX "Software\Classes\${EXT}" ""
    ${If} $0 == ""
        WriteRegStr SHCTX "Software\Classes\${EXT}" "" "${PROGID}"
    ${EndIf}
    WriteRegStr SHCTX "Software\Classes\Applications\adifedit.exe\SupportedTypes" "${EXT}" ""
!macroend

!macro UNREGISTER_EXT EXT
    DeleteRegValue SHCTX "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
    DeleteRegKey /ifempty SHCTX "Software\Classes\${EXT}\OpenWithProgids"
    ReadRegStr $0 SHCTX "Software\Classes\${EXT}" ""
    ${If} $0 == "${PROGID}"
        DeleteRegValue SHCTX "Software\Classes\${EXT}" ""
    ${EndIf}
    DeleteRegKey /ifempty SHCTX "Software\Classes\${EXT}"
!macroend

Section "Install"
    SetOutPath "$INSTDIR"
    File /r "${BUNDLE}/*"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\bin\adifedit.exe"

    ; The ProgID .adi and .adif point at.
    WriteRegStr SHCTX "Software\Classes\${PROGID}" "" "ADIF log"
    WriteRegStr SHCTX "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\bin\adifedit.exe,0"
    WriteRegStr SHCTX "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\bin\adifedit.exe" "%1"'
    WriteRegStr SHCTX "Software\Classes\Applications\adifedit.exe" "FriendlyAppName" "${APPNAME}"
    WriteRegStr SHCTX "Software\Classes\Applications\adifedit.exe\shell\open\command" "" '"$INSTDIR\bin\adifedit.exe" "%1"'
    !insertmacro REGISTER_EXT ".adi"
    !insertmacro REGISTER_EXT ".adif"
    !insertmacro NOTIFY_ASSOC_CHANGED

    ; Apps & features.
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayName" "${APPNAME}"
    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr SHCTX "${UNINSTKEY}" "Publisher" "Nicholas Heath"
    WriteRegStr SHCTX "${UNINSTKEY}" "URLInfoAbout" "https://github.com/nheathlabs/adifedit"
    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\bin\adifedit.exe,0"
    WriteRegStr SHCTX "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr SHCTX "${UNINSTKEY}" "InstallMode" "$MultiUser.InstallMode"
    WriteRegStr SHCTX "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe" /$MultiUser.InstallMode'
    WriteRegStr SHCTX "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /$MultiUser.InstallMode /S'
    WriteRegDWORD SHCTX "${UNINSTKEY}" "EstimatedSize" $0
    WriteRegDWORD SHCTX "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD SHCTX "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
    ; Only what the installer put there, in case $INSTDIR is shared.
    !include "${UNINSTLIST}"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APPNAME}.lnk"

    !insertmacro UNREGISTER_EXT ".adi"
    !insertmacro UNREGISTER_EXT ".adif"
    DeleteRegKey SHCTX "Software\Classes\${PROGID}"
    DeleteRegKey SHCTX "Software\Classes\Applications\adifedit.exe"
    !insertmacro NOTIFY_ASSOC_CHANGED

    DeleteRegKey SHCTX "${UNINSTKEY}"
SectionEnd
