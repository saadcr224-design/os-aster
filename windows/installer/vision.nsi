; VISION installer. Build with: makensis vision.nsi  (see build-installer.sh)
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!define APP "VISION"
!define VERSION "1.0.0"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VISION"

Name "${APP}"
OutFile "VISION-Setup.exe"
InstallDir "$PROGRAMFILES64\VISION"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "VISION ${VERSION}"

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName" "VISION"
VIAddVersionKey "FileDescription" "VISION Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "..\openseat\brand\vision.ico"
!define MUI_UNICON "..\openseat\brand\vision.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Welcome to VISION"
!define MUI_WELCOMEPAGE_TEXT "VISION lets several people use this one PC at the same time. Each person gets their own screen, keyboard and mouse.$\r$\n$\r$\nRunning several people at once needs Windows Server 2016 or newer. On Windows 10 and 11 you can set everything up, but only one person can be signed in at a time.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "VISION is installed"
!define MUI_FINISHPAGE_TEXT "Open VISION from the Start menu or the desktop icon.$\r$\n$\r$\nThe first Start asks to add a free input driver and to restart the PC once."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open VISION now"
!define MUI_FINISHPAGE_RUN_FUNCTION OpenVision
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

; Open it as the normal user, not with the installer's administrator rights.
Function OpenVision
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\VISION.exe"'
FunctionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "VISION needs a 64-bit version of Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Section "VISION" SecMain
  SectionIn RO
  SetShellVarContext all
  nsExec::Exec 'taskkill /F /IM VISION.exe'
  SetOutPath "$INSTDIR"
  File "VISION.exe"
  File /oname=LICENSE.txt "..\..\LICENSE"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\VISION"
  CreateShortcut "$SMPROGRAMS\VISION\VISION.lnk" "$INSTDIR\VISION.exe"
  CreateShortcut "$SMPROGRAMS\VISION\Uninstall VISION.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\VISION.lnk" "$INSTDIR\VISION.exe"

  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "VISION"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "VISION"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\VISION.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 800
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  nsExec::Exec 'taskkill /F /IM VISION.exe'
  Sleep 500
  Delete "$INSTDIR\VISION.exe"
  Delete "$INSTDIR\interception.dll"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\VISION\VISION.lnk"
  Delete "$SMPROGRAMS\VISION\Uninstall VISION.lnk"
  RMDir "$SMPROGRAMS\VISION"
  Delete "$DESKTOP\VISION.lnk"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "VISIONAgent"
  DeleteRegKey HKLM "${UNINST_KEY}"

  ; The hidden accounts hold each workplace's files, so ask before removing them.
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also remove the hidden Windows accounts VISION made for the workplaces?$\r$\n$\r$\nPeople who used those workplaces can no longer sign in to them. Choose No to keep them." /SD IDNO IDNO done
  StrCpy $0 2
  loop:
    nsExec::Exec 'net user VISION$0 /delete'
    DeleteRegValue HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList" "VISION$0"
    IntOp $0 $0 + 1
    IntCmp $0 10 loop loop done
  done:
SectionEnd
