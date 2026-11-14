; Endless Keeper setup (Inno Setup 6.5 or later)
;
; Installs Endless Keeper into its own folder, the way KeeperFX's own setup installs KeeperFX:
;   1. downloads the KeeperFX base game (a KeeperFX release package), checks it and unpacks it;
;   2. adds the Endless Keeper package (inside this setup) on top;
;   3. copies the original Dungeon Keeper files from the player's own copy (a Dungeon Keeper or KeeperFX folder);
;   4. removes KeeperFX's launchers (they would turn the folder back into plain KeeperFX) and the files KeeperFX
;      lists as obsolete since the base package.
; An update skips the download when the folder already has the same base. Uninstalling keeps the saved games.
;
; Versions: <KeeperFX stable>-<Endless Keeper release> <stage>, e.g. 1.4.0-1 Alpha: Endless Keeper release 1, on
; KeeperFX 1.4.0. The files are named EndlessKeeperSetup-1_4_0-1-Alpha.exe.
;
; Build:  ISCC /DRelease=1 /DStage=Alpha /DPackageDir=<the unpacked Endless Keeper package> installer\endless-keeper.iss
; Silent: EndlessKeeperSetup-1_4_0-1-Alpha.exe /VERYSILENT /DIR="C:\Games\EndlessKeeper" /DKDIR="<Dungeon Keeper folder>"

#ifndef Release
  #define Release "0"
#endif
#ifndef ReleaseNum
  #define ReleaseNum Release
#endif
#ifndef Stage
  #define Stage "Alpha"
#endif
#ifndef PackageDir
  #error PackageDir must be the folder of the unpacked Endless Keeper package (ISCC /DPackageDir=...)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

; The KeeperFX release the Endless Keeper package is laid over
#define BaseVersion  "1.4.0"
#define BaseName     "keeperfx_1_4_0_complete"
#define BaseBuild    "5136"
#define BaseUrl      "https://github.com/dkfans/keeperfx/releases/download/v1.4.0/keeperfx_1_4_0_complete.7z"
#define BaseHash     "82d9d5634e8ea6cabb8f62fbd4830b758919893863cd313517af256a3040a279"
#define BaseSize     "374587275"
#define BaseUnpacked "668012479"

#define AppVersion   BaseVersion + "-" + Release + " " + Stage
#define FileVersion  StringChange(BaseVersion, ".", "_") + "-" + Release + "-" + Stage

[Setup]
AppId={{79BFF639-7219-482F-B46A-D2A8ECFEC411}
AppName=Endless Keeper
AppVersion={#AppVersion}
AppVerName=Endless Keeper {#AppVersion}
AppPublisher=Endless Keeper
AppPublisherURL=https://github.com/K11MCH1/Endless-Keeper
AppSupportURL=https://github.com/K11MCH1/Endless-Keeper/issues
AppUpdatesURL=https://github.com/K11MCH1/Endless-Keeper/releases
DefaultDirName={sd}\Games\EndlessKeeper
; the folder page is always shown: the player sees where Endless Keeper goes
DisableDirPage=no
DefaultGroupName=Endless Keeper
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
UsedUserAreasWarning=no
OutputDir={#OutputDir}
OutputBaseFilename=EndlessKeeperSetup-{#FileVersion}
VersionInfoVersion={#BaseVersion}.{#ReleaseNum}
VersionInfoProductVersion={#BaseVersion}.{#ReleaseNum}
VersionInfoTextVersion={#AppVersion}
SetupIconFile=endless-keeper.ico
UninstallDisplayIcon={app}\EndlessKeeper.exe
UninstallDisplayName=Endless Keeper
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic
ArchiveExtraction=full
ExtraDiskSpaceRequired={#BaseUnpacked}

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nEndless Keeper is built on KeeperFX. During setup it downloads the KeeperFX base game (about 360 MB) and copies a few files from your own copy of Dungeon Keeper, so you need an internet connection and Dungeon Keeper (GOG, Steam, EA/Origin or CD) or an existing KeeperFX installation.%n%nEndless Keeper gets its own folder: an existing KeeperFX installation is not changed.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; 1. the KeeperFX base game is downloaded, checked and unpacked by InstallBase below, in one pass (a solid archive
;    unpacked file by file through [Files] would take hours); an update that has this base already skips it
; 2. Endless Keeper (the game settings file only on a new install: an update keeps the player's settings)
Source: "{#PackageDir}\*"; Excludes: "\keeperfx.cfg"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#PackageDir}\keeperfx.cfg"; DestDir: "{app}"; Flags: ignoreversion; Check: IsNewInstall
; 3. the original Dungeon Keeper files, from the player's own copy
Source: "{code:DkFile|data\bluepal.dat}";   DestDir: "{app}\data"; DestName: "bluepal.dat";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\bluepall.dat}";  DestDir: "{app}\data"; DestName: "bluepall.dat";  Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\dogpal.pal}";    DestDir: "{app}\data"; DestName: "dogpal.pal";    Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\hitpall.dat}";   DestDir: "{app}\data"; DestName: "hitpall.dat";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\lightng.pal}";   DestDir: "{app}\data"; DestName: "lightng.pal";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\redpal.col}";    DestDir: "{app}\data"; DestName: "redpal.col";    Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\redpall.dat}";   DestDir: "{app}\data"; DestName: "redpall.dat";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\slab0-0.dat}";   DestDir: "{app}\data"; DestName: "slab0-0.dat";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\slab0-1.dat}";   DestDir: "{app}\data"; DestName: "slab0-1.dat";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\vampal.pal}";    DestDir: "{app}\data"; DestName: "vampal.pal";    Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|data\whitepal.col}";  DestDir: "{app}\data"; DestName: "whitepal.col";  Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|sound\atmos1.sbk}";   DestDir: "{app}\sound"; DestName: "atmos1.sbk";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|sound\atmos2.sbk}";   DestDir: "{app}\sound"; DestName: "atmos2.sbk";   Flags: external ignoreversion; Check: DkElsewhere
Source: "{code:DkFile|sound\bullfrog.sbk}"; DestDir: "{app}\sound"; DestName: "bullfrog.sbk"; Flags: external ignoreversion; Check: DkElsewhere
; the music of digital copies (GOG, Steam), when there is any
Source: "{code:DkMusic|keeper02.ogg}"; DestDir: "{app}\music"; DestName: "keeper02.ogg"; Flags: external ignoreversion skipifsourcedoesntexist; Check: DkElsewhere
Source: "{code:DkMusic|keeper03.ogg}"; DestDir: "{app}\music"; DestName: "keeper03.ogg"; Flags: external ignoreversion skipifsourcedoesntexist; Check: DkElsewhere
Source: "{code:DkMusic|keeper04.ogg}"; DestDir: "{app}\music"; DestName: "keeper04.ogg"; Flags: external ignoreversion skipifsourcedoesntexist; Check: DkElsewhere
Source: "{code:DkMusic|keeper05.ogg}"; DestDir: "{app}\music"; DestName: "keeper05.ogg"; Flags: external ignoreversion skipifsourcedoesntexist; Check: DkElsewhere
Source: "{code:DkMusic|keeper06.ogg}"; DestDir: "{app}\music"; DestName: "keeper06.ogg"; Flags: external ignoreversion skipifsourcedoesntexist; Check: DkElsewhere
Source: "{code:DkMusic|keeper07.ogg}"; DestDir: "{app}\music"; DestName: "keeper07.ogg"; Flags: external ignoreversion skipifsourcedoesntexist; Check: DkElsewhere

[INI]
Filename: "{app}\endless-keeper.ini"; Section: "install"; Key: "version"; String: "{#AppVersion}"
Filename: "{app}\endless-keeper.ini"; Section: "install"; Key: "base"; String: "{#BaseName}"

[Icons]
; (the launcher: it starts the game, 64-bit where Windows can run it, and keeps Endless Keeper up to date)
Name: "{group}\Endless Keeper"; Filename: "{app}\EndlessKeeper.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Endless Keeper"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Endless Keeper"; Filename: "{app}\EndlessKeeper.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\EndlessKeeper.exe"; WorkingDir: "{app}"; Description: "Start Endless Keeper"; Flags: nowait postinstall skipifsilent

[Code]
const
  DataFileCount = 11;
  SoundFileCount = 3;

var
  DkPage: TInputDirWizardPage;
  DestNote: TNewStaticText;
  DownloadPage: TDownloadWizardPage;
  ExtractionPage: TExtractionWizardPage;
  NewInstall, BaseDone: Boolean;

function DataFileName(I: Integer): String;
begin
  case I of
    0: Result := 'bluepal.dat';   1: Result := 'bluepall.dat';  2: Result := 'dogpal.pal';
    3: Result := 'hitpall.dat';   4: Result := 'lightng.pal';   5: Result := 'redpal.col';
    6: Result := 'redpall.dat';   7: Result := 'slab0-0.dat';   8: Result := 'slab0-1.dat';
    9: Result := 'vampal.pal';   10: Result := 'whitepal.col';
  end;
end;

function SoundFileName(I: Integer): String;
begin
  case I of
    0: Result := 'atmos1.sbk';  1: Result := 'atmos2.sbk';  2: Result := 'bullfrog.sbk';
  end;
end;

{ The required original files missing from Dir, one per line ('' when it has them all). }
function MissingDkFiles(Dir: String): String;
var
  I: Integer;
begin
  Result := '';
  Dir := AddBackslash(Dir);
  for I := 0 to DataFileCount - 1 do
    if not FileExists(Dir + 'data\' + DataFileName(I)) then
      Result := Result + 'data\' + DataFileName(I) + #13#10;
  for I := 0 to SoundFileCount - 1 do
    if not FileExists(Dir + 'sound\' + SoundFileName(I)) then
      Result := Result + 'sound\' + SoundFileName(I) + #13#10;
end;

function IsDkDir(Dir: String): Boolean;
begin
  Result := (Dir <> '') and DirExists(Dir) and (MissingDkFiles(Dir) = '');
end;

{ Where Dungeon Keeper or KeeperFX usually is: the first folder that has the files, or ''. }
function FindDkDir(): String;
var
  Paths: TArrayOfString;
  Steam, KeeperFx: String;
  I, N: Integer;
begin
  N := 0;
  SetArrayLength(Paths, 16);
  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\KeeperFX_is1', 'InstallLocation', KeeperFx) then
  begin
    Paths[N] := KeeperFx; N := N + 1;
  end;
  Paths[N] := ExpandConstant('{sd}\Games\KeeperFX'); N := N + 1;
  Paths[N] := ExpandConstant('{sd}\GOG Games\Dungeon Keeper Gold'); N := N + 1;
  Paths[N] := 'C:\GOG Games\Dungeon Keeper Gold'; N := N + 1;
  Paths[N] := ExpandConstant('{commonpf32}\GOG Galaxy\Games\Dungeon Keeper Gold'); N := N + 1;
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
  begin
    StringChangeEx(Steam, '/', '\', True);
    Paths[N] := AddBackslash(Steam) + 'steamapps\common\Dungeon Keeper'; N := N + 1;
  end;
  Paths[N] := ExpandConstant('{commonpf32}\Steam\steamapps\common\Dungeon Keeper'); N := N + 1;
  Paths[N] := ExpandConstant('{commonpf32}\Origin Games\Dungeon Keeper'); N := N + 1;
  Paths[N] := ExpandConstant('{commonpf32}\Origin Games\Dungeon Keeper\DATA'); N := N + 1;
  Paths[N] := ExpandConstant('{commonpf}\EA Games\Dungeon Keeper'); N := N + 1;
  Paths[N] := ExpandConstant('{commonpf}\EA Games\Dungeon Keeper\DATA'); N := N + 1;
  Result := '';
  for I := 0 to N - 1 do
    if IsDkDir(Paths[I]) then
    begin
      Result := Paths[I];
      Exit;
    end;
end;

function DkDir(): String;
begin
  Result := RemoveBackslashUnlessRoot(DkPage.Values[0]);
end;

{ The original files are copied unless they are in the install folder already (an update). }
function DkElsewhere(): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(DkDir()), RemoveBackslashUnlessRoot(WizardDirValue())) <> 0;
end;

function DkFile(Param: String): String;
begin
  Result := AddBackslash(DkDir()) + Param;
end;

{ Digital copies keep the music in the game folder, KeeperFX in music\. }
function DkMusic(Param: String): String;
begin
  Result := AddBackslash(DkDir()) + Param;
  if not FileExists(Result) and FileExists(AddBackslash(DkDir()) + 'music\' + Param) then
    Result := AddBackslash(DkDir()) + 'music\' + Param;
end;

function InstalledBase(): String;
begin
  Result := GetIniString('install', 'base', '', ExpandConstant('{app}\endless-keeper.ini'));
end;

function NeedBase(): Boolean;
begin
  Result := InstalledBase() <> '{#BaseName}';
end;

function IsNewInstall(): Boolean;
begin
  Result := NewInstall;
end;

procedure InitializeWizard();
var
  Given: String;
begin
  { the folder offered: where Endless Keeper is now, unless that folder no longer holds it (a plain KeeperFX is
    there now, or nothing) }
  if (WizardForm.PrevAppDir <> '') and not FileExists(AddBackslash(WizardForm.PrevAppDir) + 'endless-keeper.ini') then
    WizardForm.DirEdit.Text := ExpandConstant('{sd}\Games\EndlessKeeper');
  DkPage := CreateInputDirPage(wpSelectDir, 'Original game files',
    'Where can the setup copy Dungeon Keeper''s files from?',
    'Endless Keeper is NOT installed in this folder. The setup only copies a few files from it (the game''s ' +
    'palettes, two texture files and three sound banks) and changes nothing there.' + #13#10#13#10 +
    'Choose the folder of your Dungeon Keeper (GOG, Steam, EA/Origin or the contents of the CD), or of a ' +
    'KeeperFX installation, which has these files too. It must have the data and sound folders of the game.',
    False, '');
  DkPage.Add('Copy the original files from:');
  DestNote := TNewStaticText.Create(DkPage);
  DestNote.Parent := DkPage.Surface;
  DestNote.AutoSize := False;
  DestNote.WordWrap := True;
  DestNote.Left := 0;
  DestNote.Top := DkPage.Edits[0].Top + DkPage.Edits[0].Height + ScaleY(18);
  DestNote.Width := DkPage.SurfaceWidth;
  DestNote.Height := ScaleY(44);
  Given := ExpandConstant('{param:DKDIR|}');
  if Given <> '' then
    DkPage.Values[0] := Given
  else
    DkPage.Values[0] := FindDkDir();
  DownloadPage := CreateDownloadPage('Downloading the KeeperFX base game',
    'Endless Keeper is built on KeeperFX. This download is about 360 MB.', nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
  ExtractionPage := CreateExtractionPage('Unpacking the KeeperFX base game',
    'This takes a moment.', nil);
end;

{ Downloads the KeeperFX base package to the temporary folder, checks it and unpacks it into the install folder in one pass.
  Returns '' or the error. Nothing to do when the folder already has this base. }
function InstallBase(): String;
var
  Archive: String;
begin
  Result := '';
  if BaseDone or not NeedBase() then
  begin
    BaseDone := True;
    Exit;
  end;
  Archive := ExpandConstant('{tmp}\{#BaseName}.7z');
  try
    if WizardSilent() then
    begin
      DownloadTemporaryFile('{#BaseUrl}', '{#BaseName}.7z', '{#BaseHash}', nil);
      ForceDirectories(ExpandConstant('{app}'));
      ExtractArchive(Archive, ExpandConstant('{app}'), '', True, nil);
    end
    else
    begin
      DownloadPage.Clear;
      DownloadPage.Add('{#BaseUrl}', '{#BaseName}.7z', '{#BaseHash}');
      DownloadPage.Show;
      try
        DownloadPage.Download;
      finally
        DownloadPage.Hide;
      end;
      ForceDirectories(ExpandConstant('{app}'));
      ExtractionPage.Clear;
      ExtractionPage.Add(Archive, ExpandConstant('{app}'), True);
      ExtractionPage.Show;
      try
        ExtractionPage.Extract;
      finally
        ExtractionPage.Hide;
      end;
    end;
    DeleteFile(Archive);
    BaseDone := True;
  except
    Result := 'The KeeperFX base game could not be downloaded or unpacked: ' + GetExceptionMessage + #13#10#13#10 +
      'Please check your internet connection and run the setup again.';
  end;
end;

{ The page of the original files says where Endless Keeper itself goes. }
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = DkPage.ID then
    DestNote.Caption := 'Endless Keeper itself will be installed in:' + #13#10 + WizardDirValue();
end;

{ An update: the folder Endless Keeper is in has the original files already, so the setup doesn't ask for them. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (PageID = DkPage.ID) and FileExists(AddBackslash(WizardDirValue()) + 'endless-keeper.ini') and IsDkDir(WizardDirValue()) then
  begin
    DkPage.Values[0] := WizardDirValue();
    Result := True;
  end;
end;

{ The last page before the install names both folders. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if DkElsewhere() then
    Result := Result + NewLine + NewLine + 'Original game files copied from (nothing is changed there):' + NewLine + Space + DkDir();
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, Missing: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Dir := AddBackslash(WizardDirValue());
    if FileExists(Dir + 'keeper.exe') or FileExists(Dir + 'deeper.exe') or FileExists(Dir + 'keeper95.exe') then
    begin
      SuppressibleMsgBox('This is a Dungeon Keeper folder. Please choose a new, empty folder for Endless Keeper.',
        mbError, MB_OK, IDOK);
      Result := False;
    end
    else if (FileExists(Dir + 'keeperfx-launcher-qt.exe') or FileExists(Dir + 'keeperfx.exe')) and
      not FileExists(Dir + 'endless-keeper.ini') then
    begin
      SuppressibleMsgBox('This folder has a KeeperFX installation. Endless Keeper needs its own folder, so that ' +
        'your KeeperFX stays as it is. Please choose a new, empty folder.', mbError, MB_OK, IDOK);
      Result := False;
    end;
  end
  else if CurPageID = wpReady then
  begin
    Missing := InstallBase();
    if Missing <> '' then
    begin
      SuppressibleMsgBox(Missing, mbCriticalError, MB_OK, IDOK);
      Result := False;
    end;
  end
  else if CurPageID = DkPage.ID then
  begin
    Dir := DkDir();
    if (Dir = '') or not DirExists(Dir) then
    begin
      SuppressibleMsgBox('Please choose the folder of your Dungeon Keeper or KeeperFX.', mbError, MB_OK, IDOK);
      Result := False;
    end
    else
    begin
      Missing := MissingDkFiles(Dir);
      if Missing <> '' then
      begin
        SuppressibleMsgBox('These files are not in that folder:' + #13#10#13#10 + Missing + #13#10 +
          'Please choose the folder of your Dungeon Keeper (GOG, Steam, EA/Origin or the CD) or of a KeeperFX ' +
          'installation.', mbError, MB_OK, IDOK);
        Result := False;
      end;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  NewInstall := not FileExists(ExpandConstant('{app}\endless-keeper.ini'));
  { an update whose folder has the original files takes them from there (a silent update finds no other copy) }
  if not IsDkDir(DkDir()) and IsDkDir(ExpandConstant('{app}')) then
    DkPage.Values[0] := ExpandConstant('{app}');
  { silent installs come straight here: check the original files and the folder too }
  if not IsDkDir(DkDir()) then
    Result := 'The original Dungeon Keeper files were not found in "' + DkDir() + '". ' +
      'Run the setup again and choose your Dungeon Keeper or KeeperFX folder (or pass /DKDIR="<folder>").'
  else
    Result := InstallBase(); { done already on the Ready page, except in silent installs }
end;

{ Deletes the files KeeperFX lists as obsolete in builds newer than the base package. }
procedure RemoveObsoleteFiles();
var
  Lines: TArrayOfString;
  I, P, Build: Integer;
  Line, Path: String;
begin
  if not LoadStringsFromFile(ExpandConstant('{app}\launcher-auto-file-removal.txt'), Lines) then
    Exit;
  Build := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if (Line = '') or (Copy(Line, 1, 1) = '#') then
      Continue;
    if (Copy(Line, 1, 1) = '[') and (Copy(Line, Length(Line), 1) = ']') then
    begin
      Line := Copy(Line, 2, Length(Line) - 2);
      P := Length(Line);
      while (P > 0) and (Copy(Line, P, 1) <> '.') do
        P := P - 1;
      Build := StrToIntDef(Copy(Line, P + 1, Length(Line) - P), 0);
      Continue;
    end;
    if Build <= {#BaseBuild} then
      Continue;
    Path := Line;
    StringChangeEx(Path, '/', '\', True);
    while (Length(Path) > 0) and (Copy(Path, 1, 1) = '\') do
      Path := Copy(Path, 2, Length(Path) - 1);
    if (Pos('..', Path) = 0) and (Path <> '') then
      DeleteFile(ExpandConstant('{app}\') + Path);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    { KeeperFX's launchers would update this folder to plain KeeperFX }
    DeleteFile(ExpandConstant('{app}\keeperfx-launcher-qt.exe'));
    DeleteFile(ExpandConstant('{app}\keeperfx-launcher-legacy.exe'));
    RemoveObsoleteFiles();
  end;
end;

{ Uninstalling removes everything but the saved games. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
  FindRec: TFindRec;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  App := ExpandConstant('{app}');
  if FindFirst(App + '\*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') and (CompareText(FindRec.Name, 'save') <> 0) then
        begin
          if FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0 then
            DelTree(App + '\' + FindRec.Name, True, True, True)
          else
            DeleteFile(App + '\' + FindRec.Name);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  RemoveDir(App);
end;
