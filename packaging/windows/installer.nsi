Unicode true
!include "MUI2.nsh"
!include "WinVer.nsh"
!include "x64.nsh"
!ifndef VERSION
  !error "VERSION required"
!endif
!ifndef ARCH
  !error "ARCH required"
!endif
!ifndef STAGE
  !error "STAGE required"
!endif
!ifndef OUTPUT
  !error "OUTPUT required"
!endif
Name "Rose Agent ${VERSION} (${ARCH})"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Rose Agent-${ARCH}"
RequestExecutionLevel user
ShowInstDetails show
ShowUninstDetails show
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Function .onInit
!if "${ARCH}" == "x86"
  ${IfNot} ${AtLeastWin8.1}
    MessageBox MB_ICONSTOP "This x86 build requires Windows 8.1 or newer."
    Abort
  ${EndIf}
!else
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This x64 build requires 64-bit Windows. Use the x86 installer instead."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "This Qt6 x64 build requires Windows 10 version 1809 or newer. Use the legacy x86 installer on Windows 8.1."
    Abort
  ${EndIf}
  SetRegView 64
  ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion" "CurrentBuildNumber"
  SetRegView 32
  ${If} $0 < 17763
    MessageBox MB_ICONSTOP "This Qt6 x64 build requires Windows build 17763 or newer (Windows 10 version 1809)."
    Abort
  ${EndIf}
!endif
FunctionEnd
Section "Rose Agent" SEC_APP
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Rose Agent (${ARCH})"
  CreateShortcut "$SMPROGRAMS\Rose Agent (${ARCH})\Rose Agent.lnk" "$INSTDIR\rose-agent.exe"
  CreateShortcut "$SMPROGRAMS\Rose Agent (${ARCH})\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "DisplayName" "Rose Agent (${ARCH})"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "Publisher" "Rose Agent contributors"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "URLInfoAbout" "https://github.com/alertxsto/rose-agent"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "UninstallString" '$"$INSTDIR\Uninstall.exe$"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}" "NoRepair" 1
SectionEnd
Section "Uninstall"
  SetShellVarContext current
  # Generated exact package file list. Never recursively remove a user's .mdl,
  # API key, settings, or other file saved in/near the installation directory.
  !include "${STAGE}\uninstall-files.nsh"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Rose Agent (${ARCH})\Rose Agent.lnk"
  Delete "$SMPROGRAMS\Rose Agent (${ARCH})\Uninstall.lnk"
  RMDir "$SMPROGRAMS\Rose Agent (${ARCH})"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RoseAgent-${ARCH}"
SectionEnd
