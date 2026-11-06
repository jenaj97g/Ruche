; Ruche installer. Built by build-windows.sh with: makensis -DVERSION=x.y.z -DSRCDIR=<deployed folder> -DOUTFILE=<exe>
Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin
!include "MUI2.nsh"
!include "FileFunc.nsh"

Name "Ruche ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Ruche"
InstallDirRegKey HKLM "Software\Ruche" "InstallDir"
BrandingText "Ruche ${VERSION}"

!define MUI_ICON "ruche.ico"
!define MUI_UNICON "ruche.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\Ruche.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Ruche"
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "Russian"

!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Ruche"

Section "Ruche" SecMain
  SectionIn RO
  ; a running copy would keep its DLLs locked
  nsExec::Exec 'taskkill /IM Ruche.exe /F'
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortCut "$SMPROGRAMS\Ruche.lnk" "$INSTDIR\Ruche.exe"
  WriteRegStr HKLM "Software\Ruche" "InstallDir" "$INSTDIR"
  ; "Open with" for audio files and playlists
  WriteRegStr HKLM "Software\Classes\Applications\Ruche.exe" "FriendlyAppName" "Ruche"
  WriteRegStr HKLM "Software\Classes\Applications\Ruche.exe\shell\open\command" "" '"$INSTDIR\Ruche.exe" "%1"'
  WriteRegStr HKLM "Software\Classes\Applications\Ruche.exe\DefaultIcon" "" "$INSTDIR\Ruche.exe,0"
  !macro SupportType EXT
    WriteRegStr HKLM "Software\Classes\Applications\Ruche.exe\SupportedTypes" "${EXT}" ""
    WriteRegStr HKLM "Software\Classes\${EXT}\OpenWithList\Ruche.exe" "" ""
  !macroend
  !insertmacro SupportType ".mp3"
  !insertmacro SupportType ".flac"
  !insertmacro SupportType ".ogg"
  !insertmacro SupportType ".opus"
  !insertmacro SupportType ".m4a"
  !insertmacro SupportType ".aac"
  !insertmacro SupportType ".wav"
  !insertmacro SupportType ".wv"
  !insertmacro SupportType ".ape"
  !insertmacro SupportType ".wma"
  !insertmacro SupportType ".m3u"
  !insertmacro SupportType ".m3u8"
  !insertmacro SupportType ".pls"
  WriteRegStr HKLM "${UNINST}" "DisplayName" "Ruche"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher" "Ruche"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\Ruche.exe,0"
  WriteRegStr HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINST}" "EstimatedSize" "$0"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\Ruche.lnk" "$INSTDIR\Ruche.exe"
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM Ruche.exe /F'
  Delete "$SMPROGRAMS\Ruche.lnk"
  Delete "$DESKTOP\Ruche.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Classes\Applications\Ruche.exe"
  !macro UnType EXT
    DeleteRegKey HKLM "Software\Classes\${EXT}\OpenWithList\Ruche.exe"
  !macroend
  !insertmacro UnType ".mp3"
  !insertmacro UnType ".flac"
  !insertmacro UnType ".ogg"
  !insertmacro UnType ".opus"
  !insertmacro UnType ".m4a"
  !insertmacro UnType ".aac"
  !insertmacro UnType ".wav"
  !insertmacro UnType ".wv"
  !insertmacro UnType ".ape"
  !insertmacro UnType ".wma"
  !insertmacro UnType ".m3u"
  !insertmacro UnType ".m3u8"
  !insertmacro UnType ".pls"
  DeleteRegKey HKLM "Software\Ruche"
  DeleteRegKey HKLM "${UNINST}"
  ; your library, stations and settings in %LOCALAPPDATA%\ruche stay
SectionEnd
