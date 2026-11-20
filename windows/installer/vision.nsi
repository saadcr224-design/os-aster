; VISION installer. Build with: makensis vision.nsi  (see build-installer.sh)
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!define APP "VISION"
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef VERSION4
  !define VERSION4 "0.0.0.0"
!endif
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VISION"

Name "${APP}"
OutFile "VISION-Setup.exe"
InstallDir "$PROGRAMFILES64\VISION"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "VISION ${VERSION}"

VIProductVersion "${VERSION4}"
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
!define MUI_WELCOMEPAGE_TEXT "VISION lets several people share this one PC. Each workplace gets its own screen, keyboard and mouse, and all of them use this PC's Windows.$\r$\n$\r$\nOn Windows Server every workplace works like a separate PC at the same time. On Windows 10 and 11 VISION keeps each workplace's windows and mouse on its own screen, and people take turns typing.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "VISION is installed"
!define MUI_FINISHPAGE_TEXT "VISION is set up to start with Windows.$\r$\n$\r$\nOpen VISION once to give each screen its keyboard and mouse, then press Save setup. From then on the workplaces are kept apart automatically."
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

  ; Start with Windows for every user: keeps the saved workplaces apart.
  ; It stays quiet until the workplaces have been set up once.
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "VISION" '"$INSTDIR\VISION.exe" --keep-apart --auto'
  ; Windows Server: give every workplace its own sign-in at each start. That
  ; needs administrator rights, so it runs as a scheduled task at sign-in.
  ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion" "InstallationType"
  StrCmp $0 "Client" +2 0
    nsExec::Exec 'schtasks /Create /F /TN "VISION Workplaces" /SC ONLOGON /RL HIGHEST /TR "\"$INSTDIR\VISION.exe\" --start --auto"'

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
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "VISION"
  nsExec::Exec 'schtasks /Delete /F /TN "VISION Workplaces"'
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
