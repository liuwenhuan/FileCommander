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
InstallDir "$PROGRAMFILES64\FileCommander"
InstallDirRegKey HKLM "Software\FileCommander" "InstallDir"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

!include "MUI2.nsh"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to FileCommander Setup"
!define MUI_WELCOMEPAGE_TEXT "FileCommander is a dual-pane file manager for local files, archives, network shares, and document previews.$\r$\n$\r$\nThis wizard will install FileCommander for all users of this computer."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder where FileCommander will be installed."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Install FileCommander to:"
!define MUI_FINISHPAGE_RUN "$INSTDIR\FileCommander.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch FileCommander"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

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
