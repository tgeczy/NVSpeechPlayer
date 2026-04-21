; TGSpeechSapi.iss - dual-arch (x86 + x64) SAPI5 installer
; Since v3.0: TGSpeechSapi.dll is statically linked — only one DLL per arch.
; Staging layout expected:
;   ..\release\x64\TGSpeechSapi.dll
;   ..\release\x86\TGSpeechSapi.dll
;   ..\release\espeak-ng-data\...
;   ..\release\packs\...
;   ..\release\TGSpeechSapiSettings.exe

[Setup]
AppName=TGSpeechBox SAPI Voice
AppVersion=3.10b2
AppPublisher=Tamas Geczy
AppPublisherURL=https://github.com/tgeczy/TGSpeechBox
DefaultDirName={autopf}\TGSpeechSapi
DefaultGroupName=TGSpeechBox
UninstallDisplayIcon={app}\x64\TGSpeechSapi.dll
UninstallDisplayName=TGSpeechBox SAPI Voice
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=Output
OutputBaseFilename=TGSpeechSapiSetup

[Files]
; --- x64 binary (only on 64-bit Windows) ---
Source: "..\release\x64\TGSpeechSapi.dll";   DestDir: "{app}\x64"; Flags: ignoreversion; Check: IsWin64

; --- x86 binary (installed on both 32-bit and 64-bit Windows) ---
Source: "..\release\x86\TGSpeechSapi.dll";   DestDir: "{app}\x86"; Flags: ignoreversion

; --- shared data (one copy) ---
Source: "..\release\espeak-ng-data\*";             DestDir: "{app}\espeak-ng-data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\release\packs\*";                      DestDir: "{app}\packs";          Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\release\TGSpeechSapiSettings.exe";     DestDir: "{app}";                Flags: ignoreversion
Source: "..\release\notice.txt";     DestDir: "{app}";                Flags: ignoreversion
Source: "..\release\LICENSE-GPL3.txt";     DestDir: "{app}";                Flags: ignoreversion

[Tasks]
Name: "startmenu_settings"; Description: "Add 'TGSpeechBox Settings' to my Start Menu"; GroupDescription: "Shortcuts:"; Flags: checkedonce

[Icons]
Name: "{group}\TGSpeechBox Settings";   Filename: "{app}\TGSpeechSapiSettings.exe"; Tasks: startmenu_settings
Name: "{group}\Uninstall TGSpeechBox";  Filename: "{uninstallexe}"

[Run]
; Register 64-bit COM server (64-bit regsvr32)
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\x64\TGSpeechSapi.dll"""; Flags: runhidden; Check: IsWin64

; Register 32-bit COM server (32-bit regsvr32 lives in SysWOW64 on 64-bit Windows)
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\x86\TGSpeechSapi.dll"""; Flags: runhidden; Check: IsWin64

; On 32-bit Windows, {sys}\regsvr32.exe is the 32-bit one
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\x86\TGSpeechSapi.dll"""; Flags: runhidden; Check: not IsWin64

; Launch settings app after install (user choice)
Filename: "{app}\TGSpeechSapiSettings.exe"; Description: "Open TGSpeechBox SAPI Settings"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\x64\TGSpeechSapi.dll"""; Flags: runhidden; Check: IsWin64
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/u /s ""{app}\x86\TGSpeechSapi.dll"""; Flags: runhidden; Check: IsWin64
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\x86\TGSpeechSapi.dll"""; Flags: runhidden; Check: not IsWin64

[Code]
const
  OldUninstaller = 'C:\Program Files\NVSpeechSapi\unins000.exe';

function OldProductInstalled: Boolean;
begin
  Result := FileExists(OldUninstaller);
end;

function InitializeSetup: Boolean;
var
  ResultCode: Integer;
begin
  Result := True;

  if OldProductInstalled then
  begin
    if MsgBox(
      'The older NV Speech Player SAPI engine was detected on this system.' + #13#10 + #13#10 +
      'It is recommended to remove it first to avoid having two speech engines registered.' + #13#10 + #13#10 +
      'Would you like to uninstall the old NV Speech Player SAPI now?',
      mbConfirmation, MB_YESNO) = IDYES then
    begin
      Exec(OldUninstaller, '/SILENT', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
      if (ResultCode <> 0) and FileExists(OldUninstaller) then
        MsgBox(
          'The old uninstaller exited with code ' + IntToStr(ResultCode) + '.' + #13#10 +
          'You may need to remove it manually from Add/Remove Programs.' + #13#10 + #13#10 +
          'TGSpeechBox installation will continue.',
          mbInformation, MB_OK);
    end;
  end;
end;

procedure InitializeWizard;
var
  FunPage: TOutputMsgWizardPage;
begin
  FunPage := CreateOutputMsgPage(
    wpWelcome,
    'A Note from the Developer',
    'Important info regarding your awesomeness',
    'You''re an awesome human being for wanting to try this text-to-speech engine!' + #13#10 + #13#10 +
    'TGSpeechBox is a community-built formant speech synthesizer.' + #13#10 +
    'Original engine by NVAccess, continued development by Tamas Geczy.' + #13#10 + #13#10 +
    'Report bugs at: https://github.com/tgeczy/TGSpeechBox' + #13#10 + #13#10 +
    'Hats off to you for wanting to use a lovingly crafted, community-built speech synthesizer!'
  );
end;
