; Zero Hour Controller Mod - Setup program (Inno Setup 6).
;
; Adds the controller to the player's Command & Conquer Generals Zero Hour 1.04 folder, exactly like
; the zip's Install.cmd (the zip stays as the manual option):
;   <game folder>\generalszh.exe      the controller game program
;   <game folder>\ZH Controller\      readme, controls, license, version, and the uninstaller
; Nothing of the game is replaced, renamed or changed. Uninstall: Settings > Apps, or the Start menu.
;
; Safety rules (same as Install-ZHController.ps1):
;   - the chosen folder must be Zero Hour (INIZH.big, WindowZH.big, MapsZH.big); the EA App layout
;     (registry one folder up) is found automatically
;   - a different generalszh.exe that is already there (another community build) is never replaced
;   - a "ZH Controller" folder that neither installer made stops Setup
;   - an install made with the zip's Install.cmd is taken over: its recorded files are removed first
;     (only if unchanged)
;   - the uninstaller removes generalszh.exe only if it is still the one Setup added
;   - an update replaces generalszh.exe only if it is a version this mod installed (the hash Setup
;     recorded in ZH Controller\ZHController.setup.txt, a known earlier release, or the zip
;     install's record); uninstall files alone do not prove it
;
; Built by ControllerMod\scripts\package.ps1, which passes:
;   /DAppVersion=1.0.2  /DSourceDir=<staged package folder>  /DOutputDir=<folder>  /DExeHash=<sha256>

#ifndef AppVersion
  #error Build with package.ps1 (AppVersion is not set)
#endif

; The Setup tests build a copy with their own identity (/DAppGuid, /DAppName, /DShortcutName), so a
; test never touches a real install's Apps entry, Start menu group or shortcuts. Releases use these
; defaults, which must never change (updates find the installed version by them).
#ifndef AppGuid
  #define AppGuid "6E0A3C52-8F1B-4C7D-9B2E-5A4D7C1E9F30"
#endif
#ifndef AppName
  #define AppName "Zero Hour Controller Mod"
#endif
#ifndef ShortcutName
  #define ShortcutName "Zero Hour Controller"
#endif
#define GameSubfolder "Command and Conquer Generals Zero Hour"
#define CtrlDir "ZH Controller"

[Setup]
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Zero Hour Controller Mod (on the Community Patch by TheSuperHackers)
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} Setup
DefaultDirName={code:GetDefaultGameDir}
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
DefaultGroupName={#ShortcutName}
UninstallFilesDir={app}\{#CtrlDir}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\generalszh.exe
; The game is usually in Program Files. /CURRENTUSER is for the automated tests on scratch folders.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
LicenseFile={#SourceDir}\LICENSE.txt
SetupIconFile=..\..\GeneralsMD\Code\Main\Generals.ico
WizardStyle=modern
OutputDir={#OutputDir}
OutputBaseFilename=ZHController-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x86compatible
CloseApplications=yes
; File dates are stored as UTC (the package's are the UTC build time), so Setup carries no time zone.
TimeStampsInUTC=yes
RestartApplications=no

[Messages]
SelectDirLabel3=Setup will add the controller mod to your Zero Hour game folder. Nothing of the game is replaced or changed.
SelectDirBrowseLabel=This is the folder with INIZH.big in it. For the EA App it is usually ...\EA Games\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour. Click Next to continue, or Browse to pick another folder.
SelectDirDesc=Where is Command & Conquer Generals Zero Hour?
WizardSelectDir=Select your Zero Hour folder

[Tasks]
Name: desktopicon; Description: "Make a desktop shortcut"; GroupDescription: "Shortcuts:"

[Dirs]
; Also removed (when empty) if a zip install made it before Setup took over.
Name: "{app}\{#CtrlDir}"; Flags: uninsalwaysuninstall

[UninstallDelete]
Type: files; Name: "{app}\{#CtrlDir}\ZHController.setup.txt"

[Files]
; generalszh.exe: removed by the uninstaller's own check (only if unchanged), see [Code].
Source: "{#SourceDir}\generalszh.exe"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall
Source: "{#SourceDir}\README.txt"; DestDir: "{app}\{#CtrlDir}"; Flags: ignoreversion
Source: "{#SourceDir}\CONTROLS.txt"; DestDir: "{app}\{#CtrlDir}"; Flags: ignoreversion
Source: "{#SourceDir}\RELEASE_NOTES.txt"; DestDir: "{app}\{#CtrlDir}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}\{#CtrlDir}"; Flags: ignoreversion
Source: "{#SourceDir}\VERSION.txt"; DestDir: "{app}\{#CtrlDir}"; Flags: ignoreversion

[Icons]
Name: "{autodesktop}\{#ShortcutName}"; Filename: "{app}\generalszh.exe"; WorkingDir: "{app}"; Comment: "Command & Conquer Generals Zero Hour with the controller mod"; Tasks: desktopicon
Name: "{group}\{#ShortcutName}"; Filename: "{app}\generalszh.exe"; WorkingDir: "{app}"; Comment: "Command & Conquer Generals Zero Hour with the controller mod"
Name: "{group}\Controls"; Filename: "{app}\{#CtrlDir}\CONTROLS.txt"
Name: "{group}\Read me"; Filename: "{app}\{#CtrlDir}\README.txt"
Name: "{group}\Uninstall the controller mod"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#CtrlDir}\CONTROLS.txt"; Description: "Show the controls"; Flags: postinstall shellexec skipifsilent unchecked runasoriginaluser
Filename: "{app}\generalszh.exe"; WorkingDir: "{app}"; Description: "Play now"; Flags: postinstall nowait skipifsilent unchecked runasoriginaluser

[Code]
const
  ExeHash = '{#ExeHash}';
  RecordName = 'ZHController.install.txt';
  OwnedName = 'ZHController.setup.txt';
  { generalszh.exe of earlier Setup releases: 1.0.2 (before OwnedName existed) and 1.0.3. }
  KnownSetupExeHashes = '25F8EB832359FC52C5DBA3F2A52F2E3E47D3CAE0FF3A3B9B6595C8818E470568 D44FACA931A3A17F573EAF60D549C0E67D0DF164DD9418AEA7ED82EA9BAD18CC';
  RegSub = 'SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour';

function IsZeroHourFolder(const Dir: String): Boolean;
begin
  Result := FileExists(AddBackslash(Dir) + 'INIZH.big') and FileExists(AddBackslash(Dir) + 'WindowZH.big')
    and FileExists(AddBackslash(Dir) + 'MapsZH.big');
end;

{ The folder itself, or the EA App subfolder with the same name; '' if neither is Zero Hour. }
function ResolveGameDir(const Dir: String): String;
var
  D: String;
begin
  Result := '';
  D := RemoveBackslashUnlessRoot(Trim(Dir));
  if D = '' then Exit;
  if IsZeroHourFolder(D) then
    Result := D
  else if IsZeroHourFolder(AddBackslash(D) + '{#GameSubfolder}') then
    Result := AddBackslash(D) + '{#GameSubfolder}';
end;

function RegGameDir(const RootKey: Integer): String;
var
  P: String;
begin
  Result := '';
  if RegQueryStringValue(RootKey, RegSub, 'InstallPath', P) then Result := ResolveGameDir(P);
end;

function GetDefaultGameDir(Param: String): String;
begin
  Result := RegGameDir(HKLM32);
  if (Result = '') and IsWin64 then Result := RegGameDir(HKLM64);
  if (Result = '') and IsWin64 then Result := ExpandConstant('{commonpf64}\EA Games\{#GameSubfolder}\{#GameSubfolder}');
  if Result = '' then Result := ExpandConstant('{commonpf32}\EA Games\{#GameSubfolder}\{#GameSubfolder}');
end;

function SameHash(const FileName, Hash: String): Boolean;
begin
  Result := (Hash <> '') and FileExists(FileName) and (CompareText(GetSHA256OfFile(FileName), Hash) = 0);
end;

{ Parses the zip installer's record: key=value lines; File=<relative path>|<sha256>. }
function ReadZipRecord(const GameDir: String; var Files: TArrayOfString): Boolean;
var
  Lines: TArrayOfString;
  I, N: Integer;
  RootOk, IsOurs: Boolean;
begin
  Result := False;
  SetArrayLength(Files, 0);
  if not LoadStringsFromFile(AddBackslash(GameDir) + '{#CtrlDir}\' + RecordName, Lines) then Exit;
  RootOk := False;
  IsOurs := False;
  N := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    if Lines[I] = 'ZHControllerInstall=1' then IsOurs := True;
    if CompareText(Lines[I], 'GameRoot=' + RemoveBackslashUnlessRoot(GameDir)) = 0 then RootOk := True;
    if Copy(Lines[I], 1, 5) = 'File=' then begin
      SetArrayLength(Files, N + 1);
      Files[N] := Copy(Lines[I], 6, Length(Lines[I]));
      N := N + 1;
    end;
  end;
  Result := IsOurs and RootOk;
end;

{ Is this generalszh.exe the one a zip install recorded? }
function ZipRecordedExe(const GameDir: String): Boolean;
var
  Files: TArrayOfString;
  I, Bar: Integer;
begin
  Result := False;
  if not ReadZipRecord(GameDir, Files) then Exit;
  for I := 0 to GetArrayLength(Files) - 1 do begin
    Bar := Pos('|', Files[I]);
    if (Bar > 0) and (CompareText(Copy(Files[I], 1, Bar - 1), 'generalszh.exe') = 0) then
      Result := SameHash(AddBackslash(GameDir) + 'generalszh.exe', Copy(Files[I], Bar + 1, Length(Files[I])));
  end;
end;

function SetupInstalledHere(const GameDir: String): Boolean;
begin
  Result := FileExists(AddBackslash(GameDir) + '{#CtrlDir}\unins000.dat');
end;

{ Is the generalszh.exe in GameDir one that Setup installed? Checked by its content: the hash an
  earlier Setup recorded, or a known earlier Setup release. }
function SetupOwnsExe(const GameDir: String): Boolean;
var
  Exe, Hash: String;
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := False;
  Exe := AddBackslash(GameDir) + 'generalszh.exe';
  if not SetupInstalledHere(GameDir) or not FileExists(Exe) then Exit;
  Hash := GetSHA256OfFile(Exe);
  if Pos(Uppercase(Hash), KnownSetupExeHashes) > 0 then begin
    Result := True;
    Exit;
  end;
  if LoadStringsFromFile(AddBackslash(GameDir) + '{#CtrlDir}\' + OwnedName, Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if CompareText(Trim(Lines[I]), 'ExeSHA256=' + Hash) = 0 then Result := True;
end;

{ Every file the zip installer has ever added (all versions), as Install-ZHController.ps1 lists them.
  A zip record may name nothing else. }
function KnownZipFile(const Rel: String): Boolean;
begin
  Result := (CompareText(Rel, 'generalszh.exe') = 0) or
    (CompareText(Rel, '{#CtrlDir}\README.txt') = 0) or (CompareText(Rel, '{#CtrlDir}\CONTROLS.txt') = 0) or
    (CompareText(Rel, '{#CtrlDir}\RELEASE_NOTES.txt') = 0) or (CompareText(Rel, '{#CtrlDir}\LICENSE.txt') = 0) or
    (CompareText(Rel, '{#CtrlDir}\VERSION.txt') = 0) or (CompareText(Rel, '{#CtrlDir}\Uninstall-ZHController.ps1') = 0) or
    (CompareText(Rel, '{#CtrlDir}\Uninstall.cmd') = 0);
end;

function IsSha256(const S: String): Boolean;
var
  I: Integer;
begin
  Result := Length(S) = 64;
  for I := 1 to Length(S) do
    if Pos(Uppercase(S[I]), '0123456789ABCDEF') = 0 then Result := False;
end;

{ '' when there is no zip record or every entry in it is a known file with a SHA-256; else the reason.
  The whole record is checked before Setup changes anything. }
function ZipRecordProblem(const GameDir: String): String;
var
  Files: TArrayOfString;
  I, Bar: Integer;
begin
  Result := '';
  if not ReadZipRecord(GameDir, Files) then Exit;
  for I := 0 to GetArrayLength(Files) - 1 do begin
    Bar := Pos('|', Files[I]);
    if (Bar = 0) or not KnownZipFile(Copy(Files[I], 1, Bar - 1)) or not IsSha256(Copy(Files[I], Bar + 1, Length(Files[I]))) then begin
      Result := 'The record of the earlier install (' + '{#CtrlDir}\' + RecordName + ') lists something this mod never installs:' + #13#10 +
        Files[I] + #13#10#13#10 + 'Setup did not change anything. Uninstall the earlier version with its Uninstall.cmd first.';
      Exit;
    end;
  end;
end;

{ The folder itself is a link or junction (its files would land somewhere else). }
function IsLinkFolder(const Dir: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(Dir, FindRec) then begin
    try
      Result := (FindRec.Attributes and $400) <> 0;   { FILE_ATTRIBUTE_REPARSE_POINT }
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ '' when Setup may install into GameDir, else the reason it may not. }
function CheckGameDir(const GameDir: String): String;
var
  Ctrl, Exe: String;
  Dummy: TArrayOfString;
  FindRec: TFindRec;
  HasOther: Boolean;
begin
  Result := '';
  if not IsZeroHourFolder(GameDir) then begin
    Result := 'This does not look like the Zero Hour folder (INIZH.big is missing):' + #13#10 + GameDir + #13#10#13#10 +
      'Please pick the folder that has INIZH.big, WindowZH.big and MapsZH.big in it.';
    Exit;
  end;
  if Pos('\\', GameDir) = 1 then begin
    Result := 'Please use a game folder on a drive letter, not a network path.';
    Exit;
  end;
  Ctrl := AddBackslash(GameDir) + '{#CtrlDir}';
  if DirExists(Ctrl) and IsLinkFolder(Ctrl) then begin
    Result := 'The folder "' + Ctrl + '" is a link or junction to somewhere else. Setup did not change anything.';
    Exit;
  end;
  Result := ZipRecordProblem(GameDir);
  if Result <> '' then Exit;
  if DirExists(Ctrl) and not SetupInstalledHere(GameDir) and not ReadZipRecord(GameDir, Dummy) then begin
    HasOther := False;
    if FindFirst(Ctrl + '\*', FindRec) then begin
      try
        repeat
          if (FindRec.Name <> '.') and (FindRec.Name <> '..') then HasOther := True;
        until HasOther or not FindNext(FindRec);
      finally
        FindClose(FindRec);
      end;
    end;
    if HasOther then begin
      Result := 'The folder "' + Ctrl + '" already exists and was not made by this mod. Setup did not change anything.';
      Exit;
    end;
  end;
  Exe := AddBackslash(GameDir) + 'generalszh.exe';
  if FileExists(Exe) and not SameHash(Exe, ExeHash) and not SetupOwnsExe(GameDir) and not ZipRecordedExe(GameDir) then
    Result := 'There is already a different generalszh.exe in your game folder (perhaps another community build).' + #13#10 +
      'Setup will not replace it. Rename or move it first if you want this one.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Resolved, Problem: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    Resolved := ResolveGameDir(WizardDirValue);
    if (Resolved <> '') and (CompareText(Resolved, RemoveBackslashUnlessRoot(WizardDirValue)) <> 0) then
      WizardForm.DirEdit.Text := Resolved;   { the EA App layout: use the inner folder }
    Problem := CheckGameDir(RemoveBackslashUnlessRoot(WizardDirValue));
    if Problem <> '' then begin
      MsgBox(Problem, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ Also runs for silent installs, which skip the pages. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := CheckGameDir(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
end;

{ An install made with the zip's Install.cmd: remove its recorded files (only if unchanged) and its
  record, so Setup owns the install from now on. generalszh.exe is replaced by Setup anyway. }
procedure TakeOverZipInstall(const GameDir: String);
var
  Files: TArrayOfString;
  I, Bar: Integer;
  Rel, Path: String;
begin
  if not ReadZipRecord(GameDir, Files) then Exit;
  { PrepareToInstall refused a record with anything unknown in it; checked again right here. }
  if ZipRecordProblem(GameDir) <> '' then Exit;
  for I := 0 to GetArrayLength(Files) - 1 do begin
    Bar := Pos('|', Files[I]);
    if Bar = 0 then Continue;
    Rel := Copy(Files[I], 1, Bar - 1);
    if not KnownZipFile(Rel) then Continue;
    Path := AddBackslash(GameDir) + Rel;
    if SameHash(Path, Copy(Files[I], Bar + 1, Length(Files[I]))) then DeleteFile(Path);
  end;
  DeleteFile(AddBackslash(GameDir) + '{#CtrlDir}\' + RecordName);
  Log('Took over the install made with Install.cmd.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then TakeOverZipInstall(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
  if CurStep = ssPostInstall then
    SaveStringToFile(ExpandConstant('{app}\{#CtrlDir}\') + OwnedName,
      '# generalszh.exe as installed by the Zero Hour Controller Mod Setup (updates check it)' + #13#10 +
      'ExeSHA256=' + ExeHash + #13#10, False);
  if (CurStep = ssPostInstall) and not WizardSilent then begin
    if not FileExists(ExpandConstant('{sys}\vcruntime140.dll')) then
      MsgBox('The Microsoft Visual C++ 2015-2022 Redistributable (x86) seems to be missing, and the game will not start without it.' + #13#10#13#10 +
        'Get "vc_redist.x86.exe" from Microsoft''s website (search: Visual C++ Redistributable latest supported downloads), install it, then play.', mbInformation, MB_OK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Exe: String;
begin
  if CurUninstallStep = usPostUninstall then begin
    Exe := ExpandConstant('{app}\generalszh.exe');
    if SameHash(Exe, ExeHash) then
      DeleteFile(Exe)
    else if FileExists(Exe) then
      Log('generalszh.exe was changed or replaced after Setup; it was left in place.');
  end;
end;
