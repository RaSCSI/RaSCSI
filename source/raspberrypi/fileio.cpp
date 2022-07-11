//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2001-2006 PI.(Twitter:@xm6_original)
//	Copyright (C) 2010-2021 GIMONS(Twitter:@kugimoto0715)
//
//	[ ファイルI/O(RaSCSI用サブセット) ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "rascsi.h"
#include "filepath.h"
#include "fileio.h"

//===========================================================================
//
//	ファイルI/O
//
//===========================================================================

#ifndef BAREMETAL
//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
Fileio::Fileio()
{
	// ワーク初期化
	handle = -1;
	m_bOpen = FALSE;
	m_szPath[0] = '\0';
	m_position = 0;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
Fileio::~Fileio()
{
	// 解放
	if (handle != -1) {
		close(handle);
		handle = -1;
	}
}

//---------------------------------------------------------------------------
//
//	ロード
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Load(const Filepath& path, void *buffer, int size)
{
	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(!m_bOpen);

	// オープン
	if (!Open(path, ReadOnly)) {
		return FALSE;
	}

	// 読み込み
	if (!Read(buffer, size)) {
		Close();
		return FALSE;
	}

	// クローズ
	Close();

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	セーブ
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Save(const Filepath& path, void *buffer, int size)
{
	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(!m_bOpen);

	// オープン
	if (!Open(path, WriteOnly)) {
		return FALSE;
	}

	// 書き込み
	if (!Write(buffer, size)) {
		Close();
		return FALSE;
	}

	// クローズ
	Close();

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Open(LPCTSTR fname, OpenMode mode)
{
	ASSERT(this);
	ASSERT(fname);
	ASSERT(!m_bOpen);

	// ヌル文字列からの読み込みは必ず失敗させる
	if (fname[0] == _T('\0')) {
		return FALSE;
	}

	// キャッシュファイルと同一か
	if (handle >= 0) {
		if (strcmp(fname, m_szPath) == 0 && mode == m_Mode) {
			m_bOpen = TRUE;
			return TRUE;
		}
	}

	// キャッシュファイルを解放
	close(handle);
	handle = -1;
	m_bOpen = FALSE;
	m_szPath[0] = '\0';
	m_position = 0;

	// モード別
	switch (mode) {
		// 読み込みのみ
		case ReadOnly:
			handle = open(fname, O_RDONLY);
			break;

		// 書き込みのみ
		case WriteOnly:
			handle = open(fname, O_CREAT | O_WRONLY | O_TRUNC, 0666);
			break;

		// 読み書き両方
		case ReadWrite:
			// CD-ROMからの読み込みはRWが成功してしまう
			if (access(fname, 0x06) != 0) {
				return FALSE;
			}
			handle = open(fname, O_RDWR);
			break;

		// アペンド
		case Append:
			handle = open(fname, O_CREAT | O_WRONLY | O_APPEND, 0666);
			break;

		// それ以外
		default:
			ASSERT(FALSE);
			break;
	}

	// 結果評価
	if (handle == -1) {
		return FALSE;
	}

	// 先読み無効
	if(posix_fadvise(handle, 0, 0, POSIX_FADV_RANDOM) != 0) {
		close(handle);
		return FALSE;
	}

	// パスとモードを保存
	m_bOpen = TRUE;
	strcpy(m_szPath, (LPTSTR)fname);
	m_Mode = mode;

	ASSERT(handle >= 0);
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Open(const Filepath& path, OpenMode mode)
{
	ASSERT(this);

	return Open(path.GetPath(), mode);
}

//---------------------------------------------------------------------------
//
//	1行読み込み
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::ReadLine(LPTSTR buffer, int size)
{
	int i;
	ssize_t ret;
	char c;

	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(m_bOpen);

	// 1文字ずつ読み込み
	i = 0;
	size--;
	while (i < size) {
		// 1文字読み込む
		ret = read(handle, &c, 1);

		// エラーもしくはEOF
		if (ret <= 0) {
			break;
		}

		// 文字格納
		buffer[i++] = c;

		// 位置加算
		m_position++;

		// 改行なら打ち切り
		if (c == '\n') {
			break;
		}
	}

	// 1文字も読み込むことが出来なかった
	if (i == 0) {
		return FALSE;
	}

	// NULLで終端
	buffer[i] = 0x00;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	読み込み
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Read(void *buffer, int size)
{
	int count;

	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(m_bOpen);

	// 読み込み
	count = read(handle, buffer, size);
	if (count != size) {
		return FALSE;
	}

	// 現在値を更新
	m_position += (fsize_t)size;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	書き込み
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Write(const void *buffer, int size)
{
	int count;

	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(m_bOpen);

	// 書き込み
	count = write(handle, buffer, size);
	if (count != size) {
		return FALSE;
	}

	// 現在値を更新
	m_position += (fsize_t)size;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	シーク
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Seek(fsize_t offset, BOOL relative)
{
	ASSERT(this);
	ASSERT(m_bOpen);
	ASSERT(offset >= 0);

	// 相対シークならオフセットに現在値を追加
	if (relative) {
		offset += m_position;
	}

	// オフセットに変更がなければシークしない
	if (offset == m_position) {
		return TRUE;
	}

	// シーク
	if (lseek(handle, offset, SEEK_SET) != offset) {
		return FALSE;
	}

	// 現在値を更新
	m_position = offset;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	ファイルサイズ取得
//
//---------------------------------------------------------------------------
fsize_t FASTCALL Fileio::GetFileSize()
{
	fsize_t end;

	ASSERT(this);
	ASSERT(m_bOpen);

	// ファイルサイズを64bitで取得
	end = lseek(handle, 0, SEEK_END);

	// 位置を元に戻す
	lseek(handle, m_position, SEEK_SET);

	return end;
}

//---------------------------------------------------------------------------
//
//	ファイル位置取得
//
//---------------------------------------------------------------------------
fsize_t FASTCALL Fileio::GetFilePos() const
{
	ASSERT(this);
	ASSERT(m_bOpen);

	// ファイル位置を64bitで取得
	return m_position;
}

//---------------------------------------------------------------------------
//
//	クローズ
//
//---------------------------------------------------------------------------
void FASTCALL Fileio::Close()
{
	ASSERT(this);
	ASSERT(m_bOpen);

	// 先頭にシーク
	lseek(handle, 0, SEEK_SET);
	m_position = 0;

	// クローズ
	m_bOpen = FALSE;
}
#else
//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
Fileio::Fileio()
{
	// ワーク初期化
	handle.obj.fs = 0;
	m_bOpen = FALSE;
	m_szPath[0] = '\0';
	m_position = 0;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
Fileio::~Fileio()
{
	// 解放
	if (handle.obj.fs) {
		f_close(&handle);
	}
}

//---------------------------------------------------------------------------
//
//	ロード
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Load(const Filepath& path, void *buffer, int size)
{
	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(!m_bOpen);

	// オープン
	if (!Open(path, ReadOnly)) {
		return FALSE;
	}

	// 読み込み
	if (!Read(buffer, size)) {
		Close();
		return FALSE;
	}

	// クローズ
	Close();

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	セーブ
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Save(const Filepath& path, void *buffer, int size)
{
	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(!m_bOpen);

	// オープン
	if (!Open(path, WriteOnly)) {
		return FALSE;
	}

	// 書き込み
	if (!Write(buffer, size)) {
		Close();
		return FALSE;
	}

	// クローズ
	Close();

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Open(LPCTSTR fname, OpenMode mode)
{
	FRESULT fr;
	Filepath fpath;

	ASSERT(this);
	ASSERT(fname);
	ASSERT(!m_bOpen);

	// ヌル文字列からの読み込みは必ず失敗させる
	if (fname[0] == _T('\0')) {
		return FALSE;
	}

	// キャッシュファイルと同一か
	if (handle.obj.fs) {
		if (strcmp(fname, m_szPath) == 0 && mode == m_Mode) {
			m_bOpen = TRUE;
			return TRUE;
		}
	}

	// キャッシュファイルを解放
	f_close(&handle);
	handle.obj.fs = 0;
	m_bOpen = FALSE;
	m_szPath[0] = '\0';
	m_position = 0;

	// モード別
	switch (mode) {
		// 読み込みのみ
		case ReadOnly:
			fr = f_open(&handle, fname, FA_READ);
			break;

		// 書き込みのみ
		case WriteOnly:
			fr = f_open(&handle, fname, FA_CREATE_ALWAYS | FA_WRITE);
			break;

		// 読み書き両方
		case ReadWrite:
			fr = f_open(&handle, fname, FA_READ | FA_WRITE);
			break;

		// アペンド
		case Append:
			fr = f_open(&handle, fname, FA_OPEN_APPEND | FA_WRITE);
			break;

		// それ以外
		default:
			fr = FR_NO_PATH;
			ASSERT(FALSE);
			break;
	}

	// 結果評価
	if (fr != FR_OK) {
		return FALSE;
	}

	// リンクマップテーブル生成
	handle.cltbl = cltbl;
	cltbl[0] = sizeof(cltbl) / sizeof(DWORD);
	fr = f_lseek(&handle, CREATE_LINKMAP);
	if (fr != FR_OK) {
		f_close(&handle);
		handle.obj.fs = 0;
		return FALSE;
	}

	// パスとモードを保存
	m_bOpen = TRUE;
	strcpy(m_szPath, (LPTSTR)fname);
	m_Mode = mode;

	// オープン成功
	ASSERT(handle.obj.fs);
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Open(const Filepath& path, OpenMode mode)
{
	ASSERT(this);

	return Open(path.GetPath(), mode);
}

//---------------------------------------------------------------------------
//
//	1行読み込み
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::ReadLine(LPTSTR buffer, int size)
{
	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(m_bOpen);

	// 読み込み
	if (f_gets(buffer, size -1, &handle) == NULL) {
		return FALSE;
	}

	// 現在値を更新
	m_position = (fsize_t)f_tell(&handle);

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	読み込み
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Read(void *buffer, int size)
{
	FRESULT fr;
	UINT count;

	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(m_bOpen);

	// 読み込み
	fr = f_read(&handle, buffer, size, &count);
	if (fr != FR_OK || count != (unsigned int)size) {
		return FALSE;
	}

	// 現在値を更新
	m_position += (fsize_t)size;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	書き込み
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Write(const void *buffer, int size)
{
	FRESULT fr;
	UINT count;

	ASSERT(this);
	ASSERT(buffer);
	ASSERT(size > 0);
	ASSERT(m_bOpen);

	// 書き込み
	fr = f_write(&handle, buffer, size, &count);
	if (fr != FR_OK || count != (unsigned int)size) {
		return FALSE;
	}

	// 現在値を更新
	m_position += (fsize_t)size;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	シーク
//
//---------------------------------------------------------------------------
BOOL FASTCALL Fileio::Seek(fsize_t offset, BOOL relative)
{
	FRESULT fr;

	ASSERT(this);
	ASSERT(m_bOpen);
	ASSERT(offset >= 0);

	// 相対シークならオフセットに現在値を追加
	if (relative) {
		offset += m_position;
	}

	// オフセットに変更がなければシークしない
	if (offset == m_position) {
		return TRUE;
	}

	// シーク
	fr = f_lseek(&handle, offset);
	if (fr != FR_OK) {
		return FALSE;
	}

	// 現在値を更新
	m_position = offset;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	ファイルサイズ取得
//
//---------------------------------------------------------------------------
fsize_t FASTCALL Fileio::GetFileSize()
{
	ASSERT(this);
	ASSERT(m_bOpen);

	return f_size(&handle);
}

//---------------------------------------------------------------------------
//
//	ファイル位置取得
//
//---------------------------------------------------------------------------
fsize_t FASTCALL Fileio::GetFilePos() const
{
	ASSERT(this);
	ASSERT(m_bOpen);

	return m_position;
}

//---------------------------------------------------------------------------
//
//	クローズ
//
//---------------------------------------------------------------------------
void FASTCALL Fileio::Close()
{
	ASSERT(this);
	ASSERT(m_bOpen);

	// 先頭にシーク
	f_lseek(&handle, 0);
	m_position = 0;

	// 同期するだけでキャッシュを残す
	f_sync(&handle);

	// クローズ
	m_bOpen = FALSE;
}
#endif	//BAREMETAL