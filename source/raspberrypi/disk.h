//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2001-2006 PI.(Twitter:@xm6_original)
//	Copyright (C) 2014-2021 GIMONS(Twitter:@kugimoto0715)
//	Copyright (C) 2010-2015 isaki@NetBSD.org
//  Imported sava's Anex86/T98Next image and MO format support patch.
//
//	[ ディスク ]
//
//---------------------------------------------------------------------------

#if !defined(disk_h)
#define disk_h

//===========================================================================
//
//	動作定義
//
//===========================================================================
#define DEFAULT_VERSION	RASCSI			// INQUERYのバージョン番号
#define DEFAULT_VENDER	"RASCSI"		// INQUERYのベンダ名
#define USE_LOG_OUTPUT	1				// 1:printf出力
#define USE_WAIT_CTRL	1				// 1:タイミング調整有効
#define USE_BURST_BUS	1				// 1:データバースト送受信有効
#define USE_SYNC_TRANS	0				// 1:同期転送有効
#define USE_MZ1F23_1024_SUPPORT		1	// 1:MZ-1F23(20M/セクタサイズ1024)
#define REMOVE_FIXED_SASIHD_SIZE	1	// 1:SASIHDのサイズ固定制限を解除する
#define BRIDGE_PRODUCT	"RASCSI BRIDGE"	// ブリッジデバイスの製品名

//===========================================================================
//
//	ヘッダー
//
//===========================================================================
#include "filepath.h"
#include "fileio.h"

//===========================================================================
//
//	ログ
//
//===========================================================================
class Log
{
public:
	enum loglevel {
		Detail,							// 詳細レベル
		Normal,							// 通常レベル
		Warning,						// 警告レベル
		Debug							// デバッグレベル
	};
};

//===========================================================================
//
//	SASI/SCSI バス
//
//===========================================================================
class BUS
{
public:
	//	フェーズ定義
	enum phase_t {
		busfree,						// バスフリーフェーズ
		arbitration,					// アービトレーションフェーズ
		selection,						// セレクションフェーズ
		reselection,					// リセレクションフェーズ
		command,						// コマンドフェーズ
		execute,						// 実行フェーズ
		datain,							// データイン
		dataout,						// データアウト
		status,							// ステータスフェーズ
		msgin,							// メッセージフェーズ
		msgout,							// メッセージアウトフェーズ
		reserved						// 未使用/リザーブ
	};

	// 基本ファンクション
	virtual BOOL FASTCALL Init() = 0;
										// 初期化
	virtual void FASTCALL Reset() = 0;
										// リセット
	virtual void FASTCALL Cleanup() = 0;
										// クリーンアップ
	phase_t FASTCALL GetPhase()
	{
		DWORD mci;

		ASSERT(this);

		// セレクションフェーズ
		if (GetSEL()) {
			return selection;
		}

		// バスフリーフェーズ
		if (!GetBSY()) {
			return busfree;
		}

		// バスの信号線からターゲットのフェーズを取得
		mci = GetMSG() ? 0x04 : 0x00;
		mci |= GetCD() ? 0x02 : 0x00;
		mci |= GetIO() ? 0x01 : 0x00;
		return Mci2Phase(mci);
	}
										// フェーズ取得

	static phase_t FASTCALL Mci2Phase(DWORD mci)
	{
		static const phase_t phase_table[8] =
		{
			dataout,
			datain,
			command,
			status,
			reserved,
			reserved,
			msgout,
			msgin
		};

		return phase_table[mci];
	}
										// フェーズ取得

	virtual DWORD FASTCALL Aquire() const = 0;
										// 信号取り込み

	virtual BOOL FASTCALL GetBSY() const = 0;
										// BSYシグナル取得
	virtual void FASTCALL SetBSY(BOOL ast) = 0;
										// BSYシグナル設定

	virtual BOOL FASTCALL GetSEL() const = 0;
										// SELシグナル取得
	virtual void FASTCALL SetSEL(BOOL ast) = 0;
										// SELシグナル設定

	virtual BOOL FASTCALL GetATN() const = 0;
										// ATNシグナル取得
	virtual void FASTCALL SetATN(BOOL ast) = 0;
										// ATNシグナル設定

	virtual BOOL FASTCALL GetACK() const = 0;
										// ACKシグナル取得
	virtual void FASTCALL SetACK(BOOL ast) = 0;
										// ACKシグナル設定

	virtual BOOL FASTCALL GetRST() const = 0;
										// RSTシグナル取得
	virtual void FASTCALL SetRST(BOOL ast) = 0;
										// RSTシグナル設定

	virtual BOOL FASTCALL GetMSG() const = 0;
										// MSGシグナル取得
	virtual void FASTCALL SetMSG(BOOL ast) = 0;
										// MSGシグナル設定

	virtual BOOL FASTCALL GetCD() const = 0;
										// CDシグナル取得
	virtual void FASTCALL SetCD(BOOL ast) = 0;
										// CDシグナル設定

	virtual BOOL FASTCALL GetIO() const = 0;
										// IOシグナル取得
	virtual void FASTCALL SetIO(BOOL ast) = 0;
										// IOシグナル設定

	virtual BOOL FASTCALL GetREQ() const = 0;
										// REQシグナル取得
	virtual void FASTCALL SetREQ(BOOL ast) = 0;
										// REQシグナル設定

	virtual BYTE FASTCALL GetDAT() const = 0;
										// データシグナル取得
	virtual void FASTCALL SetDAT(BYTE dat) = 0;
										// データシグナル設定
	virtual BOOL FASTCALL GetDP() const = 0;
										// パリティシグナル取得

#if USE_BURST_BUS == 1
	virtual int FASTCALL CommandHandShake(BYTE *buf) = 0;
										// 一括コマンドハンドシェイク
	virtual int FASTCALL SendHandShake(
		BYTE *buf, int len, int syncoffset = 0) = 0;
										// 一括データ送信ハンドシェイク
	virtual int FASTCALL ReceiveHandShake(
		BYTE *buf, int len, int syncoffset = 0) = 0;
										// 一括データ受信ハンドシェイク
#endif	// USE_BURST_BUS
};

//---------------------------------------------------------------------------
//
//	エラー定義(REQUEST SENSEで返されるセンスコード)
//
//	MSB		予約(0x00)
//			センスキー
//			拡張センスコード(ASC)
//	LSB		拡張センスコードクォリファイア(ASCQ)
//
//---------------------------------------------------------------------------
#define DISK_NOERROR		0x00000000	// NO ADDITIONAL SENSE INFO.
#define DISK_DEVRESET		0x00062900	// POWER ON OR RESET OCCURED
#define DISK_NOTREADY		0x00023a00	// MEDIUM NOT PRESENT
#define DISK_ATTENTION		0x00062800	// MEDIUM MAY HAVE CHANGED
#define DISK_PREVENT		0x00045302	// MEDIUM REMOVAL PREVENTED
#define DISK_READFAULT		0x00031100	// UNRECOVERED READ ERROR
#define DISK_WRITEFAULT		0x00030300	// PERIPHERAL DEVICE WRITE FAULT
#define DISK_WRITEPROTECT	0x00042700	// WRITE PROTECTED
#define DISK_MISCOMPARE		0x000e1d00	// MISCOMPARE DURING VERIFY
#define DISK_INVALIDCMD		0x00052000	// INVALID COMMAND OPERATION CODE
#define DISK_INVALIDLBA		0x00052100	// LOGICAL BLOCK ADDR. OUT OF RANGE
#define DISK_INVALIDCDB		0x00052400	// INVALID FIELD IN CDB
#define DISK_INVALIDLUN		0x00052500	// LOGICAL UNIT NOT SUPPORTED
#define DISK_INVALIDPRM		0x00052600	// INVALID FIELD IN PARAMETER LIST
#define DISK_INVALIDMSG		0x00054900	// INVALID MESSAGE ERROR
#define DISK_PARAMLEN		0x00051a00	// PARAMETERS LIST LENGTH ERROR
#define DISK_PARAMNOT		0x00052601	// PARAMETERS NOT SUPPORTED
#define DISK_PARAMVALUE		0x00052602	// PARAMETERS VALUE INVALID
#define DISK_PARAMSAVE		0x00053900	// SAVING PARAMETERS NOT SUPPORTED
#define DISK_NODEFECT		0x00010000	// DEFECT LIST NOT FOUND

#if 0
#define DISK_AUDIOPROGRESS	0x00??0011	// AUDIO PLAY IN PROGRESS
#define DISK_AUDIOPAUSED	0x00??0012	// AUDIO PLAY PAUSED
#define DISK_AUDIOSTOPPED	0x00??0014	// AUDIO PLAY STOPPED DUE TO ERROR
#define DISK_AUDIOCOMPLETE	0x00??0013	// AUDIO PLAY SUCCESSFULLY COMPLETED
#endif

//===========================================================================
//
//	ディスクトラック
//
//===========================================================================
class Disk;
class DiskTrack
{
public:
	enum {
		NumSectors = 32					// トラック毎のセクタ数
	};

	// 内部データ定義
	typedef struct {
		int track;						// トラックナンバー
		int size;						// セクタサイズ(8 or 9)
		int sectors;					// セクタ数(<=0x100)
		DWORD length;					// データバッファ長
		BYTE *buffer;					// データバッファ
		BOOL init;						// ロード済みか
		BOOL changed;					// 変更済みフラグ
		DWORD maplen;					// 変更済みマップ長
		BOOL *changemap;				// 変更済みマップ
		BOOL raw;						// RAWモード
		fsize_t imgoffset;				// 実データまでのオフセット
	} disktrk_t;

public:
	// 基本ファンクション
	DiskTrack();
										// コンストラクタ
	virtual ~DiskTrack();
										// デストラクタ
	void FASTCALL Init(
		Disk *p, int track, int size, int sectors,
		BOOL raw = FALSE, fsize_t imgoff = 0);
										// 初期化
	BOOL FASTCALL Load();
										// ロード
	BOOL FASTCALL Save();
										// セーブ

	// リード・ライト
	BOOL FASTCALL Read(BYTE *buf, int sec) const;
										// セクタリード
	BOOL FASTCALL Write(const BYTE *buf, int sec);
										// セクタライト

	// その他
	int FASTCALL GetTrack() const		{ return dt.track; }
										// トラック取得
	BOOL FASTCALL IsChanged() const		{ return dt.changed; }
										// 変更フラグチェック

private:
	// 内部データ
	Disk *disk;
										// ディスク
	disktrk_t dt;
										// 内部データ
};

//===========================================================================
//
//	ディスクキャッシュ
//
//===========================================================================
class DiskCache
{
public:
	// 内部データ定義
	typedef struct {
		DiskTrack *disktrk;				// 割り当てトラック
		DWORD serial;					// 最終シリアル
	} cache_t;

	// キャッシュ数
	enum {
		CacheMax = 16
	};

public:
	// 基本ファンクション
	DiskCache(Disk *p, int size, int blocks, fsize_t imgoff = 0);
										// コンストラクタ
	virtual ~DiskCache();
										// デストラクタ
	void FASTCALL SetRawMode(BOOL raw);
										// CD-ROM rawモード設定

	// アクセス
	BOOL FASTCALL Save();
										// 全セーブ＆解放
	BOOL FASTCALL Read(BYTE *buf, int block);
										// セクタリード
	BOOL FASTCALL Write(const BYTE *buf, int block);
										// セクタライト
	BOOL FASTCALL GetCache(int index, int& track, DWORD& serial) const;
										// キャッシュ情報取得

private:
	// 内部管理
	void FASTCALL Clear();
										// トラックをすべてクリア
	DiskTrack* FASTCALL Assign(int track);
										// トラックのロード
	BOOL FASTCALL Load(int index, int track, DiskTrack *disktrk = NULL);
										// トラックのロード
	void FASTCALL Update();
										// シリアル番号更新

	// 内部データ
	Disk *disk;
										// ディスク
	cache_t cache[CacheMax];
										// キャッシュ管理
	DWORD serial;
										// 最終アクセスシリアルナンバ
	int sec_size;
										// セクタサイズ(8 or 9 or 11)
	int sec_blocks;
										// セクタブロック数
	BOOL cd_raw;
										// CD-ROM RAWモード
	fsize_t imgoffset;
										// 実データまでのオフセット
};

//===========================================================================
//
//	ディスク
//
//===========================================================================
class Disk
{
public:
	// 内部ワーク
	typedef struct {
		DWORD id;						// メディアID
		BOOL ready;						// 有効なディスク
		BOOL writep;					// 書き込み禁止
		BOOL readonly;					// 読み込み専用
		BOOL removable;					// 取り外し
		BOOL lock;						// ロック
		BOOL attn;						// アテンション
		BOOL reset;						// リセット
		int size;						// セクタサイズ
		DWORD blocks;					// 総セクタ数
		DWORD lun;						// LUN
		DWORD code;						// ステータスコード
		DiskCache *dcache;				// ディスクキャッシュ
		fsize_t imgoffset;				// 実データまでのオフセット
	} disk_t;

public:
	// 基本ファンクション
	Disk();
										// コンストラクタ
	virtual ~Disk();
										// デストラクタ
	virtual void FASTCALL Reset();
										// デバイスリセット

	// ID
	DWORD FASTCALL GetID() const		{ return disk.id; }
										// メディアID取得
	BOOL FASTCALL IsNULL() const;
										// NULLチェック
	BOOL FASTCALL IsSASI() const;
										// SASIチェック

	// メディア操作
	virtual BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン
	void FASTCALL GetPath(Filepath& path) const;
										// パス取得
	void FASTCALL Eject(BOOL force);
										// イジェクト
	BOOL FASTCALL IsReady() const		{ return disk.ready; }
										// Readyチェック
	void FASTCALL WriteP(BOOL flag);
										// 書き込み禁止
	BOOL FASTCALL IsWriteP() const		{ return disk.writep; }
										// 書き込み禁止チェック
	BOOL FASTCALL IsReadOnly() const	{ return disk.readonly; }
										// Read Onlyチェック
	BOOL FASTCALL IsRemovable() const	{ return disk.removable; }
										// リムーバブルチェック
	BOOL FASTCALL IsLocked() const		{ return disk.lock; }
										// ロックチェック
	BOOL FASTCALL IsAttn() const		{ return disk.attn; }
										// 交換チェック
	BOOL FASTCALL Flush();
										// キャッシュフラッシュ
	void FASTCALL GetDisk(disk_t *buffer) const;
										// 内部ワーク取得

	// プロパティ
	void FASTCALL SetLUN(DWORD lun)		{ disk.lun = lun; }
										// LUNセット
	DWORD FASTCALL GetLUN()				{ return disk.lun; }
										// LUN取得
	// コマンド
	virtual int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド
	virtual int FASTCALL RequestSense(const DWORD *cdb, BYTE *buf);
										// REQUEST SENSEコマンド
	int FASTCALL SelectCheck(const DWORD *cdb);
										// SELECTチェック
	int FASTCALL SelectCheck10(const DWORD *cdb);
										// SELECT(10)チェック
	virtual BOOL FASTCALL ModeSelect(const DWORD *cdb, const BYTE *buf, int length);
										// MODE SELECTコマンド
	virtual int FASTCALL ModeSense(const DWORD *cdb, BYTE *buf);
										// MODE SENSEコマンド
	virtual int FASTCALL ModeSense10(const DWORD *cdb, BYTE *buf);
										// MODE SENSE(10)コマンド
	int FASTCALL ReadDefectData10(const DWORD *cdb, BYTE *buf);
										// READ DEFECT DATA(10)コマンド
	virtual BOOL FASTCALL TestUnitReady(const DWORD *cdb);
										// TEST UNIT READYコマンド
	BOOL FASTCALL Rezero(const DWORD *cdb);
										// REZEROコマンド
	BOOL FASTCALL Format(const DWORD *cdb);
										// FORMAT UNITコマンド
	BOOL FASTCALL Reassign(const DWORD *cdb);
										// REASSIGN UNITコマンド
	virtual int FASTCALL Read(BYTE *buf, DWORD block);
										// READコマンド
	int FASTCALL WriteCheck(DWORD block);
										// WRITEチェック
	BOOL FASTCALL Write(const BYTE *buf, DWORD block);
										// WRITEコマンド
	BOOL FASTCALL Seek(const DWORD *cdb);
										// SEEKコマンド
	BOOL FASTCALL Assign(const DWORD *cdb);
										// ASSIGNコマンド
	BOOL FASTCALL Specify(const DWORD *cdb);
										// SPECIFYコマンド
	BOOL FASTCALL StartStop(const DWORD *cdb);
										// START STOP UNITコマンド
	BOOL FASTCALL SendDiag(const DWORD *cdb);
										// SEND DIAGNOSTICコマンド
	BOOL FASTCALL Removal(const DWORD *cdb);
										// PREVENT/ALLOW MEDIUM REMOVALコマンド
	int FASTCALL ReadCapacity(const DWORD *cdb, BYTE *buf);
										// READ CAPACITYコマンド
	BOOL FASTCALL Verify(const DWORD *cdb);
										// VERIFYコマンド
	virtual int FASTCALL ReadToc(const DWORD *cdb, BYTE *buf);
										// READ TOCコマンド
	virtual BOOL FASTCALL PlayAudio(const DWORD *cdb);
										// PLAY AUDIOコマンド
	virtual BOOL FASTCALL PlayAudioMSF(const DWORD *cdb);
										// PLAY AUDIO MSFコマンド
	virtual BOOL FASTCALL PlayAudioTrack(const DWORD *cdb);
										// PLAY AUDIO TRACKコマンド
	void FASTCALL InvalidCmd()			{ disk.code = DISK_INVALIDCMD; }
										// サポートしていないコマンド

	// その他
	BOOL FASTCALL IsCacheWB() { return cache_wb; }
										// キャッシュモード取得
	void FASTCALL SetCacheWB(BOOL enable) { cache_wb = enable; }
										// キャッシュモード設定
	Fileio* FASTCALL GetFio() { return &fio; };
										// ファイルIO取得

protected:
	// サブ処理
	virtual int FASTCALL AddError(BOOL change, BYTE *buf);
										// エラーページ追加
	virtual int FASTCALL AddFormat(BOOL change, BYTE *buf);
										// フォーマットページ追加
	virtual int FASTCALL AddDrive(BOOL change, BYTE *buf);
										// ドライブページ追加
	int FASTCALL AddOpt(BOOL change, BYTE *buf);
										// オプティカルページ追加
	int FASTCALL AddCache(BOOL change, BYTE *buf);
										// キャッシュページ追加
	int FASTCALL AddCDROM(BOOL change, BYTE *buf);
										// CD-ROMページ追加
	int FASTCALL AddCDDA(BOOL change, BYTE *buf);
										// CD-DAページ追加
	virtual int FASTCALL AddVendor(int page, BOOL change, BYTE *buf);
										// ベンダ特殊ページ追加
	BOOL FASTCALL CheckReady();
										// レディチェック

	// 内部データ
	disk_t disk;
										// ディスク内部データ
	Filepath diskpath;
										// パス(GetPath用)
	BOOL cache_wb;
										// キャッシュモード
	Fileio fio;
										// ファイルIO
};

//===========================================================================
//
//	SASI ハードディスク
//
//===========================================================================
class SASIHD : public Disk
{
public:
	// 基本ファンクション
	SASIHD();
										// コンストラクタ
	void FASTCALL Reset();
										// リセット
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン
	// コマンド
	int FASTCALL RequestSense(const DWORD *cdb, BYTE *buf);
										// REQUEST SENSEコマンド
};

//===========================================================================
//
//	SCSI ハードディスク
//
//===========================================================================
class SCSIHD : public Disk
{
public:
	// 基本ファンクション
	SCSIHD();
										// コンストラクタ
	void FASTCALL Reset();
										// リセット
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン

	// コマンド
	int FASTCALL Inquiry(
		const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド
	BOOL FASTCALL ModeSelect(const DWORD *cdb, const BYTE *buf, int length);
										// MODE SELECT(6)コマンド
};

//===========================================================================
//
//	SCSI ハードディスク(PC-9801-55 NEC純正/Anex86/T98Next)
//
//===========================================================================
class SCSIHD_NEC : public SCSIHD
{
public:
	// 基本ファンクション
	SCSIHD_NEC();
										// コンストラクタ

	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン

	// コマンド
	int FASTCALL Inquiry(
		const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド

	// サブ処理
	int FASTCALL AddError(BOOL change, BYTE *buf);
										// エラーページ追加
	int FASTCALL AddFormat(BOOL change, BYTE *buf);
										// フォーマットページ追加
	int FASTCALL AddDrive(BOOL change, BYTE *buf);
										// ドライブページ追加

private:
	int cylinders;
										// シリンダ数
	int heads;
										// ヘッド数
	int sectors;
										// セクタ数
	int sectorsize;
										// セクタサイズ
	fsize_t imgoffset;
										// イメージオフセット
	fsize_t imgsize;
										// イメージサイズ
};

//===========================================================================
//
//	SCSI ハードディスク(Macintosh Apple純正)
//
//===========================================================================
class SCSIHD_APPLE : public SCSIHD
{
public:
	// 基本ファンクション
	SCSIHD_APPLE();
										// コンストラクタ
	// コマンド
	int FASTCALL Inquiry(
		const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド

	// サブ処理
	int FASTCALL AddVendor(int page, BOOL change, BYTE *buf);
										// ベンダ特殊ページ追加
};

//===========================================================================
//
//	SCSI 光磁気ディスク
//
//===========================================================================
class SCSIMO : public Disk
{
public:
	// 基本ファンクション
	SCSIMO();
										// コンストラクタ
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン

	// コマンド
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド
	BOOL FASTCALL ModeSelect(const DWORD *cdb, const BYTE *buf, int length);
										// MODE SELECT(6)コマンド

	// サブ処理
	int FASTCALL AddVendor(int page, BOOL change, BYTE *buf);
										// ベンダ特殊ページ追加
};

//---------------------------------------------------------------------------
//
//	クラス先行定義
//
//---------------------------------------------------------------------------
class SCSICD;

//===========================================================================
//
//	CD-ROM トラック
//
//===========================================================================
class CDTrack
{
public:
	// 基本ファンクション
	CDTrack(SCSICD *scsicd);
										// コンストラクタ
	virtual ~CDTrack();
										// デストラクタ
	BOOL FASTCALL Init(int track, DWORD first, DWORD last);
										// 初期化

	// プロパティ
	void FASTCALL SetPath(BOOL cdda, const Filepath& path);
										// パス設定
	void FASTCALL GetPath(Filepath& path) const;
										// パス取得
	void FASTCALL AddIndex(int index, DWORD lba);
										// インデックス追加
	DWORD FASTCALL GetFirst() const;
										// 開始LBA取得
	DWORD FASTCALL GetLast() const;
										// 終端LBA取得
	DWORD FASTCALL GetBlocks() const;
										// ブロック数取得
	int FASTCALL GetTrackNo() const;
										// トラック番号取得
	BOOL FASTCALL IsValid(DWORD lba) const;
										// 有効なLBAか
	BOOL FASTCALL IsAudio() const;
										// オーディオトラックか

private:
	SCSICD *cdrom;
										// 親デバイス
	BOOL valid;
										// 有効なトラック
	int track_no;
										// トラック番号
	DWORD first_lba;
										// 開始LBA
	DWORD last_lba;
										// 終了LBA
	BOOL audio;
										// オーディオトラックフラグ
	BOOL raw;
										// RAWデータフラグ
	Filepath imgpath;
										// イメージファイルパス
};

//===========================================================================
//
//	CD-DA バッファ
//
//===========================================================================
class CDDABuf
{
public:
	// 基本ファンクション
	CDDABuf();
										// コンストラクタ
	virtual ~CDDABuf();
										// デストラクタ
#if 0
	BOOL Init();
										// 初期化
	BOOL FASTCALL Load(const Filepath& path);
										// ロード
	BOOL FASTCALL Save(const Filepath& path);
										// セーブ

	// API
	void FASTCALL Clear();
										// バッファクリア
	BOOL FASTCALL Open(Filepath& path);
										// ファイル指定
	BOOL FASTCALL GetBuf(DWORD *buffer, int frames);
										// バッファ取得
	BOOL FASTCALL IsValid();
										// 有効チェック
	BOOL FASTCALL ReadReq();
										// 読み込み要求
	BOOL FASTCALL IsEnd() const;
										// 終了チェック

private:
	Filepath wavepath;
										// Waveパス
	BOOL valid;
										// オープン結果
	DWORD *buf;
										// データバッファ
	DWORD read;
										// Readポインタ
	DWORD write;
										// Writeポインタ
	DWORD num;
										// データ有効数
	DWORD rest;
										// ファイル残りサイズ
#endif
};

//===========================================================================
//
//	SCSI CD-ROM
//
//===========================================================================
class SCSICD : public Disk
{
public:
	// トラック数
	enum {
		TrackMax = 96					// トラック最大数
	};

public:
	// 基本ファンクション
	SCSICD();
										// コンストラクタ
	virtual ~SCSICD();
										// デストラクタ
	BOOL FASTCALL Open(const Filepath& path, BOOL attn = TRUE);
										// オープン

	// コマンド
	int FASTCALL Inquiry(const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド
	int FASTCALL Read(BYTE *buf, DWORD block);
										// READコマンド
	int FASTCALL ReadToc(const DWORD *cdb, BYTE *buf);
										// READ TOCコマンド
	BOOL FASTCALL PlayAudio(const DWORD *cdb);
										// PLAY AUDIOコマンド
	BOOL FASTCALL PlayAudioMSF(const DWORD *cdb);
										// PLAY AUDIO MSFコマンド
	BOOL FASTCALL PlayAudioTrack(const DWORD *cdb);
										// PLAY AUDIO TRACKコマンド

	// CD-DA
	BOOL FASTCALL NextFrame();
										// フレーム通知
	void FASTCALL GetBuf(DWORD *buffer, int samples, DWORD rate);
										// CD-DAバッファ取得

	// LBA-MSF変換
	void FASTCALL LBAtoMSF(DWORD lba, BYTE *msf) const;
										// LBA→MSF変換
	DWORD FASTCALL MSFtoLBA(const BYTE *msf) const;
										// MSF→LBA変換

private:
	// オープン
	BOOL FASTCALL OpenCue(const Filepath& path);
										// オープン(CUE)
	BOOL FASTCALL OpenIso(const Filepath& path);
										// オープン(ISO)
	BOOL FASTCALL OpenPhysical(const Filepath& path);
										// オープン(Physical)
	BOOL rawfile;
										// RAWフラグ

	// トラック管理
	void FASTCALL ClearTrack();
										// トラッククリア
	int FASTCALL SearchTrack(DWORD lba) const;
										// トラック検索
	CDTrack* track[TrackMax];
										// トラックオブジェクト
	int tracks;
										// トラックオブジェクト有効数
	int dataindex;
										// 現在のデータトラック
	int audioindex;
										// 現在のオーディオトラック

	int frame;
										// フレーム番号

#if 0
	CDDABuf da_buf;
										// CD-DAバッファ
	int da_num;
										// CD-DAトラック数
	int da_cur;
										// CD-DAカレントトラック
	int da_next;
										// CD-DAネクストトラック
	BOOL da_req;
										// CD-DAデータ要求
#endif
};

//===========================================================================
//
//	SCSI ホストブリッジ
//
//===========================================================================
class SCSIBR : public Disk
{
public:
	// 基本ファンクション
	SCSIBR();
										// コンストラクタ
	virtual ~SCSIBR();
										// デストラクタ

	// コマンド
	int FASTCALL Inquiry(
		const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor);
										// INQUIRYコマンド
	BOOL FASTCALL TestUnitReady(const DWORD *cdb);
										// TEST UNIT READYコマンド
	int FASTCALL GetMessage10(const DWORD *cdb, BYTE *buf);
										// GET MESSAGE10コマンド
	int FASTCALL SendMessage10(const DWORD *cdb, BYTE *buf);
										// SEND MESSAGE10コマンド

	// メッセージハンドラ関数
	typedef int (*MsgFunc)(BOOL read, int func, int phase, int len, BYTE *);
										// メッセージハンドラ関数宣言

	void FASTCALL SetMsgFunc(int type, MsgFunc f);
										// メッセージハンドラ関数登録

private:
	MsgFunc pMsgFunc[8];
										// メッセージハンドラ関数ポインタ
};

//===========================================================================
//
//	SASI コントローラ
//
//===========================================================================
class SASIDEV
{
public:
	// 論理ユニット最大数
	enum {
		UnitMax = 8
	};

#if USE_WAIT_CTRL == 1
	// タイミング調整用
	enum {
		min_status_time =	20,
		min_exec_time =		100,
		min_data_time =		200
	};
#endif	// USE_WAIT_CTRL

	// 内部データ定義
	typedef struct {
		// 全般
		BUS::phase_t phase;				// 遷移フェーズ
		int id;							// コントローラID(0-7)
		BUS *bus;						// バス

		// コマンド
		DWORD cmd[10];					// コマンドデータ
		DWORD status;					// ステータスデータ
		DWORD message;					// メッセージデータ

#if USE_WAIT_CTRL == 1
		// 実行
		DWORD execstart;				// 実行開始時間
#endif	// USE_WAIT_CTRL

		// 転送
		BYTE *buffer;					// 転送バッファ
		int bufsize;					// 転送バッファサイズ
		DWORD blocks;					// 転送ブロック数
		DWORD next;						// 次のレコード
		DWORD offset;					// 転送オフセット
		DWORD length;					// 転送残り長さ

		// 論理ユニット
		Disk *unit[UnitMax];
										// 論理ユニット
	} ctrl_t;

public:
	// 基本ファンクション
	SASIDEV();
										// コンストラクタ
	virtual ~SASIDEV();
										// デストラクタ
	virtual void FASTCALL Reset();
										// デバイスリセット

	// 外部API
	virtual BUS::phase_t FASTCALL Process();
										// 実行

	// 接続
	void FASTCALL Connect(int id, BUS *sbus);
										// コントローラ接続
	Disk* FASTCALL GetUnit(int no);
										// 論理ユニット取得
	void FASTCALL SetUnit(int no, Disk *dev);
										// 論理ユニット設定
	BOOL FASTCALL HasUnit();
										// 有効な論理ユニットを持っているか返す

	// その他
	BUS::phase_t FASTCALL GetPhase() {return ctrl.phase;}
										// フェーズ取得
	int FASTCALL GetID() {return ctrl.id;}
										// ID取得
	void FASTCALL GetCTRL(ctrl_t *buffer);
										// 内部情報取得
	ctrl_t* FASTCALL GetWorkAddr() { return &ctrl; }
										// 内部情報アドレス取得
	virtual BOOL FASTCALL IsSASI() const {return TRUE;}
										// SASIチェック
	virtual BOOL FASTCALL IsSCSI() const {return FALSE;}
										// SCSIチェック
	Disk* FASTCALL GetBusyUnit();
										// ビジー状態のユニットを取得

protected:
	// フェーズ処理
	virtual void FASTCALL BusFree();
										// バスフリーフェーズ
	virtual void FASTCALL Selection();
										// セレクションフェーズ
	virtual void FASTCALL Command();
										// コマンドフェーズ
	virtual void FASTCALL Execute();
										// 実行フェーズ
	void FASTCALL Status();
										// ステータスフェーズ
	void FASTCALL MsgIn();
										// メッセージインフェーズ
	void FASTCALL DataIn();
										// データインフェーズ
	void FASTCALL DataOut();
										// データアウトフェーズ
	virtual void FASTCALL Error();
										// 共通エラー処理

	// コマンド
	void FASTCALL CmdTestUnitReady();
										// TEST UNIT READYコマンド
	void FASTCALL CmdRezero();
										// REZERO UNITコマンド
	void FASTCALL CmdRequestSense();
										// REQUEST SENSEコマンド
	void FASTCALL CmdFormat();
										// FORMATコマンド
	void FASTCALL CmdReassign();
										// REASSIGN BLOCKSコマンド
	void FASTCALL CmdRead6();
										// READ(6)コマンド
	void FASTCALL CmdWrite6();
										// WRITE(6)コマンド
	void FASTCALL CmdSeek6();
										// SEEK(6)コマンド
	void FASTCALL CmdAssign();
										// ASSIGNコマンド
	void FASTCALL CmdSpecify();
										// SPECIFYコマンド
	void FASTCALL CmdInvalid();
										// サポートしていないコマンド

	// データ転送
	virtual void FASTCALL Send();
										// データ送信
	virtual void FASTCALL SendNext();
										// データ送信継続
	virtual void FASTCALL Receive();
										// データ受信
	virtual void FASTCALL ReceiveNext();
										// データ受信継続

#if USE_BURST_BUS == 1
	virtual void FASTCALL SendBurst();
										// バースト送信
	virtual void FASTCALL ReceiveBurst();
										// バースト受信
#endif	// USE_BURST_BUS

	BOOL FASTCALL XferIn(BYTE* buf);
										// データ転送IN
	BOOL FASTCALL XferOut(BOOL cont);
										// データ転送OUT

	// 特殊
	void FASTCALL FlushUnit();
										// 論理ユニットフラッシュ

	// ログ
	void FASTCALL Log(Log::loglevel level, const char *format, ...);
										// ログ出力

protected:
	ctrl_t ctrl;
										// 内部データ
};

//===========================================================================
//
//	SCSI デバイス(SASI デバイスを継承)
//
//===========================================================================
class SCSIDEV : public SASIDEV
{
public:
	// 内部データ定義
	typedef struct {
		// 同期転送
		BOOL syncenable;				// 同期転送可能
		int syncperiod;					// 同期転送ピリオド
		int syncoffset;					// 同期転送オフセット

		// ATNメッセージ
		BOOL atnmsg;
		int msc;
		BYTE msb[256];
	} scsi_t;

	enum {
		SYNCPERIOD = 50,
		SYNCOFFSET = 16
	};

public:
	// 基本ファンクション
	SCSIDEV();
										// コンストラクタ

	void FASTCALL Reset();
										// デバイスリセット

	// 外部API
	BUS::phase_t FASTCALL Process();
										// 実行

	// その他
	BOOL FASTCALL IsSASI() const {return FALSE;}
										// SASIチェック
	BOOL FASTCALL IsSCSI() const {return TRUE;}
										// SCSIチェック

private:
	// フェーズ
	void FASTCALL BusFree();
										// バスフリーフェーズ
	void FASTCALL Selection();
										// セレクションフェーズ
	void FASTCALL Execute();
										// 実行フェーズ
	void FASTCALL MsgOut();
										// メッセージアウトフェーズ
	void FASTCALL Error();
										// 共通エラー処理

	// コマンド
	void FASTCALL CmdInquiry();
										// INQUIRYコマンド
	void FASTCALL CmdModeSelect();
										// MODE SELECTコマンド
	void FASTCALL CmdModeSense();
										// MODE SENSEコマンド
	void FASTCALL CmdStartStop();
										// START STOP UNITコマンド
	void FASTCALL CmdSendDiag();
										// SEND DIAGNOSTICコマンド
	void FASTCALL CmdRemoval();
										// PREVENT/ALLOW MEDIUM REMOVALコマンド
	void FASTCALL CmdReadCapacity();
										// READ CAPACITYコマンド
	void FASTCALL CmdRead10();
										// READ(10)コマンド
	void FASTCALL CmdWrite10();
										// WRITE(10)コマンド
	void FASTCALL CmdSeek10();
										// SEEK(10)コマンド
	void FASTCALL CmdVerify();
										// VERIFYコマンド
	void FASTCALL CmdSynchronizeCache();
										// SYNCHRONIZE CACHE コマンド
	void FASTCALL CmdReadDefectData10();
										// READ DEFECT DATA(10) コマンド
	void FASTCALL CmdReadToc();
										// READ TOCコマンド
	void FASTCALL CmdPlayAudio10();
										// PLAY AUDIO(10)コマンド
	void FASTCALL CmdPlayAudioMSF();
										// PLAY AUDIO MSFコマンド
	void FASTCALL CmdPlayAudioTrack();
										// PLAY AUDIO TRACK INDEXコマンド
	void FASTCALL CmdModeSelect10();
										// MODE SELECT(10)コマンド
	void FASTCALL CmdModeSense10();
										// MODE SENSE(10)コマンド
	void FASTCALL CmdGetMessage10();
										// GET MESSAGE(10)コマンド
	void FASTCALL CmdSendMessage10();
										// SEND MESSAGE(10)コマンド

	// データ転送
	void FASTCALL Send();
										// データ送信
	void FASTCALL SendNext();
										// データ送信継続
	void FASTCALL Receive();
										// データ受信
	void FASTCALL ReceiveNext();
										// データ受信継続
#if USE_BURST_BUS == 1
	void FASTCALL SendBurst();
										// バースト送信
	void FASTCALL ReceiveBurst();
										// バースト受信
#endif	// USE_BURST_BUS

	BOOL FASTCALL XferMsg(DWORD msg);
										// データ転送MSG

	scsi_t scsi;
										// 内部データ
};

#endif	// disk_h
