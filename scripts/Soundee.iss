#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef PackageDir
  #error PackageDir is required
#endif
#ifndef OutputDirPath
  #error OutputDirPath is required
#endif

[Setup]
AppId={{81D6F6DB-AB9D-49DC-9888-CA839AD90C10}
AppName=Soundee
AppVersion={#AppVersion}
AppPublisher=JustApeasantCoder
AppPublisherURL=https://github.com/JustApeasantCoder/Soundee
AppSupportURL=https://github.com/JustApeasantCoder/Soundee/issues
DefaultDirName={localappdata}\Programs\Soundee
DefaultGroupName=Soundee
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
OutputDir={#OutputDirPath}
OutputBaseFilename=Soundee-{#AppVersion}-win64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Soundee.exe
CloseApplications=yes
RestartApplications=no
LicenseFile={#PackageDir}\DISTRIBUTION.txt
InfoBeforeFile=installer-info.txt

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Soundee"; Filename: "{app}\Soundee.exe"
Name: "{group}\Soundee User Guide"; Filename: "{app}\user-guide.txt"
Name: "{autodesktop}\Soundee"; Filename: "{app}\Soundee.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Soundee.exe"; Description: "Launch Soundee"; Flags: nowait postinstall skipifsilent

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  StartupCommand: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Soundee', StartupCommand) then
      if CompareText(StartupCommand, '"' + ExpandConstant('{app}\Soundee.exe') + '" --background') = 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Soundee');
  end;
end;
