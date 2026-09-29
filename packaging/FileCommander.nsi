Unicode true

!ifndef PRODUCT_VERSION
  !error "PRODUCT_VERSION is required"
!endif
!ifndef STAGE_DIR
  !error "STAGE_DIR is required"
!endif
!ifndef OUTFILE
  !error "OUTFILE is required"
!endif
!ifndef ICON_FILE
  !error "ICON_FILE is required"
!endif

Name "FileCommander"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\FileCommander"
InstallDirRegKey HKLM "Software\FileCommander" "InstallDir"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!include "MUI2.nsh"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\FileCommander.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "TradChinese"
!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "Spanish"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "Korean"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "Russian"

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey "ProductName" "FileCommander"
VIAddVersionKey "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "FileDescription" "FileCommander file manager"
VIAddVersionKey "FileVersion" "${PRODUCT_VERSION}.0"
VIAddVersionKey "LegalCopyright" "Copyright (C) FileCommander contributors"

Section "FileCommander" SecMain
  SectionIn RO
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*"

  CreateDirectory "$SMPROGRAMS\FileCommander"
  CreateShortcut "$SMPROGRAMS\FileCommander\FileCommander.lnk" "$INSTDIR\FileCommander.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\FileCommander" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "DisplayName" "FileCommander"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "Publisher" "FileCommander"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "DisplayIcon" "$INSTDIR\FileCommander.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\FileCommander\FileCommander.lnk"
  RMDir "$SMPROGRAMS\FileCommander"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander"
  DeleteRegKey HKLM "Software\FileCommander"
  RMDir /r "$INSTDIR"
SectionEnd
