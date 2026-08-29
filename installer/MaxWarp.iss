; Inno Setup script for MaxWarp. Built by .github/workflows/release.yml, which
; passes the version in and points SourceDir at the windeployqt staging tree:
;
;   iscc /DMaxWarpVersion=0.1.0 installer\MaxWarp.iss
;
; Everything in dist\MaxWarp\ is installed verbatim — the exe, the Qt runtime
; windeployqt resolved, and SimConnect.dll. There is no separate file list to
; keep in step with the build.

#ifndef MaxWarpVersion
  #define MaxWarpVersion "0.0.0"
#endif

[Setup]
AppId={{9F2C4A61-3E7D-4B18-9C55-1D0A6E8B2F34}
AppName=MaxWarp
AppVersion={#MaxWarpVersion}
AppVerName=MaxWarp {#MaxWarpVersion}
AppPublisher=MaxWarp
AppSupportURL=https://github.com/messpace/MaxWarp-Release
DefaultDirName={autopf}\MaxWarp
DefaultGroupName=MaxWarp
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=MaxWarp-v{#MaxWarpVersion}-win-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName=MaxWarp {#MaxWarpVersion}
UninstallDisplayIcon={app}\MaxWarp.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "..\dist\MaxWarp\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\MaxWarp"; Filename: "{app}\MaxWarp.exe"
Name: "{group}\Uninstall MaxWarp"; Filename: "{uninstallexe}"
Name: "{autodesktop}\MaxWarp"; Filename: "{app}\MaxWarp.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\MaxWarp.exe"; Description: "Launch MaxWarp"; Flags: nowait postinstall skipifsilent
