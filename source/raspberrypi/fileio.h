//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2001-2005 PI.(Twitter:@xm6_original)
//	Copyright (C) 2013-2021 GIMONS(Twitter:@kugimoto0715)
//
//	[ ファイルI/O(RaSCSI用サブセット) ]
//
//---------------------------------------------------------------------------

#if !defined(fileio_h)
#define fileio_h

#include "os.h"
#include "rascsi.h"

#ifdef BAREMETAL
#include "ff.h"
#define fsize_t FSIZE_t
#else
#define fsize_t off_t
#endif	// BAREMETAL

//===========================================================================
//
//	ファイルI/O
//
//===========================================================================
class Fileio
{
public:
	enum OpenMode {
		ReadOnly,						// 読み込みのみ
		WriteOnly,						// 書き込みのみ
		ReadWrite,						// 読み書き両方
		Append							// アペンド
	};

public:
	Fileio();
										// コンストラクタ
	virtual ~Fileio();
										// デストラクタ
	BOOL FASTCALL Load(const Filepath& path, void *buffer, int size);
										// ROM,RAMロード
	BOOL FASTCALL Save(const Filepath& path, void *buffer, int size);
										// RAMセーブ

	BOOL FASTCALL Open(LPCTSTR fname, OpenMode mode);
										// オープン
	BOOL FASTCALL Open(const Filepath& path, OpenMode mode);
										// オープン
	BOOL FASTCALL Seek(fsize_t offset, BOOL relative = FALSE);
										// シーク
	BOOL FASTCALL ReadLine(LPTSTR buffer, int size);
										// 1行読み込み
	BOOL FASTCALL Read(void *buffer, int size);
										// 読み込み
	BOOL FASTCALL Write(const void *buffer, int size);
										// 書き込み
	fsize_t FASTCALL GetFileSize();
										// ファイルサイズ取得
	fsize_t FASTCALL GetFilePos() const;
										// ファイル位置取得
	void FASTCALL Close();
										// クローズ
#ifndef BAREMETAL
	BOOL FASTCALL IsValid() const		{ return (BOOL)(handle != -1); }
#else
	BOOL FASTCALL IsValid() const		{ return (BOOL)(handle.obj.fs != 0); }
#endif	// BAREMETAL
										// 有効チェック

private:
#ifndef BAREMETAL
	int handle;							// ファイルハンドル
#else
	FIL handle;							// ファイルハンドル

	DWORD cltbl[256];					// クラスタリンクマップテーブル

#endif	// BAREMETAL
	BOOL m_bOpen;
										// オープン状態
	TCHAR m_szPath[_MAX_PATH];
										// ファイルパス
	OpenMode m_Mode;
										// オープンモード
	fsize_t m_position;
										// カレントポジション
};

#endif	// fileio_h
