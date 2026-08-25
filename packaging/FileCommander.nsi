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

Name "FileCommander"
Caption "FileCommander ${PRODUCT_VERSION} Setup"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\FileCommander"
InstallDirRegKey HKCU "Software\FileCommander" "InstallDir"
RequestExecutionLevel user
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey "ProductName" "FileCommander"
VIAddVersionKey "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "FileDescription" "FileCommander file manager"
VIAddVersionKey "FileVersion" "${PRODUCT_VERSION}.0"
VIAddVersionKey "LegalCopyright" "Copyright (C) FileCommander contributors"

Section "FileCommander" SecMain
  SectionIn RO
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*"

  CreateDirectory "$SMPROGRAMS\FileCommander"
  CreateShortcut "$SMPROGRAMS\FileCommander\FileCommander.lnk" "$INSTDIR\FileCommander.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\FileCommander" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "DisplayName" "FileCommander"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "Publisher" "FileCommander"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  Delete "$SMPROGRAMS\FileCommander\FileCommander.lnk"
  RMDir "$SMPROGRAMS\FileCommander"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FileCommander"
  DeleteRegKey HKCU "Software\FileCommander"
  RMDir /r "$INSTDIR"
SectionEnd
