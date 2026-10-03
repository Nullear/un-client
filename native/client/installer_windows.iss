#define ProductName "Un Client"
#define ProductVersion "1.0.0"
#define PublisherName "Un Client"
#define PackageDir AddBackslash(SourcePath) + "..\..\build\windows-encrypted"

[Setup]
AppId={{AFA41B0D-4897-4A0C-A9C5-61A626677047}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppPublisher={#PublisherName}
DefaultDirName={localappdata}\Programs\Un Client
DefaultGroupName={#ProductName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
SetupIconFile={#SetupIconPath}
UninstallDisplayIcon={app}\UnFalsusOnline.exe
OutputDir={#SourcePath}\..\..\build
OutputBaseFilename=Un-Client-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimp"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加快捷方式："; Flags: unchecked

[Files]
Source: "{#PackageDir}\UnFalsusOnline.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\client.bin"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#ProductName}"; Filename: "{app}\UnFalsusOnline.exe"
Name: "{autodesktop}\{#ProductName}"; Filename: "{app}\UnFalsusOnline.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\UnFalsusOnline.exe"; Description: "启动 {#ProductName}"; Flags: postinstall nowait skipifsilent
