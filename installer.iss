; TNxPilot installer — Inno Setup script (original, no third-party branding).
; Build on Windows after cl.exe:  iscc installer.iss
; Requires TNxPilot.exe (single static-CRT EXE) in this folder.
; The installer is fully OFFLINE: the EXE is bundled inside
; TNxPilot-Setup-v1.0.exe — no download at install time.
#define MyAppName "TNxPilot"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "TNxPilot"

[Setup]
AppId={{7E3A1B2C-9D4F-4A6B-8C1E-0123456789AB}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppVerName={#MyAppName} {#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=installer-output
OutputBaseFilename=TNxPilot-Setup-v1.0
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayName={#MyAppName} {#MyAppVersion}
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "TNxPilot.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\TNxPilot.exe"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\TNxPilot.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\TNxPilot.exe"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
