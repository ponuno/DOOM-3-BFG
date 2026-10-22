/*
================================================================================================

	n3dstex window (Windows): n3dstex.exe started without arguments

	One window over the same converter as the command line (RunConverter): pick the
	game's folder, tick what to convert, press Start. Results go to outputDoom3 next to
	the base folder, laid out as the SD card (outputDoom3/doom3/base/...). The converter
	runs on a worker thread; its log lines and progress are handed over under a lock and
	shown by a 100 ms timer. Settings are remembered in HKCU\Software\n3dstex.

	Paths: the program declares the UTF-8 code page in its manifest (n3dstex.manifest),
	so the converter's narrow paths (fopen, std::filesystem) take any folder name.

================================================================================================
*/
#if defined( _WIN32 ) && defined( N3DSTEX_GUI )

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "n3dstex.h"

namespace fs = std::filesystem;

enum {
	IDC_BASE_LABEL = 100, IDC_BASE, IDC_BASE_BROWSE, IDC_BASE_INFO,
	IDC_OUT_LABEL, IDC_OUT, IDC_OUT_BROWSE, IDC_OUT_INFO,
	IDC_GROUP_WHAT, IDC_TEX, IDC_SND, IDC_VID, IDC_SD,
	IDC_GROUP_Q, IDC_QTEX_LABEL, IDC_QTEX, IDC_VSIZE_LABEL, IDC_VSIZE, IDC_VFPS_LABEL, IDC_VFPS,
	IDC_START, IDC_CANCEL, IDC_OPENOUT, IDC_PROGRESS, IDC_STATUS, IDC_LOG
};
enum { TIMER_SCAN = 1, TIMER_UI = 2 };
static const UINT WM_APP_DONE = WM_APP + 1;

static HWND			mainWnd;
static HFONT		uiFont, logFont, boldFont;
static int			dpi = 96;
static bool			outEdited = false;		// the user typed or picked an output folder
static bool			settingText = false;	// our own SetWindowText: not the user's edit
static bool			running = false;
static bool			closeWhenDone = false;
static std::thread	worker;
static std::chrono::steady_clock::time_point runStart;

// handed over from the converter thread
static std::mutex	shareLock;
static std::string	pendingLog;
static int			curStage = -1;
static double		curFraction = 0.0;
static std::string	curText;

static int S( int v ) {
	return MulDiv( v, dpi, 96 );
}

static std::wstring Wide( const std::string & s ) {
	if ( s.empty() ) {
		return std::wstring();
	}
	const int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0 );
	std::wstring w( n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n );
	return w;
}

static std::string Utf8( const std::wstring & w ) {
	if ( w.empty() ) {
		return std::string();
	}
	const int n = WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL );
	std::string s( n, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL );
	return s;
}

static HWND Item( int id ) {
	return GetDlgItem( mainWnd, id );
}

static std::wstring GetText( int id ) {
	HWND h = Item( id );
	const int n = GetWindowTextLengthW( h );
	std::wstring w( n + 1, L'\0' );
	GetWindowTextW( h, &w[0], n + 1 );
	w.resize( n );
	return w;
}

static void SetText( int id, const std::wstring & text ) {
	settingText = true;
	SetWindowTextW( Item( id ), text.c_str() );
	settingText = false;
}

static bool Checked( int id ) {
	return SendMessageW( Item( id ), BM_GETCHECK, 0, 0 ) == BST_CHECKED;
}

static void SetChecked( int id, bool on ) {
	SendMessageW( Item( id ), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0 );
}

static int ComboIndex( int id ) {
	return (int)SendMessageW( Item( id ), CB_GETCURSEL, 0, 0 );
}

/*
========================
Settings (HKCU\Software\n3dstex)
========================
*/
static const wchar_t * REG_KEY = L"Software\\n3dstex";

static std::wstring RegString( const wchar_t * name ) {
	wchar_t buf[2048];
	DWORD size = sizeof( buf );
	if ( RegGetValueW( HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ, NULL, buf, &size ) == ERROR_SUCCESS ) {
		return buf;
	}
	return std::wstring();
}

static int RegInt( const wchar_t * name, int def ) {
	DWORD v = 0, size = sizeof( v );
	if ( RegGetValueW( HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &size ) == ERROR_SUCCESS ) {
		return (int)v;
	}
	return def;
}

static void SaveSettings() {
	HKEY key;
	if ( RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ) != ERROR_SUCCESS ) {
		return;
	}
	auto putStr = [&]( const wchar_t * name, const std::wstring & v ) {
		RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)( ( v.size() + 1 ) * sizeof( wchar_t ) ) );
	};
	auto putInt = [&]( const wchar_t * name, int v ) {
		DWORD d = (DWORD)v;
		RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&d, sizeof( d ) );
	};
	putStr( L"base", GetText( IDC_BASE ) );
	putStr( L"out", outEdited ? GetText( IDC_OUT ) : std::wstring() );
	putInt( L"textures", Checked( IDC_TEX ) );
	putInt( L"sounds", Checked( IDC_SND ) );
	putInt( L"videos", Checked( IDC_VID ) );
	putInt( L"sd", Checked( IDC_SD ) );
	putInt( L"qtex", ComboIndex( IDC_QTEX ) );
	putInt( L"vsize", ComboIndex( IDC_VSIZE ) );
	putInt( L"vfps", ComboIndex( IDC_VFPS ) );
	RegCloseKey( key );
}

/*
========================
Folder checks
========================
*/
struct baseInfo_t {
	bool		found = false;		// _common.resources
	int			resources = 0;
	bool		sounds = false;		// _sound_pc*.resources
	int			videos = 0;
	uint64_t	bytes = 0;			// of the .resources
	std::string	base;				// detected base folder
};

static baseInfo_t ScanBase( const std::wstring & dir ) {
	baseInfo_t info;
	if ( dir.empty() ) {
		return info;
	}
	info.base = DetectBaseDir( Utf8( dir ) );
	std::error_code ec;
	const fs::path base = fs::u8path( info.base );
	info.found = fs::exists( base / "_common.resources", ec );
	if ( !fs::is_directory( base, ec ) ) {
		return info;
	}
	for ( fs::recursive_directory_iterator it( base, fs::directory_options::skip_permission_denied, ec ), endIt; it != endIt; it.increment( ec ) ) {
		if ( ec ) {
			break;
		}
		if ( it->is_directory( ec ) && it->path().filename() == "outputDoom3" ) {
			it.disable_recursion_pending();
			continue;
		}
		if ( !it->is_regular_file( ec ) ) {
			continue;
		}
		std::wstring ext = it->path().extension().wstring();
		for ( wchar_t & c : ext ) {
			c = towlower( c );
		}
		if ( ext == L".resources" ) {
			info.resources++;
			info.bytes += it->file_size( ec );
			std::wstring name = it->path().filename().wstring();
			for ( wchar_t & c : name ) {
				c = towlower( c );
			}
			if ( name.compare( 0, 9, L"_sound_pc" ) == 0 ) {
				info.sounds = true;
			}
		} else if ( ext == L".bik" ) {
			info.videos++;
		}
	}
	return info;
}

static std::wstring DefaultOut( const std::string & base ) {
	fs::path b = fs::u8path( base );
	std::error_code ec;
	fs::path c = fs::weakly_canonical( b, ec );
	if ( !ec && !c.empty() ) {
		b = c;
	}
	if ( !b.has_filename() ) {
		b = b.parent_path();
	}
	fs::path parent = b.parent_path();
	if ( parent.empty() ) {
		parent = b;
	}
	return ( parent / "outputDoom3" ).wstring();
}

static void UpdateOutInfo() {
	const std::wstring out = GetText( IDC_OUT );
	if ( out.empty() ) {
		SetText( IDC_OUT_INFO, L"" );
		return;
	}
	SetText( IDC_OUT_INFO, L"Получится: " + ( fs::path( out ) / L"doom3" / L"base" ).wstring() +
		L"  \u2192  содержимое outputDoom3 копируется в корень SD-карты" );
}

static void RefreshBaseInfo() {
	const std::wstring dir = GetText( IDC_BASE );
	if ( dir.empty() ) {
		SetText( IDC_BASE_INFO, L"Укажите папку игры DOOM 3 BFG Edition (или её папку base)." );
		return;
	}
	const baseInfo_t info = ScanBase( dir );
	std::wstring text;
	if ( info.found ) {
		wchar_t buf[512];
		swprintf( buf, 512, L"\u2713 Найдено: %d файлов .resources (%.1f ГБ)%s, роликов .bik: %d", info.resources,
			info.bytes / 1073741824.0, info.sounds ? L", звуки есть" : L", звуков (_sound_pc*.resources) нет", info.videos );
		text = buf;
		if ( Wide( info.base ) != dir ) {
			text += L"  (папка base: " + Wide( info.base ) + L")";
		}
	} else if ( info.videos > 0 ) {
		wchar_t buf[256];
		swprintf( buf, 256, L"Нет _common.resources: можно сконвертировать только ролики (%d .bik).", info.videos );
		text = buf;
	} else {
		text = L"\u2717 Здесь нет _common.resources: выберите папку base игры (…\\DOOM 3 BFG Edition\\base).";
	}
	SetText( IDC_BASE_INFO, text );
	if ( !outEdited ) {
		SetText( IDC_OUT, DefaultOut( info.base ) );
	}
	UpdateOutInfo();
}

static bool BrowseFolder( const wchar_t * title, std::wstring & path ) {
	IFileOpenDialog * dlg = NULL;
	if ( FAILED( CoCreateInstance( CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&dlg ) ) ) {
		return false;
	}
	DWORD options = 0;
	dlg->GetOptions( &options );
	dlg->SetOptions( options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST );
	dlg->SetTitle( title );
	if ( !path.empty() ) {
		IShellItem * start = NULL;
		std::error_code ec;
		fs::path p( path );
		while ( !p.empty() && !fs::is_directory( p, ec ) && p.has_parent_path() && p.parent_path() != p ) {
			p = p.parent_path();
		}
		if ( SUCCEEDED( SHCreateItemFromParsingName( p.wstring().c_str(), NULL, IID_IShellItem, (void **)&start ) ) ) {
			dlg->SetFolder( start );
			start->Release();
		}
	}
	bool ok = false;
	if ( SUCCEEDED( dlg->Show( mainWnd ) ) ) {
		IShellItem * result = NULL;
		if ( SUCCEEDED( dlg->GetResult( &result ) ) ) {
			PWSTR chosen = NULL;
			if ( SUCCEEDED( result->GetDisplayName( SIGDN_FILESYSPATH, &chosen ) ) ) {
				path = chosen;
				CoTaskMemFree( chosen );
				ok = true;
			}
			result->Release();
		}
	}
	dlg->Release();
	return ok;
}

/*
========================
Converter thread hand-over
========================
*/
static void GuiLogSink( const char * text ) {
	std::lock_guard<std::mutex> lock( shareLock );
	pendingLog += text;
}

static void GuiProgressSink( int stage, double fraction, const char * text ) {
	std::lock_guard<std::mutex> lock( shareLock );
	curStage = stage;
	curFraction = fraction;
	curText = text;
}

static void AppendLog( const std::string & utf8 ) {
	std::wstring w = Wide( utf8 );
	std::wstring crlf;
	crlf.reserve( w.size() + w.size() / 16 );
	for ( wchar_t c : w ) {
		if ( c == L'\n' ) {
			crlf += L'\r';
		}
		crlf += c;
	}
	HWND log = Item( IDC_LOG );
	const int len = GetWindowTextLengthW( log );
	SendMessageW( log, EM_SETSEL, len, len );
	SendMessageW( log, EM_REPLACESEL, FALSE, (LPARAM)crlf.c_str() );
	SendMessageW( log, EM_SCROLLCARET, 0, 0 );
}

static const wchar_t * StageName( int stage ) {
	switch ( stage ) {
		case STAGE_TEXTURES:	return L"Текстуры";
		case STAGE_SOUNDS:		return L"Звуки";
		case STAGE_VIDEOS:		return L"Ролики";
		case STAGE_SDCOPY:		return L"Папка для SD";
		default:				return L"";
	}
}

static void UpdateUi() {
	std::string log, text;
	int stage;
	double fraction;
	{
		std::lock_guard<std::mutex> lock( shareLock );
		log.swap( pendingLog );
		stage = curStage;
		fraction = curFraction;
		text = curText;
	}
	if ( !log.empty() ) {
		AppendLog( log );
	}
	if ( !running ) {
		return;
	}
	// overall: the enabled stages weighted by how long they usually take
	static const double weight[NUM_STAGES] = { 0.45, 0.35, 0.15, 0.05 };
	const bool on[NUM_STAGES] = { Checked( IDC_TEX ), Checked( IDC_SND ), Checked( IDC_VID ), Checked( IDC_SD ) };
	double total = 0.0, before = 0.0;
	int step = 0, steps = 0;
	for ( int s = 0; s < NUM_STAGES; s++ ) {
		if ( !on[s] ) {
			continue;
		}
		total += weight[s];
		steps++;
		if ( stage >= 0 && s < stage ) {
			before += weight[s];
			step++;
		}
	}
	double overall = 0.0;
	if ( total > 0.0 && stage >= 0 && stage < NUM_STAGES ) {
		overall = ( before + weight[stage] * fraction ) / total;
	}
	SendMessageW( Item( IDC_PROGRESS ), PBM_SETPOS, (WPARAM)( overall * 1000.0 ), 0 );
	const int secs = (int)std::chrono::duration<double>( std::chrono::steady_clock::now() - runStart ).count();
	wchar_t buf[512];
	if ( stage < 0 ) {
		swprintf( buf, 512, L"Читаю файлы игры…   %d:%02d", secs / 60, secs % 60 );
	} else {
		swprintf( buf, 512, L"Шаг %d из %d — %ls: %ls   (%d%%, %d:%02d)", step + 1, steps, StageName( stage ), Wide( text ).c_str(),
			(int)( overall * 100.0 ), secs / 60, secs % 60 );
	}
	SetText( IDC_STATUS, buf );
}

static void EnableInputs( bool enable ) {
	static const int ids[] = { IDC_BASE, IDC_BASE_BROWSE, IDC_OUT, IDC_OUT_BROWSE, IDC_TEX, IDC_SND, IDC_VID, IDC_SD,
		IDC_QTEX, IDC_VSIZE, IDC_VFPS, IDC_START };
	for ( int id : ids ) {
		EnableWindow( Item( id ), enable );
	}
	EnableWindow( Item( IDC_CANCEL ), !enable );
}

static void StartConversion() {
	if ( running ) {
		return;
	}
	const std::wstring baseW = GetText( IDC_BASE );
	const std::wstring outW = GetText( IDC_OUT );
	const bool tex = Checked( IDC_TEX ), snd = Checked( IDC_SND ), vid = Checked( IDC_VID ), sd = Checked( IDC_SD );
	if ( !tex && !snd && !vid && !sd ) {
		MessageBoxW( mainWnd, L"Отметьте, что конвертировать.", L"n3dstex", MB_ICONINFORMATION );
		return;
	}
	const baseInfo_t info = ScanBase( baseW );
	if ( !info.found && ( tex || snd || sd ) ) {
		MessageBoxW( mainWnd, L"В выбранной папке нет _common.resources.\n\nУкажите папку base игры DOOM 3 BFG Edition "
			L"(например C:\\Program Files (x86)\\Steam\\steamapps\\common\\DOOM 3 BFG Edition\\base).", L"n3dstex", MB_ICONWARNING );
		return;
	}
	if ( !info.found && info.videos == 0 ) {
		MessageBoxW( mainWnd, L"В выбранной папке нет ни файлов игры, ни роликов .bik.", L"n3dstex", MB_ICONWARNING );
		return;
	}
	if ( outW.empty() ) {
		MessageBoxW( mainWnd, L"Укажите папку результата.", L"n3dstex", MB_ICONWARNING );
		return;
	}
	// free space, roughly: packs ~0.2 + 0.7 GB, videos ~0.15 GB, the SD folder the slimmed .resources
	double needGB = ( tex ? 0.25 : 0.0 ) + ( snd ? 0.75 : 0.0 ) + ( vid ? 0.2 : 0.0 ) + ( sd ? info.bytes / 1073741824.0 * 0.6 : 0.0 );
	fs::path probe( outW );
	std::error_code ec;
	while ( !probe.empty() && !fs::exists( probe, ec ) && probe.has_parent_path() && probe.parent_path() != probe ) {
		probe = probe.parent_path();
	}
	ULARGE_INTEGER freeBytes;
	if ( GetDiskFreeSpaceExW( probe.wstring().c_str(), &freeBytes, NULL, NULL ) && freeBytes.QuadPart / 1073741824.0 < needGB ) {
		wchar_t buf[512];
		swprintf( buf, 512, L"На диске с папкой результата свободно %.1f ГБ, а нужно примерно %.1f ГБ.\n\nВсё равно начать?",
			freeBytes.QuadPart / 1073741824.0, needGB );
		if ( MessageBoxW( mainWnd, buf, L"n3dstex", MB_YESNO | MB_ICONWARNING ) != IDYES ) {
			return;
		}
	}
	SaveSettings();

	static const int texQuality[] = { 0, 1, 2 };
	static const int videoSizes[] = { 256, 512, 1024 };
	static const int videoFps[] = { 15, 20, 30 };
	opt = options_t();
	opt.baseDir = Utf8( baseW );
	opt.outDir = Utf8( outW );
	opt.doTextures = tex;
	opt.doSounds = snd;
	opt.doVideos = vid;
	opt.sdFull = sd;
	opt.quality = texQuality[std::max( 0, std::min( 2, ComboIndex( IDC_QTEX ) ) )];
	opt.videoSize = videoSizes[std::max( 0, std::min( 2, ComboIndex( IDC_VSIZE ) ) )];
	opt.videoFps = videoFps[std::max( 0, std::min( 2, ComboIndex( IDC_VFPS ) ) )];
	if ( !ResolveOptions() ) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock( shareLock );
		pendingLog.clear();
		curStage = -1;
		curFraction = 0.0;
		curText.clear();
	}
	SetText( IDC_LOG, L"" );
	SendMessageW( Item( IDC_PROGRESS ), PBM_SETPOS, 0, 0 );
	running = true;
	runStart = std::chrono::steady_clock::now();
	EnableInputs( false );
	cancelRequested = false;
	SetTimer( mainWnd, TIMER_UI, 100, NULL );
	UpdateUi();
	worker = std::thread( []() {
		const int rc = RunConverter();
		PostMessageW( mainWnd, WM_APP_DONE, (WPARAM)rc, 0 );
	} );
}

static void ConversionDone( int rc ) {
	if ( worker.joinable() ) {
		worker.join();
	}
	running = false;
	UpdateUi();
	KillTimer( mainWnd, TIMER_UI );
	EnableInputs( true );
	const int secs = (int)std::chrono::duration<double>( std::chrono::steady_clock::now() - runStart ).count();
	wchar_t buf[1024];
	if ( closeWhenDone ) {
		DestroyWindow( mainWnd );
		return;
	}
	if ( rc == 2 ) {
		SetText( IDC_STATUS, L"Отменено. Недописанные файлы удалены." );
		return;
	}
	if ( rc == 0 ) {
		SendMessageW( Item( IDC_PROGRESS ), PBM_SETPOS, 1000, 0 );
		swprintf( buf, 1024, L"Готово за %d:%02d. Результат: %ls", secs / 60, secs % 60, Wide( opt.outDir ).c_str() );
		SetText( IDC_STATUS, buf );
		swprintf( buf, 1024, L"Готово!\n\nВсё сохранено в папке\n%ls\n\nСкопируйте её содержимое (папку doom3) в корень SD-карты.%ls\n\nОткрыть папку?",
			Wide( opt.outDir ).c_str(), opt.sdFull ? L"" : L"\nОригинальные файлы игры (.resources) должны остаться на карте в doom3\\base." );
		if ( MessageBoxW( mainWnd, buf, L"n3dstex", MB_YESNO | MB_ICONINFORMATION ) == IDYES ) {
			ShellExecuteW( mainWnd, L"open", Wide( opt.outDir ).c_str(), NULL, NULL, SW_SHOWNORMAL );
		}
	} else {
		SetText( IDC_STATUS, L"Завершено с ошибками: подробности в журнале ниже." );
		MessageBoxW( mainWnd, L"Конвертация завершилась с ошибками.\nПодробности в журнале внизу окна.", L"n3dstex", MB_ICONWARNING );
	}
}

/*
========================
Window
========================
*/
static HWND Make( const wchar_t * cls, const wchar_t * text, DWORD style, int id, DWORD exStyle = 0 ) {
	HWND h = CreateWindowExW( exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, mainWnd,
		(HMENU)(INT_PTR)id, GetModuleHandleW( NULL ), NULL );
	SendMessageW( h, WM_SETFONT, (WPARAM)uiFont, FALSE );
	return h;
}

static void FillCombo( int id, const wchar_t * const * items, int count, int sel ) {
	HWND h = Item( id );
	for ( int i = 0; i < count; i++ ) {
		SendMessageW( h, CB_ADDSTRING, 0, (LPARAM)items[i] );
	}
	SendMessageW( h, CB_SETCURSEL, std::max( 0, std::min( count - 1, sel ) ), 0 );
}

static void CreateControls() {
	Make( L"STATIC", L"Папка игры DOOM 3 BFG Edition (или её папка base):", 0, IDC_BASE_LABEL );
	SendMessageW( Item( IDC_BASE_LABEL ), WM_SETFONT, (WPARAM)boldFont, FALSE );
	Make( L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_BASE, WS_EX_CLIENTEDGE );
	Make( L"BUTTON", L"Обзор…", WS_TABSTOP | BS_PUSHBUTTON, IDC_BASE_BROWSE );
	Make( L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, IDC_BASE_INFO );
	Make( L"STATIC", L"Папка результата:", 0, IDC_OUT_LABEL );
	SendMessageW( Item( IDC_OUT_LABEL ), WM_SETFONT, (WPARAM)boldFont, FALSE );
	Make( L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_OUT, WS_EX_CLIENTEDGE );
	Make( L"BUTTON", L"Обзор…", WS_TABSTOP | BS_PUSHBUTTON, IDC_OUT_BROWSE );
	Make( L"STATIC", L"", SS_PATHELLIPSIS | SS_NOPREFIX, IDC_OUT_INFO );

	Make( L"BUTTON", L"Что конвертировать", BS_GROUPBOX, IDC_GROUP_WHAT );
	Make( L"BUTTON", L"Текстуры  (_n3ds_textures.pak)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_TEX );
	Make( L"BUTTON", L"Звуки  (_n3ds_sounds.pak)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_SND );
	Make( L"BUTTON", L"Ролики  (video\\*.n3v)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_VID );
	Make( L"BUTTON", L"Собрать полную папку для SD-карты: файлы игры без того, что заменили паки "
		L"(.resources становятся меньше), плюс все остальные файлы. Тогда на карту не нужно класть оригиналы.",
		WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP, IDC_SD );

	Make( L"BUTTON", L"Качество", BS_GROUPBOX, IDC_GROUP_Q );
	Make( L"STATIC", L"Текстуры (ETC1):", 0, IDC_QTEX_LABEL );
	Make( L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_QTEX );
	Make( L"STATIC", L"Размер роликов:", 0, IDC_VSIZE_LABEL );
	Make( L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_VSIZE );
	Make( L"STATIC", L"Кадров в секунду:", 0, IDC_VFPS_LABEL );
	Make( L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_VFPS );

	Make( L"BUTTON", L"Начать", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_START );
	SendMessageW( Item( IDC_START ), WM_SETFONT, (WPARAM)boldFont, FALSE );
	Make( L"BUTTON", L"Отмена", WS_TABSTOP | BS_PUSHBUTTON, IDC_CANCEL );
	Make( L"BUTTON", L"Открыть папку результата", WS_TABSTOP | BS_PUSHBUTTON, IDC_OPENOUT );
	Make( PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS );
	SendMessageW( Item( IDC_PROGRESS ), PBM_SETRANGE32, 0, 1000 );
	Make( L"STATIC", L"Выберите папку игры и нажмите «Начать».", SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS );
	Make( L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
		IDC_LOG, WS_EX_CLIENTEDGE );
	SendMessageW( Item( IDC_LOG ), WM_SETFONT, (WPARAM)logFont, FALSE );
	SendMessageW( Item( IDC_LOG ), EM_SETLIMITTEXT, 0, 0 );

	static const wchar_t * const qtex[] = { L"Быстро", L"Средне (рекомендуется)", L"Высоко (долго)" };
	static const wchar_t * const vsize[] = { L"256", L"512 (рекомендуется)", L"1024" };
	static const wchar_t * const vfps[] = { L"15", L"20 (рекомендуется)", L"30" };
	FillCombo( IDC_QTEX, qtex, 3, RegInt( L"qtex", 1 ) );
	FillCombo( IDC_VSIZE, vsize, 3, RegInt( L"vsize", 1 ) );
	FillCombo( IDC_VFPS, vfps, 3, RegInt( L"vfps", 1 ) );
	SetChecked( IDC_TEX, RegInt( L"textures", 1 ) != 0 );
	SetChecked( IDC_SND, RegInt( L"sounds", 1 ) != 0 );
	SetChecked( IDC_VID, RegInt( L"videos", 1 ) != 0 );
	SetChecked( IDC_SD, RegInt( L"sd", 1 ) != 0 );
	EnableWindow( Item( IDC_CANCEL ), FALSE );

	// the game folder: next to / around the program first, else the last one used
	std::wstring base;
	wchar_t exe[MAX_PATH * 2];
	if ( GetModuleFileNameW( NULL, exe, MAX_PATH * 2 ) > 0 ) {
		const fs::path dir = fs::path( exe ).parent_path();
		for ( const fs::path & d : { dir, dir.parent_path() } ) {
			const std::string found = DetectBaseDir( Utf8( d.wstring() ) );
			std::error_code ec;
			if ( fs::exists( fs::u8path( found ) / "_common.resources", ec ) ) {
				base = Wide( found );
				break;
			}
		}
	}
	if ( base.empty() ) {
		base = RegString( L"base" );
	}
	const std::wstring out = RegString( L"out" );
	if ( !out.empty() ) {
		outEdited = true;
		SetText( IDC_OUT, out );
	}
	SetText( IDC_BASE, base );
	RefreshBaseInfo();
}

static void Layout( int cx, int cy ) {
	const int m = S( 12 ), gap = S( 8 );
	const int x = m, w = cx - 2 * m;
	const int bw = S( 100 ), eh = S( 24 ), lh = S( 18 );
	auto place = [&]( int id, int px, int py, int pw, int ph ) {
		MoveWindow( Item( id ), px, py, std::max( 1, pw ), std::max( 1, ph ), FALSE );
	};
	int y = m;
	place( IDC_BASE_LABEL, x, y, w, lh );
	y += lh + S( 4 );
	place( IDC_BASE, x, y, w - bw - gap, eh );
	place( IDC_BASE_BROWSE, x + w - bw, y - 1, bw, eh + 2 );
	y += eh + S( 4 );
	place( IDC_BASE_INFO, x, y, w, lh );
	y += lh + S( 10 );
	place( IDC_OUT_LABEL, x, y, w, lh );
	y += lh + S( 4 );
	place( IDC_OUT, x, y, w - bw - gap, eh );
	place( IDC_OUT_BROWSE, x + w - bw, y - 1, bw, eh + 2 );
	y += eh + S( 4 );
	place( IDC_OUT_INFO, x, y, w, lh );
	y += lh + S( 12 );

	// two groups side by side
	const int gh = S( 112 );
	const int gw1 = ( w - gap ) * 5 / 9, gw2 = w - gap - gw1;
	const int gx2 = x + gw1 + gap;
	place( IDC_GROUP_WHAT, x, y, gw1, gh );
	place( IDC_TEX, x + S( 12 ), y + S( 24 ), gw1 - S( 24 ), S( 22 ) );
	place( IDC_SND, x + S( 12 ), y + S( 50 ), gw1 - S( 24 ), S( 22 ) );
	place( IDC_VID, x + S( 12 ), y + S( 76 ), gw1 - S( 24 ), S( 22 ) );
	place( IDC_GROUP_Q, gx2, y, gw2, gh );
	const int lw = std::min( S( 130 ), ( gw2 - S( 24 ) ) / 2 );
	const int cxq = gx2 + S( 12 ) + lw + S( 6 ), cw = gx2 + gw2 - S( 12 ) - cxq;
	place( IDC_QTEX_LABEL, gx2 + S( 12 ), y + S( 26 ), lw, lh );
	place( IDC_QTEX, cxq, y + S( 22 ), cw, S( 200 ) );
	place( IDC_VSIZE_LABEL, gx2 + S( 12 ), y + S( 54 ), lw, lh );
	place( IDC_VSIZE, cxq, y + S( 50 ), cw, S( 200 ) );
	place( IDC_VFPS_LABEL, gx2 + S( 12 ), y + S( 82 ), lw, lh );
	place( IDC_VFPS, cxq, y + S( 78 ), cw, S( 200 ) );
	y += gh + S( 8 );
	place( IDC_SD, x + S( 4 ), y, w - S( 4 ), S( 40 ) );
	y += S( 48 );

	const int bh = S( 32 );
	place( IDC_START, x, y, S( 150 ), bh );
	place( IDC_CANCEL, x + S( 150 ) + gap, y, S( 110 ), bh );
	place( IDC_OPENOUT, x + w - S( 220 ), y, S( 220 ), bh );
	y += bh + S( 10 );
	place( IDC_PROGRESS, x, y, w, S( 18 ) );
	y += S( 18 ) + S( 6 );
	place( IDC_STATUS, x, y, w, lh + S( 2 ) );
	y += lh + S( 10 );
	place( IDC_LOG, x, y, w, std::max( S( 60 ), cy - m - y ) );
	InvalidateRect( mainWnd, NULL, TRUE );
}

static LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	switch ( msg ) {
		case WM_CREATE:
			mainWnd = hwnd;
			CreateControls();
			return 0;
		case WM_SIZE:
			Layout( LOWORD( lp ), HIWORD( lp ) );
			return 0;
		case WM_GETMINMAXINFO: {
			MINMAXINFO * mm = (MINMAXINFO *)lp;
			mm->ptMinTrackSize.x = S( 640 );
			mm->ptMinTrackSize.y = S( 600 );
			return 0;
		}
		case WM_CTLCOLORSTATIC: {
			// the info lines: green when the folder is right, red when not
			HWND ctl = (HWND)lp;
			HDC dc = (HDC)wp;
			if ( ctl == Item( IDC_BASE_INFO ) || ctl == Item( IDC_OUT_INFO ) ) {
				SetBkMode( dc, TRANSPARENT );
				COLORREF color = GetSysColor( COLOR_GRAYTEXT );
				if ( ctl == Item( IDC_BASE_INFO ) ) {
					const std::wstring t = GetText( IDC_BASE_INFO );
					if ( !t.empty() && t[0] == L'\u2713' ) {
						color = RGB( 0, 128, 0 );
					} else if ( !t.empty() && t[0] == L'\u2717' ) {
						color = RGB( 192, 0, 0 );
					}
				}
				SetTextColor( dc, color );
				return (LRESULT)GetSysColorBrush( COLOR_BTNFACE );
			}
			break;
		}
		case WM_COMMAND: {
			const int id = LOWORD( wp ), code = HIWORD( wp );
			if ( id == IDC_BASE && code == EN_CHANGE && !settingText ) {
				SetTimer( hwnd, TIMER_SCAN, 500, NULL );
			} else if ( id == IDC_OUT && code == EN_CHANGE && !settingText ) {
				outEdited = !GetText( IDC_OUT ).empty();
				if ( !outEdited ) {
					RefreshBaseInfo();
				}
				UpdateOutInfo();
			} else if ( id == IDC_BASE_BROWSE ) {
				std::wstring p = GetText( IDC_BASE );
				if ( BrowseFolder( L"Папка игры DOOM 3 BFG Edition или её папка base", p ) ) {
					SetText( IDC_BASE, Wide( DetectBaseDir( Utf8( p ) ) ) );
					RefreshBaseInfo();
				}
			} else if ( id == IDC_OUT_BROWSE ) {
				std::wstring p = GetText( IDC_OUT );
				if ( BrowseFolder( L"Куда сохранить результат", p ) ) {
					// a folder picked as the place for it: the results go into outputDoom3 there
					fs::path chosen( p );
					if ( chosen.filename() != L"outputDoom3" ) {
						chosen /= L"outputDoom3";
					}
					outEdited = true;
					SetText( IDC_OUT, chosen.wstring() );
					UpdateOutInfo();
				}
			} else if ( id == IDC_START ) {
				StartConversion();
			} else if ( id == IDC_CANCEL ) {
				if ( running ) {
					cancelRequested = true;
					EnableWindow( Item( IDC_CANCEL ), FALSE );
					SetText( IDC_STATUS, L"Останавливаю…" );
				}
			} else if ( id == IDC_OPENOUT ) {
				const std::wstring out = GetText( IDC_OUT );
				std::error_code ec;
				if ( !out.empty() && fs::is_directory( fs::path( out ), ec ) ) {
					ShellExecuteW( hwnd, L"open", out.c_str(), NULL, NULL, SW_SHOWNORMAL );
				} else {
					MessageBoxW( hwnd, L"Папки результата ещё нет: она появится после конвертации.", L"n3dstex", MB_ICONINFORMATION );
				}
			}
			return 0;
		}
		case WM_TIMER:
			if ( wp == TIMER_SCAN ) {
				KillTimer( hwnd, TIMER_SCAN );
				RefreshBaseInfo();
			} else if ( wp == TIMER_UI ) {
				UpdateUi();
			}
			return 0;
		case WM_APP_DONE:
			ConversionDone( (int)wp );
			return 0;
		case WM_CLOSE:
			if ( running ) {
				if ( MessageBoxW( hwnd, L"Конвертация ещё идёт. Прервать и закрыть?", L"n3dstex", MB_YESNO | MB_ICONQUESTION ) == IDYES ) {
					closeWhenDone = true;
					cancelRequested = true;
					SetText( IDC_STATUS, L"Останавливаю…" );
				}
				return 0;
			}
			SaveSettings();
			DestroyWindow( hwnd );
			return 0;
		case WM_DESTROY:
			PostQuitMessage( 0 );
			return 0;
	}
	return DefWindowProcW( hwnd, msg, wp, lp );
}

int RunGui() {
	CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );
	INITCOMMONCONTROLSEX icc = { sizeof( icc ), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
	InitCommonControlsEx( &icc );

	HDC screen = GetDC( NULL );
	dpi = GetDeviceCaps( screen, LOGPIXELSY );
	ReleaseDC( NULL, screen );

	NONCLIENTMETRICSW ncm = {};
	ncm.cbSize = sizeof( ncm );
	SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof( ncm ), &ncm, 0 );
	uiFont = CreateFontIndirectW( &ncm.lfMessageFont );
	LOGFONTW bold = ncm.lfMessageFont;
	bold.lfWeight = FW_SEMIBOLD;
	boldFont = CreateFontIndirectW( &bold );
	logFont = CreateFontW( -MulDiv( 9, dpi, 72 ), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas" );

	logSink = GuiLogSink;
	progressSink = GuiProgressSink;

	HINSTANCE inst = GetModuleHandleW( NULL );
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof( wc );
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW( NULL, IDC_ARROW );
	wc.hbrBackground = GetSysColorBrush( COLOR_BTNFACE );
	wc.lpszClassName = L"n3dstexWindow";
	wc.hIcon = LoadIconW( inst, MAKEINTRESOURCEW( 1 ) );
	if ( wc.hIcon == NULL ) {
		wc.hIcon = LoadIconW( NULL, IDI_APPLICATION );
	}
	wc.hIconSm = wc.hIcon;
	RegisterClassExW( &wc );

	HWND hwnd = CreateWindowExW( 0, wc.lpszClassName, L"n3dstex — DOOM 3 BFG для New Nintendo 3DS: текстуры, звуки, ролики",
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S( 780 ), S( 760 ), NULL, NULL, inst, NULL );
	if ( hwnd == NULL ) {
		return 1;
	}
	ShowWindow( hwnd, SW_SHOWNORMAL );
	UpdateWindow( hwnd );

	MSG msg;
	while ( GetMessageW( &msg, NULL, 0, 0 ) > 0 ) {
		if ( !IsDialogMessageW( hwnd, &msg ) ) {
			TranslateMessage( &msg );
			DispatchMessageW( &msg );
		}
	}
	if ( worker.joinable() ) {
		worker.join();
	}
	CoUninitialize();
	return 0;
}

#endif
