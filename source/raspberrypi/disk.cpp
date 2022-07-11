//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2001-2006 PI.(Twitter:@xm6_original)
//	Copyright (C) 2014-2021 GIMONS(Twitter:@kugimoto0715)
//	Copyright (C) 2010-2015 isaki@NetBSD.org
//	Copyright (C) 2010 Y.Sugahara
//	Imported sava's Anex86/T98Next image and MO format support patch.
//	Imported NetBSD support and some optimisation patch by Rin Okuyama.
//
//	[ ディスク ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "rascsi.h"
#include "filepath.h"
#include "fileio.h"
#include "disk.h"

#if USE_WAIT_CTRL == 1
#ifdef __cplusplus
extern "C" {
#endif
DWORD GetTimeUs();
void SleepUs(int us);
#ifdef __cplusplus
}
#endif
#endif	// USE_WAIT_CTRL

//===========================================================================
//
//	ディスク
//
//===========================================================================
//#define DISK_LOG
//#define DISK_LOG_WARNING

//===========================================================================
//
//	ディスクトラック
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
DiskTrack::DiskTrack()
{
	// 内部情報の初期化
	dt.track = 0;
	dt.size = 0;
	dt.sectors = 0;
	dt.raw = FALSE;
	dt.init = FALSE;
	dt.changed = FALSE;
	dt.length = 0;
	dt.buffer = NULL;
	dt.maplen = 0;
	dt.changemap = NULL;
	dt.imgoffset = 0;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
DiskTrack::~DiskTrack()
{
	// メモリ解放は行うが、自動セーブはしない
	if (dt.buffer) {
		free(dt.buffer);
		dt.buffer = NULL;
	}
	if (dt.changemap) {
		free(dt.changemap);
		dt.changemap = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	初期化
//
//---------------------------------------------------------------------------
void FASTCALL DiskTrack::Init(
	Disk *p, int track, int size, int sectors, BOOL raw, fsize_t imgoff)
{
	ASSERT(p);
	ASSERT(track >= 0);
	ASSERT((size >= 8) && (size <= 11));
	ASSERT((sectors > 0) && (sectors <= NumSectors));
	ASSERT(imgoff >= 0);

	// ディスク
	disk = p;

	// パラメータを設定
	dt.track = track;
	dt.size = size;
	dt.sectors = sectors;
	dt.raw = raw;

	// 初期化されていない(ロードする必要あり)
	dt.init = FALSE;

	// 変更されていない
	dt.changed = FALSE;

	// 実データまでのオフセット
	dt.imgoffset = imgoff;
}

//---------------------------------------------------------------------------
//
//	ロード
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Load()
{
	fsize_t offset;
	int i;
	int length;
	Fileio *fio;
	Filepath path;

	ASSERT(this);

	// 既にロードされていれば不要
	if (dt.init) {
		ASSERT(dt.buffer);
		ASSERT(dt.changemap);
		return TRUE;
	}

	// オフセットを計算
	// これ以前のトラックは固定のセクタ数(NumSectors)を保持とみなす
	offset = ((fsize_t)dt.track * NumSectors);
	if (dt.raw) {
		ASSERT(dt.size == 11);
		offset *= 0x930;
		offset += 0x10;
	} else {
		offset <<= dt.size;
	}

	// 実イメージまでのオフセットを追加
	offset += dt.imgoffset;

	// レングスを計算(このトラックのデータサイズ)
	length = dt.sectors << dt.size;

	// バッファのメモリを確保
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= NumSectors));

	if (dt.buffer == NULL) {
		dt.buffer = (BYTE *)malloc(length * sizeof(BYTE));
		dt.length = length;
	}

	if (!dt.buffer) {
		return FALSE;
	}

	// バッファ長が異なるなら再確保
	if (dt.length != (DWORD)length) {
		free(dt.buffer);
		dt.buffer = (BYTE *)malloc(length * sizeof(BYTE));
		dt.length = length;
	}

	// 変更マップのメモリを確保
	if (dt.changemap == NULL) {
		dt.changemap = (BOOL *)malloc(dt.sectors * sizeof(BOOL));
		dt.maplen = dt.sectors;
	}

	if (!dt.changemap) {
		return FALSE;
	}

	// バッファ長が異なるなら再確保
	if (dt.maplen != (DWORD)dt.sectors) {
		free(dt.changemap);
		dt.changemap = (BOOL *)malloc(dt.sectors * sizeof(BOOL));
		dt.maplen = dt.sectors;
	}

	// 変更マップクリア
	memset(dt.changemap, 0x00, dt.sectors * sizeof(BOOL));

	// ファイルから読み込む(fioの再利用を促すためオープンモードを考慮)
	fio = disk->GetFio();
	disk->GetPath(path);
	if (!fio->Open(path,
		disk->IsWriteP() ? Fileio::ReadOnly : Fileio::ReadWrite)) {
		return FALSE;
	}
	if (dt.raw) {
		// 分割読み
		for (i = 0; i < dt.sectors; i++) {
			// シーク
			if (!fio->Seek(offset)) {
				fio->Close();
				return FALSE;
			}

			// 読み込み
			if (!fio->Read(&dt.buffer[i << dt.size], 1 << dt.size)) {
				fio->Close();
				return FALSE;
			}

			// 次のオフセット
			offset += 0x930;
		}
	} else {
		// 連続読み
		if (!fio->Seek(offset)) {
			fio->Close();
			return FALSE;
		}
		if (!fio->Read(dt.buffer, length)) {
			fio->Close();
			return FALSE;
		}
	}
	fio->Close();

	// フラグを立て、正常終了
	dt.init = TRUE;
	dt.changed = FALSE;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	セーブ
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Save()
{
	fsize_t offset;
	int i;
	int j;
	int length;
	int total;
	Fileio *fio;
	Filepath path;

	ASSERT(this);

	// 初期化されていなければ不要
	if (!dt.init) {
		return TRUE;
	}

	// 変更されていなければ不要
	if (!dt.changed) {
		return TRUE;
	}

	// 書き込む必要がある
	ASSERT(dt.buffer);
	ASSERT(dt.changemap);
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= NumSectors));

	// RAWモードでは書き込みはありえない
	ASSERT(!dt.raw);

	// オフセットを計算
	// これ以前のトラックは固定のセクタ数(NumSectors)を保持とみなす
	offset = ((fsize_t)dt.track * NumSectors);
	offset <<= dt.size;

	// 実イメージまでのオフセットを追加
	offset += dt.imgoffset;

	// セクタあたりのレングスを計算
	length = 1 << dt.size;

	// ファイルオープン
	fio = disk->GetFio();
	disk->GetPath(path);
	if (!fio->Open(path, Fileio::ReadWrite)) {
		return FALSE;
	}

	// 部分書き込みループ
	for (i = 0; i < dt.sectors;) {
		// 変更されていれば
		if (dt.changemap[i]) {
			// 書き込みサイズ初期化
			total = 0;

			// シーク
			if (!fio->Seek(offset + ((fsize_t)i << dt.size))) {
				fio->Close();
				return FALSE;
			}

			// 連続するセクタ長
			for (j = i; j < dt.sectors; j++) {
				// 途切れたら終了
				if (!dt.changemap[j]) {
					break;
				}

				// 1セクタ分加算
				total += length;
			}

			// 書き込み
			if (!fio->Write(&dt.buffer[i << dt.size], total)) {
				fio->Close();
				return FALSE;
			}

			// 未変更のセクタへ
			i = j;
		} else {
			// 次のセクタ
			i++;
		}
	}

	// クローズ
	fio->Close();

	// 変更フラグを落とし、終了
	memset(dt.changemap, 0x00, dt.sectors * sizeof(BOOL));
	dt.changed = FALSE;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	リードセクタ
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Read(BYTE *buf, int sec) const
{
	ASSERT(this);
	ASSERT(buf);
	ASSERT((sec >= 0) & (sec < NumSectors));

	// 初期化されていなければエラー
	if (!dt.init) {
		return FALSE;
	}

	// セクタが有効数を超えていればエラー
	if (sec >= dt.sectors) {
		return FALSE;
	}

	// コピー
	ASSERT(dt.buffer);
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= NumSectors));
	memcpy(buf, &dt.buffer[sec << dt.size], 1 << dt.size);

	// 成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	ライトセクタ
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskTrack::Write(const BYTE *buf, int sec)
{
	int offset;
	int length;

	ASSERT(this);
	ASSERT(buf);
	ASSERT((sec >= 0) & (sec < NumSectors));
	ASSERT(!dt.raw);

	// 初期化されていなければエラー
	if (!dt.init) {
		return FALSE;
	}

	// セクタが有効数を超えていればエラー
	if (sec >= dt.sectors) {
		return FALSE;
	}

	// オフセット、レングスを計算
	offset = sec << dt.size;
	length = 1 << dt.size;

	// 比較
	ASSERT(dt.buffer);
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= NumSectors));
	if (memcmp(buf, &dt.buffer[offset], length) == 0) {
		// 同じものを書き込もうとしているので、正常終了
		return TRUE;
	}

	// コピー、変更あり
	memcpy(&dt.buffer[offset], buf, length);
	dt.changemap[sec] = TRUE;
	dt.changed = TRUE;

	// 成功
	return TRUE;
}

//===========================================================================
//
//	ディスクキャッシュ
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
DiskCache::DiskCache(Disk *p, int size, int blocks, fsize_t imgoff)
{
	int i;

	ASSERT(p);
	ASSERT((size >= 8) && (size <= 11));
	ASSERT(blocks > 0);
	ASSERT(imgoff >= 0);

	// ディスク
	disk = p;

	// キャッシュワーク
	for (i = 0; i < CacheMax; i++) {
		cache[i].disktrk = NULL;
		cache[i].serial = 0;
	}

	// その他
	serial = 0;
	sec_size = size;
	sec_blocks = blocks;
	cd_raw = FALSE;
	imgoffset = imgoff;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
DiskCache::~DiskCache()
{
	// トラックをクリア
	Clear();
}

//---------------------------------------------------------------------------
//
//	RAWモード設定
//
//---------------------------------------------------------------------------
void FASTCALL DiskCache::SetRawMode(BOOL raw)
{
	ASSERT(this);
	ASSERT(sec_size == 11);

	// 設定
	cd_raw = raw;
}

//---------------------------------------------------------------------------
//
//	セーブ
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Save()
{
	int i;

	ASSERT(this);

	// トラックを保存
	for (i = 0; i < CacheMax; i++) {
		// 有効なトラックか
		if (cache[i].disktrk) {
			// 保存
			if (!cache[i].disktrk->Save()) {
				return FALSE;
			}
		}
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	ディスクキャッシュ情報取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::GetCache(int index, int& track, DWORD& aserial) const
{
	ASSERT(this);
	ASSERT((index >= 0) && (index < CacheMax));

	// 未使用ならFALSE
	if (!cache[index].disktrk) {
		return FALSE;
	}

	// トラックとシリアルを設定
	track = cache[index].disktrk->GetTrack();
	aserial = cache[index].serial;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	クリア
//
//---------------------------------------------------------------------------
void FASTCALL DiskCache::Clear()
{
	int i;

	ASSERT(this);

	// キャッシュワークを解放
	for (i = 0; i < CacheMax; i++) {
		if (cache[i].disktrk) {
			delete cache[i].disktrk;
			cache[i].disktrk = NULL;
		}
	}
}

//---------------------------------------------------------------------------
//
//	セクタリード
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Read(BYTE *buf, int block)
{
	int track;
	DiskTrack *disktrk;

	ASSERT(this);
	ASSERT(sec_size != 0);

	// 先に更新
	Update();

	// トラックを算出
	// セクタ/トラックNumSectorsに固定
	track = block / DiskTrack::NumSectors;

	// そのトラックデータを得る
	disktrk = Assign(track);
	if (!disktrk) {
		return FALSE;
	}

	// トラックに任せる
	block &= DiskTrack::NumSectors - 1;
	return disktrk->Read(buf, block);
}

//---------------------------------------------------------------------------
//
//	セクタライト
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Write(const BYTE *buf, int block)
{
	int track;
	DiskTrack *disktrk;

	ASSERT(this);
	ASSERT(sec_size != 0);

	// 先に更新
	Update();

	// トラックを算出
	// セクタ/トラックNumSectorsに固定
	track = block / DiskTrack::NumSectors;

	// そのトラックデータを得る
	disktrk = Assign(track);
	if (!disktrk) {
		return FALSE;
	}

	// トラックに任せる
	block &= DiskTrack::NumSectors - 1;
	return disktrk->Write(buf, block);
}

//---------------------------------------------------------------------------
//
//	トラックの割り当て
//
//---------------------------------------------------------------------------
DiskTrack* FASTCALL DiskCache::Assign(int track)
{
	int i;
	int c;
	DWORD s;
	DiskTrack *disktrk;

	ASSERT(this);
	ASSERT(sec_size != 0);
	ASSERT(track >= 0);

	// まず、既に割り当てされていないか調べる
	for (i = 0; i < CacheMax; i++) {
		if (cache[i].disktrk) {
			if (cache[i].disktrk->GetTrack() == track) {
				// トラックが一致
				cache[i].serial = serial;
				return cache[i].disktrk;
			}
		}
	}

	// 次に、空いているものがないか調べる
	for (i = 0; i < CacheMax; i++) {
		if (!cache[i].disktrk) {
			// ロードを試みる
			if (Load(i, track)) {
				// ロード成功
				cache[i].serial = serial;
				return cache[i].disktrk;
			}

			// ロード失敗
			return NULL;
		}
	}

	// 最後に、シリアル番号の一番若いものを探し、削除する

	// インデックス0を候補cとする
	s = cache[0].serial;
	c = 0;

	// 候補とシリアルを比較し、より小さいものへ更新する
	for (i = 0; i < CacheMax; i++) {
		ASSERT(cache[i].disktrk);

		// 既に存在するシリアルと比較、更新
		if (cache[i].serial < s) {
			s = cache[i].serial;
			c = i;
		}
	}

	// このトラックを保存
	if (!cache[c].disktrk->Save()) {
		return NULL;
	}

	// このトラックを削除
	disktrk = cache[c].disktrk;
	cache[c].disktrk = NULL;

	// ロード
	if (Load(c, track, disktrk)) {
		// ロード成功
		cache[c].serial = serial;
		return cache[c].disktrk;
	}

	// ロード失敗
	return NULL;
}

//---------------------------------------------------------------------------
//
//	トラックのロード
//
//---------------------------------------------------------------------------
BOOL FASTCALL DiskCache::Load(int index, int track, DiskTrack *disktrk)
{
	int sectors;

	ASSERT(this);
	ASSERT((index >= 0) && (index < CacheMax));
	ASSERT(track >= 0);
	ASSERT(!cache[index].disktrk);

	// このトラックのセクタ数を取得
	sectors = sec_blocks - (track * DiskTrack::NumSectors);
	ASSERT(sectors > 0);
	if (sectors > DiskTrack::NumSectors) {
		sectors = DiskTrack::NumSectors;
	}

	// ディスクトラックを作成
	if (disktrk == NULL) {
		disktrk = new DiskTrack();
	}

	// ディスクトラックを初期化
	disktrk->Init(disk, track, sec_size, sectors, cd_raw, imgoffset);

	// ロードを試みる
	if (!disktrk->Load()) {
		// 失敗
		delete disktrk;
		return FALSE;
	}

	// 割り当て成功、ワークを設定
	cache[index].disktrk = disktrk;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	シリアル番号の更新
//
//---------------------------------------------------------------------------
void FASTCALL DiskCache::Update()
{
	int i;

	ASSERT(this);

	// 更新して、0以外は何もしない
	serial++;
	if (serial != 0) {
		return;
	}

	// 全キャッシュのシリアルをクリア(32bitループしている)
	for (i = 0; i < CacheMax; i++) {
		cache[i].serial = 0;
	}
}


//===========================================================================
//
//	ディスク
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
Disk::Disk()
{
	// ワーク初期化
	disk.id = MAKEID('N', 'U', 'L', 'L');
	disk.ready = FALSE;
	disk.writep = FALSE;
	disk.readonly = FALSE;
	disk.removable = FALSE;
	disk.lock = FALSE;
	disk.attn = FALSE;
	disk.reset = FALSE;
	disk.size = 0;
	disk.blocks = 0;
	disk.lun = 0;
	disk.code = 0;
	disk.dcache = NULL;
	disk.imgoffset = 0;

	// その他
	cache_wb = TRUE;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
Disk::~Disk()
{
	// ディスクキャッシュの保存
	if (disk.ready) {
		// レディの場合のみ
		if (disk.dcache) {
			disk.dcache->Save();
		}
	}

	// ディスクキャッシュの削除
	if (disk.dcache) {
		delete disk.dcache;
		disk.dcache = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	リセット
//
//---------------------------------------------------------------------------
void FASTCALL Disk::Reset()
{
	ASSERT(this);

	// ロックなし、アテンションなし、リセットあり
	disk.lock = FALSE;
	disk.attn = FALSE;
	disk.reset = TRUE;
}

//---------------------------------------------------------------------------
//
//	NULLチェック
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::IsNULL() const
{
	ASSERT(this);

	if (disk.id == MAKEID('N', 'U', 'L', 'L')) {
		return TRUE;
	}
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	SASIチェック
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::IsSASI() const
{
	ASSERT(this);

	if (disk.id == MAKEID('S', 'A', 'H', 'D')) {
		return TRUE;
	}
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	オープン
//	※派生クラスで、オープン成功後の後処理として呼び出すこと
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Open(const Filepath& path, BOOL /*attn*/)
{
	ASSERT(this);
	ASSERT((disk.size >= 8) && (disk.size <= 11));
	ASSERT(disk.blocks > 0);

	// レディ
	disk.ready = TRUE;

	// キャッシュ初期化
	ASSERT(!disk.dcache);
	disk.dcache =
		new DiskCache(this, disk.size, disk.blocks, disk.imgoffset);

	// 読み書きオープン可能か
	if (fio.Open(path, Fileio::ReadWrite)) {
		// 書き込み許可、リードオンリーでない
		disk.writep = FALSE;
		disk.readonly = FALSE;
		fio.Close();
	} else {
		// 書き込み禁止、リードオンリー
		disk.writep = TRUE;
		disk.readonly = TRUE;
	}

	// ロックされていない
	disk.lock = FALSE;

	// パス保存
	diskpath = path;

	// 成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	イジェクト
//
//---------------------------------------------------------------------------
void FASTCALL Disk::Eject(BOOL force)
{
	ASSERT(this);

	// リムーバブルでなければイジェクトできない
	if (!disk.removable) {
		return;
	}

	// レディでなければイジェクト必要ない
	if (!disk.ready) {
		return;
	}

	// 強制フラグがない場合、ロックされていないことが必要
	if (!force) {
		if (disk.lock) {
			return;
		}
	}

	// ディスクキャッシュを削除
	disk.dcache->Save();
	delete disk.dcache;
	disk.dcache = NULL;

	// ノットレディ、アテンションなし
	disk.ready = FALSE;
	disk.writep = FALSE;
	disk.readonly = FALSE;
	disk.attn = FALSE;
}

//---------------------------------------------------------------------------
//
//	書き込み禁止
//
//---------------------------------------------------------------------------
void FASTCALL Disk::WriteP(BOOL writep)
{
	ASSERT(this);

	// レディであること
	if (!disk.ready) {
		return;
	}

	// Read Onlyの場合は、プロテクト状態のみ
	if (disk.readonly) {
		ASSERT(disk.writep);
		return;
	}

	// フラグ設定
	disk.writep = writep;
}

//---------------------------------------------------------------------------
//
//	内部ワーク取得
//
//---------------------------------------------------------------------------
void FASTCALL Disk::GetDisk(disk_t *buffer) const
{
	ASSERT(this);
	ASSERT(buffer);

	// 内部ワークをコピー
	*buffer = disk;
}

//---------------------------------------------------------------------------
//
//	パス取得
//
//---------------------------------------------------------------------------
void FASTCALL Disk::GetPath(Filepath& path) const
{
	path = diskpath;
}

//---------------------------------------------------------------------------
//
//	フラッシュ
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Flush()
{
	ASSERT(this);

	// キャッシュがなければ何もしない
	if (!disk.dcache) {
		return TRUE;
	}

	// キャッシュを保存
	return disk.dcache->Save();
}

//---------------------------------------------------------------------------
//
//	レディチェック
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::CheckReady()
{
	ASSERT(this);

	// リセットなら、ステータスを返す
	if (disk.reset) {
		disk.code = DISK_DEVRESET;
		disk.reset = FALSE;
		return FALSE;
	}

	// アテンションなら、ステータスを返す
	if (disk.attn) {
		disk.code = DISK_ATTENTION;
		disk.attn = FALSE;
		return FALSE;
	}

	// ノットレディなら、ステータスを返す
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return FALSE;
	}

	// エラーなしに初期化
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//	※常時成功する必要がある
//
//---------------------------------------------------------------------------
int FASTCALL Disk::Inquiry(
	const DWORD* /*cdb*/, BYTE* /*buf*/, DWORD /*major*/, DWORD /*minor*/)
{
	ASSERT(this);

	// デフォルトはINQUIRY失敗
	disk.code = DISK_INVALIDCMD;
	return 0;
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//	※SASIは別処理
//
//---------------------------------------------------------------------------
int FASTCALL Disk::RequestSense(const DWORD *cdb, BYTE *buf)
{
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);

	// エラーがない場合に限り、ノットレディをチェック
	if (disk.code == DISK_NOERROR) {
		if (!disk.ready) {
			disk.code = DISK_NOTREADY;
		}
	}

	// サイズ決定(アロケーションレングスに従う)
	size = (int)cdb[4];
	ASSERT((size >= 0) && (size < 0x100));

	// SCSI-1では、サイズ0のときに4バイト転送する(SCSI-2ではこの仕様は削除)
	if (size == 0) {
		size = 4;
	}

	// バッファをクリア
	memset(buf, 0, size);

	// 拡張センスデータを含めた、18バイトを設定
	buf[0] = 0x70;
	buf[2] = (BYTE)(disk.code >> 16);
	buf[7] = 10;
	buf[12] = (BYTE)(disk.code >> 8);
	buf[13] = (BYTE)disk.code;

	// コードをクリア
	disk.code = 0x00;

	return size;
}

//---------------------------------------------------------------------------
//
//	MODE SELECTチェック
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
int FASTCALL Disk::SelectCheck(const DWORD *cdb)
{
	int length;

	ASSERT(this);
	ASSERT(cdb);

	// SCSIHDでなくセーブパラメータが設定されていればエラー
	if (disk.id != MAKEID('S', 'C', 'H', 'D')) {
		// セーブパラメータが設定されていればエラー
		if (cdb[1] & 0x01) {
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// パラメータレングスで指定されたデータを受け取る
	length = (int)cdb[4];
	return length;
}


//---------------------------------------------------------------------------
//
//	MODE SELECT(10)チェック
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
int FASTCALL Disk::SelectCheck10(const DWORD *cdb)
{
	DWORD length;

	ASSERT(this);
	ASSERT(cdb);

	// SCSIHDでなくセーブパラメータが設定されていればエラー
	if (disk.id != MAKEID('S', 'C', 'H', 'D')) {
		if (cdb[1] & 0x01) {
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// パラメータレングスで指定されたデータを受け取る
	length = cdb[7];
	length <<= 8;
	length |= cdb[8];
	if (length > 0x800) {
		length = 0x800;
	}

	return (int)length;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::ModeSelect(
	const DWORD* /*cdb*/, const BYTE *buf, int length)
{
	ASSERT(this);
	ASSERT(buf);
	ASSERT(length >= 0);

	// 設定できない
	disk.code = DISK_INVALIDPRM;

	return FALSE;
}

//---------------------------------------------------------------------------
//
//	MODE SENSE
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ModeSense(const DWORD *cdb, BYTE *buf)
{
	int page;
	int length;
	int size;
	BOOL valid;
	BOOL change;
	int ret;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x1a);

	// レングス取得、バッファクリア
	length = (int)cdb[4];
	ASSERT((length >= 0) && (length < 0x100));
	memset(buf, 0, length);

	// 変更可能フラグ取得
	if ((cdb[2] & 0xc0) == 0x40) {
		change = TRUE;
	} else {
		change = FALSE;
	}

	// ページコード取得(0x00は最初からvalid)
	page = cdb[2] & 0x3f;
	if (page == 0x00) {
		valid = TRUE;
	} else {
		valid = FALSE;
	}

	// 基本情報
	size = 4;

	// MEDIUM TYPE
	if (disk.id == MAKEID('S', 'C', 'M', 'O')) {
		buf[1] = 0x03; // optical reversible or erasable
	}

	// DEVICE SPECIFIC PARAMETER
	if (disk.writep) {
		buf[2] = 0x80;
	}

	// DBDが0なら、ブロックディスクリプタを追加
	if ((cdb[1] & 0x08) == 0) {
		// モードパラメータヘッダ
		buf[3] = 0x08;

		// レディの場合に限り
		if (disk.ready) {
			// ブロックディスクリプタ(ブロック数)
			buf[5] = (BYTE)(disk.blocks >> 16);
			buf[6] = (BYTE)(disk.blocks >> 8);
			buf[7] = (BYTE)disk.blocks;

			// ブロックディスクリプタ(ブロックレングス)
			size = 1 << disk.size;
			buf[9] = (BYTE)(size >> 16);
			buf[10] = (BYTE)(size >> 8);
			buf[11] = (BYTE)size;
		}

		// サイズ再設定
		size = 12;
	}

	// ページコード1(read-write error recovery)
	if ((page == 0x01) || (page == 0x3f)) {
		size += AddError(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード3(format device)
	if ((page == 0x03) || (page == 0x3f)) {
		size += AddFormat(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード4(drive parameter)
	if ((page == 0x04) || (page == 0x3f)) {
		size += AddDrive(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード6(optical)
	if (disk.id == MAKEID('S', 'C', 'M', 'O')) {
		if ((page == 0x06) || (page == 0x3f)) {
			size += AddOpt(change, &buf[size]);
			valid = TRUE;
		}
	}

	// ページコード8(caching)
	if ((page == 0x08) || (page == 0x3f)) {
		size += AddCache(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード13(CD-ROM)
	if (disk.id == MAKEID('S', 'C', 'C', 'D')) {
		if ((page == 0x0d) || (page == 0x3f)) {
			size += AddCDROM(change, &buf[size]);
			valid = TRUE;
		}
	}

	// ページコード14(CD-DA)
	if (disk.id == MAKEID('S', 'C', 'C', 'D')) {
		if ((page == 0x0e) || (page == 0x3f)) {
			size += AddCDDA(change, &buf[size]);
			valid = TRUE;
		}
	}

	// ページ(ベンダ特殊)
	ret = AddVendor(page, change, &buf[size]);
	if (ret > 0) {
		size += ret;
		valid = TRUE;
	}

	// モードデータレングスを最終設定
	buf[0] = (BYTE)(size - 1);

	// サポートしていないページか
	if (!valid) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// MODE SENSE成功
	disk.code = DISK_NOERROR;
	return length;
}

//---------------------------------------------------------------------------
//
//	MODE SENSE(10)
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ModeSense10(const DWORD *cdb, BYTE *buf)
{
	int page;
	int length;
	int size;
	BOOL valid;
	BOOL change;
	int ret;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x5a);

	// レングス取得、バッファクリア
	length = cdb[7];
	length <<= 8;
	length |= cdb[8];
	if (length > 0x800) {
		length = 0x800;
	}
	ASSERT((length >= 0) && (length < 0x800));
	memset(buf, 0, length);

	// 変更可能フラグ取得
	if ((cdb[2] & 0xc0) == 0x40) {
		change = TRUE;
	} else {
		change = FALSE;
	}

	// ページコード取得(0x00は最初からvalid)
	page = cdb[2] & 0x3f;
	if (page == 0x00) {
		valid = TRUE;
	} else {
		valid = FALSE;
	}

	// 基本情報
	size = 4;
	if (disk.writep) {
		buf[2] = 0x80;
	}

	// DBDが0なら、ブロックディスクリプタを追加
	if ((cdb[1] & 0x08) == 0) {
		// モードパラメータヘッダ
		buf[3] = 0x08;

		// レディの場合に限り
		if (disk.ready) {
			// ブロックディスクリプタ(ブロック数)
			buf[5] = (BYTE)(disk.blocks >> 16);
			buf[6] = (BYTE)(disk.blocks >> 8);
			buf[7] = (BYTE)disk.blocks;

			// ブロックディスクリプタ(ブロックレングス)
			size = 1 << disk.size;
			buf[9] = (BYTE)(size >> 16);
			buf[10] = (BYTE)(size >> 8);
			buf[11] = (BYTE)size;
		}

		// サイズ再設定
		size = 12;
	}

	// ページコード1(read-write error recovery)
	if ((page == 0x01) || (page == 0x3f)) {
		size += AddError(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード3(format device)
	if ((page == 0x03) || (page == 0x3f)) {
		size += AddFormat(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード4(drive parameter)
	if ((page == 0x04) || (page == 0x3f)) {
		size += AddDrive(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード6(optical)
	if (disk.id == MAKEID('S', 'C', 'M', 'O')) {
		if ((page == 0x06) || (page == 0x3f)) {
			size += AddOpt(change, &buf[size]);
			valid = TRUE;
		}
	}

	// ページコード8(caching)
	if ((page == 0x08) || (page == 0x3f)) {
		size += AddCache(change, &buf[size]);
		valid = TRUE;
	}

	// ページコード13(CD-ROM)
	if (disk.id == MAKEID('S', 'C', 'C', 'D')) {
		if ((page == 0x0d) || (page == 0x3f)) {
			size += AddCDROM(change, &buf[size]);
			valid = TRUE;
		}
	}

	// ページコード14(CD-DA)
	if (disk.id == MAKEID('S', 'C', 'C', 'D')) {
		if ((page == 0x0e) || (page == 0x3f)) {
			size += AddCDDA(change, &buf[size]);
			valid = TRUE;
		}
	}

	// ページ(ベンダ特殊)
	ret = AddVendor(page, change, &buf[size]);
	if (ret > 0) {
		size += ret;
		valid = TRUE;
	}

	// モードデータレングスを最終設定
	buf[0] = (BYTE)(size - 1);

	// サポートしていないページか
	if (!valid) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// MODE SENSE成功
	disk.code = DISK_NOERROR;
	return length;
}

//---------------------------------------------------------------------------
//
//	エラーページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddError(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x01;
	buf[1] = 0x0a;

	// 変更可能領域はない
	if (change) {
		return 12;
	}

	// リトライカウントは0、リミットタイムは装置内部のデフォルト値を使用
	return 12;
}

//---------------------------------------------------------------------------
//
//	フォーマットページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddFormat(BOOL change, BYTE *buf)
{
	int size;

	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x80 | 0x03;
	buf[1] = 0x16;

	// 物理セクタのバイト数は変更可能に見せる(実際には変更できないが)
	if (change) {
		buf[0xc] = 0xff;
		buf[0xd] = 0xff;
		return 24;
	}

	if (disk.ready) {
		// 1ゾーンのトラック数を8に設定(TODO)
		buf[0x3] = 0x08;

		// セクタ/トラックを25に設定(TODO)
		buf[0xa] = 0x00;
		buf[0xb] = 0x19;

		// 物理セクタのバイト数を設定
		size = 1 << disk.size;
		buf[0xc] = (BYTE)(size >> 8);
		buf[0xd] = (BYTE)size;
	}

	// リムーバブル属性を設定
	if (disk.removable) {
		buf[20] = 0x20;
	}

	return 24;
}

//---------------------------------------------------------------------------
//
//	ドライブページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddDrive(BOOL change, BYTE *buf)
{
	DWORD cylinder;

	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x04;
	buf[1] = 0x16;

	// 変更可能領域はない
	if (change) {
		return 24;
	}

	if (disk.ready) {
		// シリンダ数を設定(総ブロック数を25セクタ/トラックと8ヘッドで除算)
		cylinder = disk.blocks;
		cylinder >>= 3;
		cylinder /= 25;
		buf[0x2] = (BYTE)(cylinder >> 16);
		buf[0x3] = (BYTE)(cylinder >> 8);
		buf[0x4] = (BYTE)cylinder;

		// ヘッドを8で固定
		buf[0x5] = 0x8;
	}

	return 24;
}

//---------------------------------------------------------------------------
//
//	オプティカルページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddOpt(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x06;
	buf[1] = 0x02;

	// 変更可能領域はない
	if (change) {
		return 4;
	}

	// 更新ブロックのレポートは行わない
	return 4;
}

//---------------------------------------------------------------------------
//
//	キャッシュページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddCache(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x08;
	buf[1] = 0x0a;

	// 変更可能領域はない
	if (change) {
		return 12;
	}

	// 読み込みキャッシュのみ有効、プリフェッチは行わない
	return 12;
}

//---------------------------------------------------------------------------
//
//	CD-ROMページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddCDROM(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x0d;
	buf[1] = 0x06;

	// 変更可能領域はない
	if (change) {
		return 8;
	}

	// インアクティブタイマは2sec
	buf[3] = 0x05;

	// MSF倍数はそれぞれ60, 75
	buf[5] = 60;
	buf[7] = 75;

	return 8;
}

//---------------------------------------------------------------------------
//
//	CD-DAページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddCDDA(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x0e;
	buf[1] = 0x0e;

	// 変更可能領域はない
	if (change) {
		return 16;
	}

	// オーディオは操作完了を待ち、 複数トラックにまたがるPLAYを許可する
	return 16;
}

//---------------------------------------------------------------------------
//
//	Vendor特殊ページ追加
//
//---------------------------------------------------------------------------
int FASTCALL Disk::AddVendor(int /*page*/, BOOL /*change*/, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	return 0;
}

//---------------------------------------------------------------------------
//
//	READ DEFECT DATA(10)
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ReadDefectData10(const DWORD *cdb, BYTE *buf)
{
	DWORD length;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x37);

	// レングス取得、バッファクリア
	length = cdb[7];
	length <<= 8;
	length |= cdb[8];
	if (length > 0x800) {
		length = 0x800;
	}
	ASSERT((length >= 0) && (length < 0x800));
	memset(buf, 0, length);

	// P/G/FORMAT
	buf[1] = (cdb[1] & 0x18) | 5;
	buf[3] = 8;

	buf[4] = 0xff;
	buf[5] = 0xff;
	buf[6] = 0xff;
	buf[7] = 0xff;

	buf[8] = 0xff;
	buf[9] = 0xff;
	buf[10] = 0xff;
	buf[11] = 0xff;

	// リスト無し
	disk.code = DISK_NODEFECT;
	return 4;
}

//---------------------------------------------------------------------------
//
//	コマンド
//
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//
//	TEST UNIT READY
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::TestUnitReady(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// TEST UNIT READY成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	REZERO UNIT
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Rezero(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// REZERO成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	FORMAT UNIT
//	※SASIはオペコード$06、SCSIはオペコード$04
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Format(const DWORD *cdb)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// FMTDATA=1はサポートしない(但しDEFECT LISTが無いならOK)
	if ((cdb[1] & 0x10) != 0 && cdb[4] != 0) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// FORMAT成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	REASSIGN BLOCKS
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Reassign(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// REASSIGN BLOCKS成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ
//
//---------------------------------------------------------------------------
int FASTCALL Disk::Read(BYTE *buf, DWORD block)
{
	ASSERT(this);
	ASSERT(buf);

	// 状態チェック
	if (!CheckReady()) {
		return 0;
	}

	// トータルブロック数を超えていればエラー
	if (block >= disk.blocks) {
		disk.code = DISK_INVALIDLBA;
		return 0;
	}

	// キャッシュに任せる
	if (!disk.dcache->Read(buf, block)) {
		disk.code = DISK_READFAULT;
		return 0;
	}

	// 成功
	return (1 << disk.size);
}

//---------------------------------------------------------------------------
//
//	WRITEチェック
//
//---------------------------------------------------------------------------
int FASTCALL Disk::WriteCheck(DWORD block)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return 0;
	}

	// トータルブロック数を超えていればエラー
	if (block >= disk.blocks) {
		return 0;
	}

	// 書き込み禁止ならエラー
	if (disk.writep) {
		disk.code = DISK_WRITEPROTECT;
		return 0;
	}

	// 成功
	return (1 << disk.size);
}

//---------------------------------------------------------------------------
//
//	WRITE
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Write(const BYTE *buf, DWORD block)
{
	ASSERT(this);
	ASSERT(buf);

	// レディでなければエラー
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return FALSE;
	}

	// トータルブロック数を超えていればエラー
	if (block >= disk.blocks) {
		disk.code = DISK_INVALIDLBA;
		return FALSE;
	}

	// 書き込み禁止ならエラー
	if (disk.writep) {
		disk.code = DISK_WRITEPROTECT;
		return FALSE;
	}

	// キャッシュに任せる
	if (!disk.dcache->Write(buf, block)) {
		disk.code = DISK_WRITEFAULT;
		return FALSE;
	}

	// 成功
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SEEK
//	※LBAのチェックは行わない(SASI IOCS)
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Seek(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// SEEK成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	ASSIGN
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Assign(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// 成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SPECIFY
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Specify(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// 成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	START STOP UNIT
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::StartStop(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1b);

	// イジェクトビットを見て、必要ならイジェクト
	if (cdb[4] & 0x02) {
		if (disk.lock) {
			// ロックされているので、イジェクトできない
			disk.code = DISK_PREVENT;
			return FALSE;
		}

		// イジェクト
		Eject(FALSE);
	}

	// OK
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SEND DIAGNOSTIC
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::SendDiag(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1d);

	// PFビットはサポートしない
	if (cdb[1] & 0x10) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// パラメータリストはサポートしない
	if ((cdb[3] != 0) || (cdb[4] != 0)) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// 常に成功
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	PREVENT/ALLOW MEDIUM REMOVAL
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Removal(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1e);

	// 状態チェック
	if (!CheckReady()) {
		return FALSE;
	}

	// ロックフラグを設定
	if (cdb[4] & 0x01) {
		disk.lock = TRUE;
	} else {
		disk.lock = FALSE;
	}

	// REMOVAL成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ CAPACITY
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ReadCapacity(const DWORD* /*cdb*/, BYTE *buf)
{
	DWORD blocks;
	DWORD length;

	ASSERT(this);
	ASSERT(buf);

	// バッファクリア
	memset(buf, 0, 8);

	// 状態チェック
	if (!CheckReady()) {
		return 0;
	}

	// 論理ブロックアドレスの終端(disk.blocks - 1)を作成
	ASSERT(disk.blocks > 0);
	blocks = disk.blocks - 1;
	buf[0] = (BYTE)(blocks >> 24);
	buf[1] = (BYTE)(blocks >> 16);
	buf[2] = (BYTE)(blocks >> 8);
	buf[3] = (BYTE)blocks;

	// ブロックレングス(1 << disk.size)を作成
	length = 1 << disk.size;
	buf[4] = (BYTE)(length >> 24);
	buf[5] = (BYTE)(length >> 16);
	buf[6] = (BYTE)(length >> 8);
	buf[7] = (BYTE)length;

	// 返送サイズを返す
	return 8;
}

//---------------------------------------------------------------------------
//
//	VERIFY
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::Verify(const DWORD *cdb)
{
	DWORD record;
	DWORD blocks;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x2f);

	// パラメータ取得
	record = cdb[2];
	record <<= 8;
	record |= cdb[3];
	record <<= 8;
	record |= cdb[4];
	record <<= 8;
	record |= cdb[5];
	blocks = cdb[7];
	blocks <<= 8;
	blocks |= cdb[8];

	// 状態チェック
	if (!CheckReady()) {
		return 0;
	}

	// パラメータチェック
	if (disk.blocks < (record + blocks)) {
		disk.code = DISK_INVALIDLBA;
		return FALSE;
	}

	// 成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ TOC
//
//---------------------------------------------------------------------------
int FASTCALL Disk::ReadToc(const DWORD *cdb, BYTE *buf)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x43);
	ASSERT(buf);

	// このコマンドはサポートしない
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::PlayAudio(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x45);

	// このコマンドはサポートしない
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO MSF
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::PlayAudioMSF(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x47);

	// このコマンドはサポートしない
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO TRACK
//
//---------------------------------------------------------------------------
BOOL FASTCALL Disk::PlayAudioTrack(const DWORD *cdb)
{
	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x48);

	// このコマンドはサポートしない
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//===========================================================================
//
//	SASI ハードディスク
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SASIHD::SASIHD() : Disk()
{
	// SASI ハードディスク
	disk.id = MAKEID('S', 'A', 'H', 'D');
}

//---------------------------------------------------------------------------
//
//	リセット
//
//---------------------------------------------------------------------------
void FASTCALL SASIHD::Reset()
{
	// ロック状態解除、アテンション解除
	disk.lock = FALSE;
	disk.attn = FALSE;

	// リセットなし、コードをクリア
	disk.reset = FALSE;
	disk.code = 0x00;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL SASIHD::Open(const Filepath& path, BOOL /*attn*/)
{
	fsize_t size;

	ASSERT(this);
	ASSERT(!disk.ready);

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// ファイルサイズの取得
	size = fio.GetFileSize();
	fio.Close();

#if USE_MZ1F23_1024_SUPPORT == 1
	// MZ-2500/MZ-2800用 MZ-1F23(SASI 20M/セクタサイズ1024)専用
	// 20M(22437888 BS=1024 C=21912)
	if (size == 0x1566000) {
		// セクタサイズとブロック数
		disk.size = 10;
		disk.blocks = (DWORD)(size >> 10);

		// 基本クラス
		return Disk::Open(path);
	}
#endif	// USE_MZ1F23_1024_SUPPORT

#if REMOVE_FIXED_SASIHD_SIZE == 1
	// 256バイト単位であること
	if (size & 0xff) {
		return FALSE;
	}

	// 10MB以上
	if (size < 0x9f5400) {
		return FALSE;
	}

	// 512MB程度に制限しておく
	if (size > 512 * 1024 * 1024) {
		return FALSE;
	}
#else
	// 10MB, 20MB, 40MBのみ
	switch (size) {
		// 10MB(10441728 BS=256 C=40788)
		case 0x9f5400:
			break;

		// 20MB(20748288 BS=256 C=81048)
		case 0x13c9800:
			break;

		// 40MB(41496576 BS=256 C=162096)
		case 0x2793000:
			break;

		// その他(サポートしない)
		default:
			return FALSE;
	}
#endif	// REMOVE_FIXED_SASIHD_SIZE

	// セクタサイズとブロック数
	disk.size = 8;
	disk.blocks = (DWORD)(size >> 8);

	// 基本クラス
	return Disk::Open(path);
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//
//---------------------------------------------------------------------------
int FASTCALL SASIHD::RequestSense(const DWORD *cdb, BYTE *buf)
{
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);

	// サイズ決定
	size = (int)cdb[4];
	ASSERT((size >= 0) && (size < 0x100));

	// サイズ0のときに4バイト転送する(Shugart Associates System Interface仕様)
	if (size == 0) {
		size = 4;
	}

	// SASIは非拡張フォーマットに固定
	memset(buf, 0, size);
	buf[0] = (BYTE)(disk.code >> 16);
	buf[1] = (BYTE)(disk.lun << 5);

	// コードをクリア
	disk.code = 0x00;

	return size;
}

//===========================================================================
//
//	SCSI ハードディスク
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSIHD::SCSIHD() : Disk()
{
	// SCSI ハードディスク
	disk.id = MAKEID('S', 'C', 'H', 'D');
}

//---------------------------------------------------------------------------
//
//	リセット
//
//---------------------------------------------------------------------------
void FASTCALL SCSIHD::Reset()
{
	// ロック状態解除、アテンション解除
	disk.lock = FALSE;
	disk.attn = FALSE;

	// リセットあり、コードをクリア
	disk.reset = TRUE;
	disk.code = 0x00;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIHD::Open(const Filepath& path, BOOL /*attn*/)
{
	fsize_t size;

	ASSERT(this);
	ASSERT(!disk.ready);

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// ファイルサイズの取得
	size = fio.GetFileSize();
	fio.Close();

	// 512バイト単位であること
	if (size & 0x1ff) {
		return FALSE;
	}

	// 10MB以上
	if (size < 0x9f5400) {
		return FALSE;
	}
	if (size > 2LL * 1024 * 1024 * 1024 * 1024) {
		return FALSE;
	}

	// セクタサイズとブロック数
	disk.size = 9;
	disk.blocks = (DWORD)(size >> 9);

	// 基本クラス
	return Disk::Open(path);
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD::Inquiry(
	const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	char vendor[32];
	char product[32];
	char rev[32];
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPDチェック
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// レディチェック(イメージファイルがない場合、エラーとする)
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return 0;
	}

	// 基本データ
	// buf[0] ... Direct Access Device
	// buf[2] ... SCSI-2準拠のコマンド体系
	// buf[3] ... SCSI-2準拠のInquiryレスポンス
	// buf[4] ... Inquiry追加データ
	memset(buf, 0, 8);

	// SCSI-2 本の p.104 4.4.3 不当なロジカルユニットの処理
	if (((cdb[1] >> 5) & 0x07) != disk.lun) {
		buf[0] = 0x7f;
	}

	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 122 + 3;	// 実HDDに近い値

	// 空白で埋める
	memset(&buf[8], 0x20, buf[4] - 3);

	// ベンダ名/製品名を決定
	sprintf(vendor, DEFAULT_VENDER);
	size = disk.blocks >> 11;
	if (size < 300)
		sprintf(product, "PRODRIVE LPS%dS", size);
	else if (size < 600)
		sprintf(product, "MAVERICK%dS", size);
	else if (size < 800)
		sprintf(product, "LIGHTNING%dS", size);
	else if (size < 1000)
		sprintf(product, "TRAILBRAZER%dS", size);
	else if (size < 2000)
		sprintf(product, "FIREBALL%dS", size);
	else
		sprintf(product, "FBSE%d.%dS", size / 1000, (size % 1000) / 100);

	// ベンダ名
	memcpy(&buf[8], vendor, strlen(vendor));

	// 製品名
	memcpy(&buf[16], product, strlen(product));

	// リビジョン
	sprintf(rev, "0%01d%01d%01d",
			(int)major, (int)(minor >> 4), (int)(minor & 0x0f));
	memcpy(&buf[32], rev, 4);

	// 返却できるデータのサイズ
	size = (buf[4] + 5);

	// 相手のバッファが少なければ制限する
	if (size > (int)cdb[4]) {
		size = (int)cdb[4];
	}

	// 成功
	disk.code = DISK_NOERROR;
	return size;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIHD::ModeSelect(const DWORD *cdb, const BYTE *buf, int length)
{
	int page;
	int size;

	ASSERT(this);
	ASSERT(buf);
	ASSERT(length >= 0);

	// PF
	if (cdb[1] & 0x10) {
		// Mode Parameter header
		if (length >= 12) {
			// ブロックレングスのバイト数をチェック
			size = 1 << disk.size;
			if (buf[9] != (BYTE)(size >> 16) ||
				buf[10] != (BYTE)(size >> 8) ||
				buf[11] != (BYTE)size) {
				// 今のところセクタ長の変更は許さない
				disk.code = DISK_INVALIDPRM;
				return FALSE;
			}
			buf += 12;
			length -= 12;
		}

		// ページの解析
		while (length > 0) {
			// ページの取得
			page = buf[0];

			switch (page) {
				// format device
				case 0x03:
					// 物理セクタのバイト数をチェック
					size = 1 << disk.size;
					if (buf[0xc] != (BYTE)(size >> 8) ||
						buf[0xd] != (BYTE)size) {
						// 今のところセクタ長の変更は許さない
						disk.code = DISK_INVALIDPRM;
						return FALSE;
					}
					break;

				// その他ページ
				default:
					break;
			}

			// 次のページへ
			size = buf[1] + 2;
			length -= size;
			buf += size;
		}
	}

	// とりあえずエラーは発生させない(MINIX)
	disk.code = DISK_NOERROR;

	return TRUE;
}

//===========================================================================
//
//	SCSI ハードディスク(PC-9801-55 NEC純正/Anex86/T98Next)
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSIHD_NEC::SCSIHD_NEC() : SCSIHD()
{
	// ワーク初期化
	cylinders = 0;
	heads = 0;
	sectors = 0;
	sectorsize = 0;
	imgoffset = 0;
	imgsize = 0;
}

//---------------------------------------------------------------------------
//
//	リトルエンディアンと想定したワードを取り出す
//
//---------------------------------------------------------------------------
static inline WORD getWordLE(const BYTE *b)
{
	return ((WORD)(b[1]) << 8) | b[0];
}

//---------------------------------------------------------------------------
//
//	リトルエンディアンと想定したロングワードを取り出す
//
//---------------------------------------------------------------------------
static inline DWORD getDwordLE(const BYTE *b)
{
	return ((DWORD)(b[3]) << 24) | ((DWORD)(b[2]) << 16) |
		((DWORD)(b[1]) << 8) | b[0];
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIHD_NEC::Open(const Filepath& path, BOOL /*attn*/)
{
	fsize_t size;
	BYTE hdr[512];
	LPCTSTR ext;

	ASSERT(this);
	ASSERT(!disk.ready);

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// ファイルサイズの取得
	size = fio.GetFileSize();

	// ヘッダー読み込み
	if (size >= (fsize_t)sizeof(hdr)) {
		if (!fio.Read(hdr, sizeof(hdr))) {
			fio.Close();
			return FALSE;
		}
	}
	fio.Close();

	// 512バイト単位であること
	if (size & 0x1ff) {
		return FALSE;
	}

	// 10MB以上
	if (size < 0x9f5400) {
		return FALSE;
	}
	if (size > 2LL * 1024 * 1024 * 1024 * 1024) {
		return FALSE;
	}

	// 拡張子別にパラメータを決定
	ext = path.GetFileExt();
	if (_xstrcasecmp(ext, _T(".HDN")) == 0) {
		// デフォルト設定としてセクタサイズ512,セクタ数25,ヘッド数8を想定
		imgoffset = 0;
		imgsize = size;
		sectorsize = 512;
		sectors = 25;
		heads = 8;
		cylinders = (int)(size >> 9);
		cylinders >>= 3;
		cylinders /= 25;
	} else if (_xstrcasecmp(ext, _T(".HDI")) == 0) { // Anex86 HD image? 
		imgoffset = getDwordLE(&hdr[4 + 4]);
		imgsize = getDwordLE(&hdr[4 + 4 + 4]);
		sectorsize = getDwordLE(&hdr[4 + 4 + 4 + 4]);
		sectors = getDwordLE(&hdr[4 + 4 + 4 + 4 + 4]);
		heads = getDwordLE(&hdr[4 + 4 + 4 + 4 + 4 + 4]);
		cylinders = getDwordLE(&hdr[4 + 4 + 4 + 4 + 4 + 4 + 4]);
	} else if (_xstrcasecmp(ext, _T(".NHD")) == 0 &&
		memcmp(hdr, "T98HDDIMAGE.R0\0", 15) == 0) { // T98Next HD image?
		imgoffset = getDwordLE(&hdr[0x10 + 0x100]);
		cylinders = getDwordLE(&hdr[0x10 + 0x100 + 4]);
		heads = getWordLE(&hdr[0x10 + 0x100 + 4 + 4]);
		sectors = getWordLE(&hdr[0x10 + 0x100 + 4 + 4 + 2]);
		sectorsize = getWordLE(&hdr[0x10 + 0x100 + 4 + 4 + 2 + 2]);
		imgsize = (fsize_t)cylinders * heads * sectors * sectorsize;
	}

	// セクタサイズは256または512をサポート
	if (sectorsize != 256 && sectorsize != 512) {
		return FALSE;
	}

	// イメージサイズの整合性チェック
	if (imgoffset + imgsize > size || (imgsize % sectorsize != 0)) {
		return FALSE;
	}

	// セクタサイズ
	for(disk.size = 16; disk.size > 0; --(disk.size)) {
		if ((1 << disk.size) == sectorsize)
			break;
	}
	if (disk.size <= 0 || disk.size > 16) {
		return FALSE;
	}

	// ブロック数
	disk.blocks = (DWORD)(imgsize >> disk.size);
	disk.imgoffset = imgoffset;

	// 基本クラス
	return Disk::Open(path);
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD_NEC::Inquiry(
	const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	int size;

	// 基底クラス
	size = SCSIHD::Inquiry(cdb, buf, major, minor);

	// 基底クラスでエラーなら終了
	if (size == 0) {
		return 0;
	}

	// SCSI1相当に変更
	buf[2] = 0x01;
	buf[3] = 0x01;

	// ベンダー名差し替え
	buf[8] = 'N';
	buf[9] = 'E';
	buf[10] = 'C';

	return size;
}

//---------------------------------------------------------------------------
//
//	エラーページ追加
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD_NEC::AddError(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x01;
	buf[1] = 0x06;

	// 変更可能領域はない
	if (change) {
		return 8;
	}

	// リトライカウントは0、リミットタイムは装置内部のデフォルト値を使用
	return 8;
}

//---------------------------------------------------------------------------
//
//	フォーマットページ追加
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD_NEC::AddFormat(BOOL change, BYTE *buf)
{
	int size;

	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x80 | 0x03;
	buf[1] = 0x16;

	// 物理セクタのバイト数は変更可能に見せる(実際には変更できないが)
	if (change) {
		buf[0xc] = 0xff;
		buf[0xd] = 0xff;
		return 24;
	}

	if (disk.ready) {
		// 1ゾーンのトラック数を設定(PC-9801-55はこの値を見ているようだ)
		buf[0x2] = (BYTE)(heads >> 8);
		buf[0x3] = (BYTE)heads;
		
		// 1トラックのセクタ数を設定
		buf[0xa] = (BYTE)(sectors >> 8);
		buf[0xb] = (BYTE)sectors;

		// 物理セクタのバイト数を設定
		size = 1 << disk.size;
		buf[0xc] = (BYTE)(size >> 8);
		buf[0xd] = (BYTE)size;
	}

	// リムーバブル属性を設定(昔の名残)
	if (disk.removable) {
		buf[20] = 0x20;
	}

	return 24;
}

//---------------------------------------------------------------------------
//
//	ドライブページ追加
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD_NEC::AddDrive(BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// コード・レングスを設定
	buf[0] = 0x04;
	buf[1] = 0x12;

	// 変更可能領域はない
	if (change) {
		return 20;
	}

	if (disk.ready) {
		// シリンダ数を設定
		buf[0x2] = (BYTE)(cylinders >> 16);
		buf[0x3] = (BYTE)(cylinders >> 8);
		buf[0x4] = (BYTE)cylinders;
		
		// ヘッド数を設定
		buf[0x5] = (BYTE)heads;
	}

	return 20;
}

//===========================================================================
//
//	SCSI ハードディスク(Macintosh Apple純正)
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSIHD_APPLE::SCSIHD_APPLE() : SCSIHD()
{
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD_APPLE::Inquiry(
	const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	int size;
	char vendor[32];
	char product[32];

	// 基底クラス
	size = SCSIHD::Inquiry(cdb, buf, major, minor);

	// 基底クラスでエラーなら終了
	if (size == 0) {
		return 0;
	}

	// ベンダ名
	sprintf(vendor, " SEAGATE");
	memcpy(&buf[8], vendor, strlen(vendor));

	// 製品名
	sprintf(product, "          ST225N");
	memcpy(&buf[16], product, strlen(product));

	return size;
}

//---------------------------------------------------------------------------
//
//	Vendor特殊ページ追加
//
//---------------------------------------------------------------------------
int FASTCALL SCSIHD_APPLE::AddVendor(int page, BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// ページコード48
	if ((page != 0x30) && (page != 0x3f)) {
		return 0;
	}

	// コード・レングスを設定
	buf[0] = 0x30;
	buf[1] = 0x1c;

	// 変更可能領域はない
	if (change) {
		return 30;
	}

	// APPLE COMPUTER, INC.
	memcpy(&buf[0xa], "APPLE COMPUTER, INC.", 20);

	return 30;
}

//===========================================================================
//
//	SCSI 光磁気ディスク
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSIMO::SCSIMO() : Disk()
{
	// SCSI 光磁気ディスク
	disk.id = MAKEID('S', 'C', 'M', 'O');

	// リムーバブル
	disk.removable = TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIMO::Open(const Filepath& path, BOOL attn)
{
	fsize_t size;

	ASSERT(this);
	ASSERT(!disk.ready);

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// ファイルサイズの取得
	size = fio.GetFileSize();
	fio.Close();

	switch (size) {
		// 128MB
		case 0x797f400:
			disk.size = 9;
			disk.blocks = 248826;
			break;

		// 230MB
		case 0xd9eea00:
			disk.size = 9;
			disk.blocks = 446325;
			break;

		// 540MB
		case 0x1fc8b800:
			disk.size = 9;
			disk.blocks = 1041500;
			break;

		// 640MB
		case 0x25e28000:
			disk.size = 11;
			disk.blocks = 310352;
			break;

		// その他(エラー)
		default:
			return FALSE;
	}

	// 基本クラス
	Disk::Open(path);

	// レディならアテンション
	if (disk.ready && attn) {
		disk.attn = TRUE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIMO::Inquiry(
	const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	int size;
	char rev[32];

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPDチェック
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// 基本データ
	// buf[0] ... Optical Memory Device
	// buf[1] ... リムーバブル
	// buf[2] ... SCSI-2準拠のコマンド体系
	// buf[3] ... SCSI-2準拠のInquiryレスポンス
	// buf[4] ... Inquiry追加データ
	memset(buf, 0, 8);
	buf[0] = 0x07;

	// SCSI-2 本の p.104 4.4.3 不当なロジカルユニットの処理
	if (((cdb[1] >> 5) & 0x07) != disk.lun) {
		buf[0] = 0x7f;
	}

	buf[1] = 0x80;
	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 36 - 5;	// 必須

	// 空白で埋める
	memset(&buf[8], 0x20, buf[4] - 3);

	// ベンダ
	memcpy(&buf[8], DEFAULT_VENDER, strlen(DEFAULT_VENDER));

	// 製品名
	memcpy(&buf[16], "M2513A", 6);

	// リビジョン(XM6のバージョンNo)
	sprintf(rev, "0%01d%01d%01d",
				(int)major, (int)(minor >> 4), (int)(minor & 0x0f));
	memcpy(&buf[32], rev, 4);

	// 返却できるデータのサイズ
	size = (buf[4] + 5);

	// 相手のバッファが少なければ制限する
	if (size > (int)cdb[4]) {
		size = (int)cdb[4];
	}

	// 成功
	disk.code = DISK_NOERROR;
	return size;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//	※disk.codeの影響を受けない
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIMO::ModeSelect(const DWORD *cdb, const BYTE *buf, int length)
{
	int page;
	int size;

	ASSERT(this);
	ASSERT(buf);
	ASSERT(length >= 0);

	// PF
	if (cdb[1] & 0x10) {
		// Mode Parameter header
		if (length >= 12) {
			// ブロックレングスのバイト数をチェック
			size = 1 << disk.size;
			if (buf[9] != (BYTE)(size >> 16) ||
				buf[10] != (BYTE)(size >> 8) || buf[11] != (BYTE)size) {
				// 今のところセクタ長の変更は許さない
				disk.code = DISK_INVALIDPRM;
				return FALSE;
			}
			buf += 12;
			length -= 12;
		}

		// ページの解析
		while (length > 0) {
			// ページの取得
			page = buf[0];

			switch (page) {
				// format device
				case 0x03:
					// 物理セクタのバイト数をチェック
					size = 1 << disk.size;
					if (buf[0xc] != (BYTE)(size >> 8) ||
						buf[0xd] != (BYTE)size) {
						// 今のところセクタ長の変更は許さない
						disk.code = DISK_INVALIDPRM;
						return FALSE;
					}
					break;
				// vendor unique format
				case 0x20:
					// just ignore, for now
					break;

				// その他ページ
				default:
					break;
			}

			// 次のページへ
			size = buf[1] + 2;
			length -= size;
			buf += size;
		}
	}

	// とりあえずエラーは発生させない(MINIX)
	disk.code = DISK_NOERROR;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Vendor Unique Format Page 20h (MO)
//
//---------------------------------------------------------------------------
int FASTCALL SCSIMO::AddVendor(int page, BOOL change, BYTE *buf)
{
	ASSERT(this);
	ASSERT(buf);

	// ページコード20h
	if ((page != 0x20) && (page != 0x3f)) {
		return 0;
	}

	// コード・レングスを設定
	buf[0] = 0x20;
	buf[1] = 0x0a;

	// 変更可能領域はない
	if (change) {
		return 12;
	}

	/*
		mode page code 20h - Vendor Unique Format Page
		format mode XXh type 0
		information: http://h20628.www2.hp.com/km-ext/kmcsdirect/emr_na-lpg28560-1.pdf
		
		offset  description
		  02h   format mode
		  03h   type of format (0)
		04~07h  size of user band (total sectors?)
		08~09h  size of spare band (spare sectors?)
		0A~0Bh  number of bands
		
		actual value of each 3.5inches optical medium (grabbed by Fujitsu M2513EL)
		
		                     128M     230M    540M    640M
		---------------------------------------------------
		size of user band   3CBFAh   6CF75h  FE45Ch  4BC50h
		size of spare band   0400h    0401h   08CAh   08C4h
		number of bands      0001h    000Ah   0012h   000Bh
		
		further information: http://r2089.blog36.fc2.com/blog-entry-177.html
	*/

	if (disk.ready) {
		unsigned spare = 0;
		unsigned bands = 0;
		
		if (disk.size == 9) switch (disk.blocks) {
			// 128MB
			case 248826:
				spare = 1024;
				bands = 1;
				break;

			// 230MB
			case 446325:
				spare = 1025;
				bands = 10;
				break;

			// 540MB
			case 1041500:
				spare = 2250;
				bands = 18;
				break;
		}

		if (disk.size == 11) switch (disk.blocks) {
			// 640MB
			case 310352:
				spare = 2244;
				bands = 11;
				break;

			// 1.3GB (lpproj: not tested with real device)
			case 605846:
				spare = 4437;
				bands = 18;
				break;
		}

		buf[2] = 0; // format mode
		buf[3] = 0; // type of format
		buf[4] = (BYTE)(disk.blocks >> 24);
		buf[5] = (BYTE)(disk.blocks >> 16);
		buf[6] = (BYTE)(disk.blocks >> 8);
		buf[7] = (BYTE)disk.blocks;
		buf[8] = (BYTE)(spare >> 8);
		buf[9] = (BYTE)spare;
		buf[10] = (BYTE)(bands >> 8);
		buf[11] = (BYTE)bands;
	}

	return 12;
}

//===========================================================================
//
//	CDトラック
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
CDTrack::CDTrack(SCSICD *scsicd)
{
	ASSERT(scsicd);

	// 親となるCD-ROMデバイスを設定
	cdrom = scsicd;

	// トラック無効
	valid = FALSE;

	// その他のデータを初期化
	track_no = -1;
	first_lba = 0;
	last_lba = 0;
	audio = FALSE;
	raw = FALSE;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
CDTrack::~CDTrack()
{
}

//---------------------------------------------------------------------------
//
//	初期化
//
//---------------------------------------------------------------------------
BOOL FASTCALL CDTrack::Init(int track, DWORD first, DWORD last)
{
	ASSERT(this);
	ASSERT(!valid);
	ASSERT(track >= 1);
	ASSERT(first < last);

	// トラック番号を設定、有効化
	track_no = track;
	valid = TRUE;

	// LBAを記憶
	first_lba = first;
	last_lba = last;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	パス設定
//
//---------------------------------------------------------------------------
void FASTCALL CDTrack::SetPath(BOOL cdda, const Filepath& path)
{
	ASSERT(this);
	ASSERT(valid);

	// CD-DAか、データか
	audio = cdda;

	// パス記憶
	imgpath = path;
}

//---------------------------------------------------------------------------
//
//	パス取得
//
//---------------------------------------------------------------------------
void FASTCALL CDTrack::GetPath(Filepath& path) const
{
	ASSERT(this);
	ASSERT(valid);

	// パスを返す
	path = imgpath;
}

//---------------------------------------------------------------------------
//
//	インデックス追加
//
//---------------------------------------------------------------------------
void FASTCALL CDTrack::AddIndex(int index, DWORD lba)
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(index > 0);
	ASSERT(first_lba <= lba);
	ASSERT(lba <= last_lba);

	// 現在はインデックスはサポートしない
	ASSERT(FALSE);
}

//---------------------------------------------------------------------------
//
//	開始LBA取得
//
//---------------------------------------------------------------------------
DWORD FASTCALL CDTrack::GetFirst() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(first_lba < last_lba);

	return first_lba;
}

//---------------------------------------------------------------------------
//
//	終端LBA取得
//
//---------------------------------------------------------------------------
DWORD FASTCALL CDTrack::GetLast() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(first_lba < last_lba);

	return last_lba;
}

//---------------------------------------------------------------------------
//
//	ブロック数取得
//
//---------------------------------------------------------------------------
DWORD FASTCALL CDTrack::GetBlocks() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(first_lba < last_lba);

	// 開始LBAと最終LBAから算出
	return (DWORD)(last_lba - first_lba + 1);
}

//---------------------------------------------------------------------------
//
//	トラック番号取得
//
//---------------------------------------------------------------------------
int FASTCALL CDTrack::GetTrackNo() const
{
	ASSERT(this);
	ASSERT(valid);
	ASSERT(track_no >= 1);

	return track_no;
}

//---------------------------------------------------------------------------
//
//	有効なブロックか
//
//---------------------------------------------------------------------------
BOOL FASTCALL CDTrack::IsValid(DWORD lba) const
{
	ASSERT(this);

	// トラック自体が無効なら、FALSE
	if (!valid) {
		return FALSE;
	}

	// firstより前なら、FALSE
	if (lba < first_lba) {
		return FALSE;
	}

	// lastより後なら、FALSE
	if (last_lba < lba) {
		return FALSE;
	}

	// このトラック
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オーディオトラックか
//
//---------------------------------------------------------------------------
BOOL FASTCALL CDTrack::IsAudio() const
{
	ASSERT(this);
	ASSERT(valid);

	return audio;
}

//===========================================================================
//
//	CD-DAバッファ
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
CDDABuf::CDDABuf()
{
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
CDDABuf::~CDDABuf()
{
}

//===========================================================================
//
//	SCSI CD-ROM
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSICD::SCSICD() : Disk()
{
	int i;

	// SCSI CD-ROM
	disk.id = MAKEID('S', 'C', 'C', 'D');

	// リムーバブル、書込み禁止
	disk.removable = TRUE;
	disk.writep = TRUE;

	// RAW形式でない
	rawfile = FALSE;

	// フレーム初期化
	frame = 0;

	// トラック初期化
	for (i = 0; i < TrackMax; i++) {
		track[i] = NULL;
	}
	tracks = 0;
	dataindex = -1;
	audioindex = -1;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
SCSICD::~SCSICD()
{
	// トラッククリア
	ClearTrack();
}

//---------------------------------------------------------------------------
//
//	オープン
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::Open(const Filepath& path, BOOL attn)
{
	fsize_t size;
	TCHAR file[5];

	ASSERT(this);
	ASSERT(!disk.ready);

	// 初期化、トラッククリア
	disk.blocks = 0;
	rawfile = FALSE;
	ClearTrack();

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// 物理CDアクセスならクローズして委譲する
	if (path.GetPath()[0] == _T('\\')) {
		// クローズ
		fio.Close();

		// 物理CDオープン
		if (!OpenPhysical(path)) {
			return FALSE;
		}
	} else {
		// サイズ取得
		size = fio.GetFileSize();
		if (size <= 4) {
			fio.Close();
			return FALSE;
		}

		// CUEシートか、ISOファイルかの判定を行う
		fio.Read(file, 4);
		file[4] = '\0';
		fio.Close();

		// FILEで始まっていれば、CUEシートとみなす
		if (_xstrncasecmp(file, _T("FILE"), 4) == 0) {
			// CUEとしてオープン
			if (!OpenCue(path)) {
				return FALSE;
			}
		} else {
			// ISOとしてオープン
			if (!OpenIso(path)) {
				return FALSE;
			}
		}
	}

	// オープン成功
	ASSERT(disk.blocks > 0);
	disk.size = 11;

	// 基本クラス
	Disk::Open(path);

	// RAWフラグを設定
	ASSERT(disk.dcache);
	disk.dcache->SetRawMode(rawfile);

	// ROMメディアなので、書き込みはできない
	disk.writep = TRUE;

	// レディならアテンション
	if (disk.ready && attn) {
		disk.attn = TRUE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン(CUE)
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::OpenCue(const Filepath& /*path*/)
{
	ASSERT(this);

	// 常に失敗
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	オープン(ISO)
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::OpenIso(const Filepath& path)
{
	fsize_t size;
	BYTE header[12];
	BYTE sync[12];

	ASSERT(this);

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// サイズ取得
	size = fio.GetFileSize();
	if (size < 0x800) {
		fio.Close();
		return FALSE;
	}

	// 最初の12バイトを読み取って、クローズ
	if (!fio.Read(header, sizeof(header))) {
		fio.Close();
		return FALSE;
	}

	// RAW形式かチェック
	memset(sync, 0xff, sizeof(sync));
	sync[0] = 0x00;
	sync[11] = 0x00;
	rawfile = FALSE;
	if (memcmp(header, sync, sizeof(sync)) == 0) {
		// 00,FFx10,00なので、RAW形式と推定される
		if (!fio.Read(header, 4)) {
			fio.Close();
			return FALSE;
		}

		// MODE1/2048またはMODE1/2352のみサポート
		if (header[3] != 0x01) {
			// モードが違う
			fio.Close();
			return FALSE;
		}

		// RAWファイルに設定
		rawfile = TRUE;
	}
	fio.Close();

	if (rawfile) {
		// サイズが2536の倍数で、700MB以下であること
		if (size % 0x930) {
			return FALSE;
		}
		if (size > 912579600) {
			return FALSE;
		}

		// ブロック数を設定
		disk.blocks = (DWORD)(size / 0x930);
	} else {
		// サイズが2048の倍数で、700MB以下であること
		if (size & 0x7ff) {
			return FALSE;
		}
		if (size > 0x2bed5000) {
			return FALSE;
		}

		// ブロック数を設定
		disk.blocks = (DWORD)(size >> 11);
	}

	// データトラック1つのみ作成
	ASSERT(!track[0]);
	track[0] = new CDTrack(this);
	track[0]->Init(1, 0, disk.blocks - 1);
	track[0]->SetPath(FALSE, path);
	tracks = 1;
	dataindex = 0;

	// オープン成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	オープン(Physical)
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::OpenPhysical(const Filepath& path)
{
	fsize_t size;

	ASSERT(this);

	// 読み込みオープンが必要
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// サイズ取得
	size = fio.GetFileSize();
	if (size < 0x800) {
		fio.Close();
		return FALSE;
	}

	// クローズ
	fio.Close();

	// サイズが2048の倍数で、700MB以下であること
	if (size & 0x7ff) {
		return FALSE;
	}
	if (size > 0x2bed5000) {
		return FALSE;
	}

	// ブロック数を設定
	disk.blocks = (DWORD)(size >> 11);

	// データトラック1つのみ作成
	ASSERT(!track[0]);
	track[0] = new CDTrack(this);
	track[0]->Init(1, 0, disk.blocks - 1);
	track[0]->SetPath(FALSE, path);
	tracks = 1;
	dataindex = 0;

	// オープン成功
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::Inquiry(
	const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	char rev[32];
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPDチェック
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// 基本データ
	// buf[0] ... CD-ROM Device
	// buf[1] ... リムーバブル
	// buf[2] ... SCSI-2準拠のコマンド体系
	// buf[3] ... SCSI-2準拠のInquiryレスポンス
	// buf[4] ... Inquiry追加データ
	memset(buf, 0, 8);
	buf[0] = 0x05;

	// SCSI-2 本の p.104 4.4.3 不当なロジカルユニットの処理
	if (((cdb[1] >> 5) & 0x07) != disk.lun) {
		buf[0] = 0x7f;
	}

	buf[1] = 0x80;
	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 36 - 5;	// 必須

	// 空白で埋める
	memset(&buf[8], 0x20, buf[4] - 3);

	// ベンダ
	memcpy(&buf[8], DEFAULT_VENDER, strlen(DEFAULT_VENDER));

	// 製品名
	memcpy(&buf[16], "CD-ROM CDU-55S", 14);

	// リビジョン(XM6のバージョンNo)
	sprintf(rev, "0%01d%01d%01d",
				(int)major, (int)(minor >> 4), (int)(minor & 0x0f));
	memcpy(&buf[32], rev, 4);

	// 返却できるデータのサイズ
	size = (buf[4] + 5);

	// 相手のバッファが少なければ制限する
	if (size > (int)cdb[4]) {
		size = (int)cdb[4];
	}

	// 成功
	disk.code = DISK_NOERROR;
	return size;
}

//---------------------------------------------------------------------------
//
//	READ
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::Read(BYTE *buf, DWORD block)
{
	int index;
	Filepath path;

	ASSERT(this);
	ASSERT(buf);

	// 状態チェック
	if (!CheckReady()) {
		return 0;
	}

	// トラック検索
	index = SearchTrack(block);

	// 無効なら、範囲外
	if (index < 0) {
		disk.code = DISK_INVALIDLBA;
		return 0;
	}
	ASSERT(track[index]);

	// 現在のデータトラックと異なっていれば
	if (dataindex != index) {
		// 現在のディスクキャッシュを削除(Saveの必要はない)
		delete disk.dcache;
		disk.dcache = NULL;

		// ブロック数を再設定
		disk.blocks = track[index]->GetBlocks();
		ASSERT(disk.blocks > 0);

		// ディスクキャッシュを作り直す
		track[index]->GetPath(path);
		disk.dcache = new DiskCache(this, disk.size, disk.blocks);
		disk.dcache->SetRawMode(rawfile);

		// データインデックスを再設定
		dataindex = index;
	}

	// 基本クラス
	ASSERT(dataindex >= 0);
	return Disk::Read(buf, block);
}

//---------------------------------------------------------------------------
//
//	READ TOC
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::ReadToc(const DWORD *cdb, BYTE *buf)
{
	int last;
	int index;
	int length;
	int loop;
	int i;
	BOOL msf;
	DWORD lba;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x43);
	ASSERT(buf);

	// レディチェック
	if (!CheckReady()) {
		return 0;
	}

	// レディであるなら、トラックが最低1つ以上存在する
	ASSERT(tracks > 0);
	ASSERT(track[0]);

	// アロケーションレングス取得、バッファクリア
	length = cdb[7] << 8;
	length |= cdb[8];
	memset(buf, 0, length);

	// MSFフラグ取得
	if (cdb[1] & 0x02) {
		msf = TRUE;
	} else {
		msf = FALSE;
	}

	// 最終トラック番号を取得、チェック
	last = track[tracks - 1]->GetTrackNo();
	if ((int)cdb[6] > last) {
		// ただしAAは除外
		if (cdb[6] != 0xaa) {
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// 開始インデックスをチェック
	index = 0;
	if (cdb[6] != 0x00) {
		// トラック番号が一致するまで、トラックを進める
		while (track[index]) {
			if ((int)cdb[6] == track[index]->GetTrackNo()) {
				break;
			}
			index++;
		}

		// 見つからなければAAか、内部エラー
		if (!track[index]) {
			if (cdb[6] == 0xaa) {
				// AAなので、最終LBA+1を返す
				buf[0] = 0x00;
				buf[1] = 0x0a;
				buf[2] = (BYTE)track[0]->GetTrackNo();
				buf[3] = (BYTE)last;
				buf[6] = 0xaa;
				lba = track[tracks - 1]->GetLast() + 1;
				if (msf) {
					LBAtoMSF(lba, &buf[8]);
				} else {
					buf[10] = (BYTE)(lba >> 8);
					buf[11] = (BYTE)lba;
				}
				return length;
			}

			// それ以外はエラー
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// 今回返すトラックディスクリプタの個数(ループ数)
	loop = last - track[index]->GetTrackNo() + 1;
	ASSERT(loop >= 1);

	// ヘッダ作成
	buf[0] = (BYTE)(((loop << 3) + 2) >> 8);
	buf[1] = (BYTE)((loop << 3) + 2);
	buf[2] = (BYTE)track[0]->GetTrackNo();
	buf[3] = (BYTE)last;
	buf += 4;

	// ループ
	for (i = 0; i < loop; i++) {
		// ADRとControl
		if (track[index]->IsAudio()) {
			// オーディオトラック
			buf[1] = 0x10;
		} else {
			// データトラック
			buf[1] = 0x14;
		}

		// トラック番号
		buf[2] = (BYTE)track[index]->GetTrackNo();

		// トラックアドレス
		if (msf) {
			LBAtoMSF(track[index]->GetFirst(), &buf[4]);
		} else {
			buf[6] = (BYTE)(track[index]->GetFirst() >> 8);
			buf[7] = (BYTE)(track[index]->GetFirst());
		}

		// バッファとインデックスを進める
		buf += 8;
		index++;
	}

	// アロケーションレングスだけ必ず返す
	return length;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::PlayAudio(const DWORD* /*cdb*/)
{
	ASSERT(this);

	disk.code = DISK_INVALIDCDB;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO MSF
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::PlayAudioMSF(const DWORD* /*cdb*/)
{
	ASSERT(this);

	disk.code = DISK_INVALIDCDB;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO TRACK
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::PlayAudioTrack(const DWORD* /*cdb*/)
{
	ASSERT(this);

	disk.code = DISK_INVALIDCDB;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	LBA→MSF変換
//
//---------------------------------------------------------------------------
void FASTCALL SCSICD::LBAtoMSF(DWORD lba, BYTE *msf) const
{
	DWORD m;
	DWORD s;
	DWORD f;

	ASSERT(this);

	// 75、75*60でそれぞれ余りを出す
	m = lba / (75 * 60);
	s = lba % (75 * 60);
	f = s % 75;
	s /= 75;

	// 基点はM=0,S=2,F=0
	s += 2;
	if (s >= 60) {
		s -= 60;
		m++;
	}

	// 格納
	ASSERT(m < 0x100);
	ASSERT(s < 60);
	ASSERT(f < 75);
	msf[0] = 0x00;
	msf[1] = (BYTE)m;
	msf[2] = (BYTE)s;
	msf[3] = (BYTE)f;
}

//---------------------------------------------------------------------------
//
//	MSF→LBA変換
//
//---------------------------------------------------------------------------
DWORD FASTCALL SCSICD::MSFtoLBA(const BYTE *msf) const
{
	DWORD lba;

	ASSERT(this);
	ASSERT(msf[2] < 60);
	ASSERT(msf[3] < 75);

	// 1, 75, 75*60の倍数で合算
	lba = msf[1];
	lba *= 60;
	lba += msf[2];
	lba *= 75;
	lba += msf[3];

	// 基点はM=0,S=2,F=0なので、150を引く
	lba -= 150;

	return lba;
}

//---------------------------------------------------------------------------
//
//	トラッククリア
//
//---------------------------------------------------------------------------
void FASTCALL SCSICD::ClearTrack()
{
	int i;

	ASSERT(this);

	// トラックオブジェクトを削除
	for (i = 0; i < TrackMax; i++) {
		if (track[i]) {
			delete track[i];
			track[i] = NULL;
		}
	}

	// トラック数0
	tracks = 0;

	// データ、オーディオとも設定なし
	dataindex = -1;
	audioindex = -1;
}

//---------------------------------------------------------------------------
//
//	トラック検索
//	※見つからなければ-1を返す
//
//---------------------------------------------------------------------------
int FASTCALL SCSICD::SearchTrack(DWORD lba) const
{
	int i;

	ASSERT(this);

	// トラックループ
	for (i = 0; i < tracks; i++) {
		// トラックに聞く
		ASSERT(track[i]);
		if (track[i]->IsValid(lba)) {
			return i;
		}
	}

	// 見つからなかった
	return -1;
}

//---------------------------------------------------------------------------
//
//	フレーム通知
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSICD::NextFrame()
{
	ASSERT(this);
	ASSERT((frame >= 0) && (frame < 75));

	// フレームを0～74の範囲で設定
	frame = (frame + 1) % 75;

	// 1周したらFALSE
	if (frame != 0) {
		return TRUE;
	} else {
		return FALSE;
	}
}

//---------------------------------------------------------------------------
//
//	CD-DAバッファ取得
//
//---------------------------------------------------------------------------
void FASTCALL SCSICD::GetBuf(
	DWORD* /*buffer*/, int /*samples*/, DWORD /*rate*/)
{
	ASSERT(this);
}

//===========================================================================
//
//	SCSI ホストブリッジ
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSIBR::SCSIBR() : Disk()
{
	// ホストブリッジ
	disk.id = MAKEID('S', 'C', 'B', 'R');

	// メッセージハンドラ関数ポインタクリア
	memset(pMsgFunc, 0x00, sizeof(pMsgFunc));
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
SCSIBR::~SCSIBR()
{
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int FASTCALL SCSIBR::Inquiry(
	const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	int i;
	char rev[32];
	int size;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPDチェック
	if (cdb[1] & 0x01) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// 基本データ
	// buf[0] ... Communication Device
	// buf[2] ... SCSI-2準拠のコマンド体系
	// buf[3] ... SCSI-2準拠のInquiryレスポンス
	// buf[4] ... Inquiry追加データ
	memset(buf, 0, 8);
	buf[0] = 0x09;

	// SCSI-2 本の p.104 4.4.3 不当なロジカルユニットの処理
	if (((cdb[1] >> 5) & 0x07) != disk.lun) {
		buf[0] = 0x7f;
	}

	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 36 - 5 + 8;	// 必須 + 8バイト拡張

	// 空白で埋める
	memset(&buf[8], 0x20, buf[4] - 3);

	// ベンダ
	memcpy(&buf[8], DEFAULT_VENDER, strlen(DEFAULT_VENDER));

	// 製品名
	memcpy(&buf[16], BRIDGE_PRODUCT, strlen(BRIDGE_PRODUCT));

	// リビジョン(XM6のバージョンNo)
	sprintf(rev, "0%01d%01d%01d",
				(int)major, (int)(minor >> 4), (int)(minor & 0x0f));
	memcpy(&buf[32], rev, 4);

	// メッセージハンドラ関数からオプション有効フラグに反映
	for (i = 0; i < 8; i++) {
		buf[36 + i] = pMsgFunc[0] ? '1' : '0';
	}

	// 返却できるデータのサイズ
	size = (buf[4] + 5);

	// 相手のバッファが少なければ制限する
	if (size > (int)cdb[4]) {
		size = (int)cdb[4];
	}

	// 成功
	disk.code = DISK_NOERROR;
	return size;
}

//---------------------------------------------------------------------------
//
//	TEST UNIT READY
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIBR::TestUnitReady(const DWORD* /*cdb*/)
{
	ASSERT(this);

	// TEST UNIT READY成功
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	GET MESSAGE(10)
//
//---------------------------------------------------------------------------
int FASTCALL SCSIBR::GetMessage10(const DWORD *cdb, BYTE *buf)
{
	int type;
	int func;
	int phase;
	int len;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);

	// タイプ
	type = cdb[2];

	// 機能番号
	func = cdb[3];

	// フェーズ
	phase = cdb[9];

	// リード数を取得
	len = cdb[6];
	len <<= 8;
	len |= cdb[7];
	len <<= 8;
	len |= cdb[8];

	// タイプチェック
	if (type < 0 || type > 7) {
		return -1;
	}

	// コールバック登録チェック
	if (!pMsgFunc[type]) {
		return -1;
	}

	// コールバック
	return pMsgFunc[type](TRUE, func, phase, len, buf);
}

//---------------------------------------------------------------------------
//
//	SEND MESSAGE(10)
//
//---------------------------------------------------------------------------
int FASTCALL SCSIBR::SendMessage10(const DWORD *cdb, BYTE *buf)
{
	int type;
	int func;
	int phase;
	int len;

	ASSERT(this);
	ASSERT(cdb);
	ASSERT(buf);

	// タイプ
	type = cdb[2];

	// 機能番号
	func = cdb[3];

	// フェーズ
	phase = cdb[9];

	// ライト数を取得
	len = cdb[6];
	len <<= 8;
	len |= cdb[7];
	len <<= 8;
	len |= cdb[8];

	// タイプチェック
	if (type < 0 || type > 7) {
		return -1;
	}

	// コールバック登録チェック
	if (!pMsgFunc[type]) {
		return -1;
	}

	// コールバック
	return pMsgFunc[type](FALSE, func, phase, len, buf);
}

//---------------------------------------------------------------------------
//
//	メッセージハンドラ登録
//
//---------------------------------------------------------------------------
void FASTCALL SCSIBR::SetMsgFunc(int type, MsgFunc f)
{
	// タイプチェック
	if (type < 0 || type > 7) {
		return;
	}

	// ハンドラ記録
	pMsgFunc[type] = f;
}

//===========================================================================
//
//	SASI デバイス
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SASIDEV::SASIDEV()
{
	int i;

	// ワーク初期化
	ctrl.phase = BUS::busfree;
	ctrl.id = -1;
	ctrl.bus = NULL;
	memset(ctrl.cmd, 0x00, sizeof(ctrl.cmd));
	ctrl.status = 0x00;
	ctrl.message = 0x00;
#if USE_WAIT_CTRL == 1
	ctrl.execstart = 0;
#endif	// USE_WAIT_CTRL
	ctrl.bufsize = 0x800;
	ctrl.buffer = (BYTE *)malloc(ctrl.bufsize);
	memset(ctrl.buffer, 0x00, ctrl.bufsize);
	ctrl.blocks = 0;
	ctrl.next = 0;
	ctrl.offset = 0;
	ctrl.length = 0;

	// 論理ユニット初期化
	for (i = 0; i < UnitMax; i++) {
		ctrl.unit[i] = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
SASIDEV::~SASIDEV()
{
	// バッファを開放
	if (ctrl.buffer) {
		free(ctrl.buffer);
		ctrl.buffer = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	デバイスリセット
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Reset()
{
	int i;

	ASSERT(this);

#if defined(DISK_LOG)
		Log(Log::Normal, "デバイスリセット");
#endif	// DISK_LOG

	// ワーク初期化
	memset(ctrl.cmd, 0x00, sizeof(ctrl.cmd));
	ctrl.phase = BUS::busfree;
	ctrl.status = 0x00;
	ctrl.message = 0x00;
#if USE_WAIT_CTRL == 1
	ctrl.execstart = 0;
#endif	// USE_WAIT_CTRL
	memset(ctrl.buffer, 0x00, ctrl.bufsize);
	ctrl.blocks = 0;
	ctrl.next = 0;
	ctrl.offset = 0;
	ctrl.length = 0;

	// ユニット初期化
	for (i = 0; i < UnitMax; i++) {
		if (ctrl.unit[i]) {
			ctrl.unit[i]->Reset();
		}
	}
}

//---------------------------------------------------------------------------
//
//	コントローラ接続
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Connect(int id, BUS *bus)
{
	ASSERT(this);

	ctrl.id = id;
	ctrl.bus = bus;
}

//---------------------------------------------------------------------------
//
//	論理ユニット取得
//
//---------------------------------------------------------------------------
Disk* FASTCALL SASIDEV::GetUnit(int no)
{
	ASSERT(this);
	ASSERT(no < UnitMax);

	return ctrl.unit[no];
}

//---------------------------------------------------------------------------
//
//	論理ユニット設定
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::SetUnit(int no, Disk *dev)
{
	ASSERT(this);
	ASSERT(no < UnitMax);

	ctrl.unit[no] = dev;
}

//---------------------------------------------------------------------------
//
//	有効な論理ユニットを持っているか返す
//
//---------------------------------------------------------------------------
BOOL FASTCALL SASIDEV::HasUnit()
{
	int i;

	ASSERT(this);

	for (i = 0; i < UnitMax; i++) {
		if (ctrl.unit[i]) {
			return TRUE;
		}
	}

	return FALSE;
}

//---------------------------------------------------------------------------
//
//	内部データ取得
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::GetCTRL(ctrl_t *buffer)
{
	ASSERT(this);
	ASSERT(buffer);

	// 内部ワークをコピー
	*buffer = ctrl;
}

//---------------------------------------------------------------------------
//
//	ビジー状態のユニットを取得
//
//---------------------------------------------------------------------------
Disk* FASTCALL SASIDEV::GetBusyUnit()
{
	DWORD lun;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	return ctrl.unit[lun];
}

//---------------------------------------------------------------------------
//
//	実行
//
//---------------------------------------------------------------------------
BUS::phase_t FASTCALL SASIDEV::Process()
{
	ASSERT(this);

	// 未接続なら何もしない
	if (ctrl.id < 0 || ctrl.bus == NULL) {
		return ctrl.phase;
	}

	// バス情報の取り込み
	ctrl.bus->Aquire();

	// リセット
	if (ctrl.bus->GetRST()) {
#if defined(DISK_LOG)
		Log(Log::Normal, "RESET信号受信");
#endif	// DISK_LOG

		// コントローラをリセット
		Reset();

		// バスもリセット
		ctrl.bus->Reset();
		return ctrl.phase;
	}

	// フェーズ処理
	switch (ctrl.phase) {
		// バスフリー
		case BUS::busfree:
			BusFree();
			break;

		// セレクション
		case BUS::selection:
			Selection();
			break;

		// データアウト(MCI=000)
		case BUS::dataout:
			DataOut();
			break;

		// データイン(MCI=001)
		case BUS::datain:
			DataIn();
			break;

		// コマンド(MCI=010)
		case BUS::command:
			Command();
			break;

		// ステータス(MCI=011)
		case BUS::status:
			Status();
			break;

		// メッセージイン(MCI=111)
		case BUS::msgin:
			MsgIn();
			break;

		// その他
		default:
			ASSERT(FALSE);
			break;
	}

	return ctrl.phase;
}

//---------------------------------------------------------------------------
//
//	バスフリーフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::BusFree()
{
	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::busfree) {

#if defined(DISK_LOG)
		Log(Log::Normal, "バスフリーフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::busfree;

		// 信号線
		ctrl.bus->SetREQ(FALSE);
		ctrl.bus->SetMSG(FALSE);
		ctrl.bus->SetCD(FALSE);
		ctrl.bus->SetIO(FALSE);
		ctrl.bus->SetBSY(FALSE);

		// ステータスとメッセージを初期化
		ctrl.status = 0x00;
		ctrl.message = 0x00;
		return;
	}

	// セレクションフェーズに移行
	if (ctrl.bus->GetSEL() && !ctrl.bus->GetBSY()) {
		Selection();
	}
}

//---------------------------------------------------------------------------
//
//	セレクションフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Selection()
{
	DWORD id;

	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::selection) {
		// IDが一致していなければ無効
		id = 1 << ctrl.id;
		ctrl.bus->Aquire();
		if ((ctrl.bus->GetDAT() & id) == 0) {
			return;
		}

		// 有効なユニットが無ければ終了
		if (!HasUnit()) {
			return;
		}

#if defined(DISK_LOG)
		Log(Log::Normal,
			"セレクションフェーズ ID=%d (デバイスあり)", ctrl.id);
#endif	// DISK_LOG

		// フェーズチェンジ
		ctrl.phase = BUS::selection;

		// BSYを上げて応答
		ctrl.bus->SetBSY(TRUE);
		return;
	}

	// セレクション完了でコマンドフェーズ移行
	if (!ctrl.bus->GetSEL() && ctrl.bus->GetBSY()) {
		Command();
	}
}

//---------------------------------------------------------------------------
//
//	コマンドフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Command()
{
#if USE_BURST_BUS == 1
	int count;
	int i;
#endif	// USE_BURST_BUS

	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::command) {

#if defined(DISK_LOG)
		Log(Log::Normal, "コマンドフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::command;

		// ターゲットが操作する信号線
		ctrl.bus->SetMSG(FALSE);
		ctrl.bus->SetCD(TRUE);
		ctrl.bus->SetIO(FALSE);

		// データ転送は6バイトx1ブロック
		ctrl.offset = 0;
		ctrl.length = 6;
		ctrl.blocks = 1;

#if USE_BURST_BUS == 1
		// コマンド受信ハンドシェイク(最初のコマンドで自動で10バイト受信する)
		count = ctrl.bus->CommandHandShake(ctrl.buffer);
	
		// 1バイトも受信できなければステータスフェーズへ移行
		if (count == 0) {
			Error();
			return;
		}
	
		// 10バイトCDBのチェック
		if (ctrl.buffer[0] >= 0x20 && ctrl.buffer[0] <= 0x7D) {
			ctrl.length = 10;
		}
	
		// 全て受信できなければステータスフェーズへ移行
		if (count != (int)ctrl.length) {
			Error();
			return;
		}
	
		// コマンドデータ転送
		for (i = 0; i < (int)ctrl.length; i++) {
			ctrl.cmd[i] = (DWORD)ctrl.buffer[i];
		}
	
		// レングスとブロックをクリア
		ctrl.length = 0;
		ctrl.blocks = 0;
	
		// 実行フェーズ
		Execute();
#else
		// コマンドを要求
		ctrl.bus->SetREQ(TRUE);
#endif	// USE_BURST_BUS
		return;
	}

#if USE_BURST_BUS != 1
	// リクエスト中
	if (ctrl.bus->GetREQ()) {
		// イニシエータが送信した
		if (ctrl.bus->GetACK()) {
			Receive();
		}
	} else {
		// イニシエータに次を要求
		if (!ctrl.bus->GetACK()) {
			ReceiveNext();
		}
	}
#endif	// USE_BURST_BUS
}

//---------------------------------------------------------------------------
//
//	実行フェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Execute()
{
	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "実行フェーズ コマンド$%02X", ctrl.cmd[0]);
#endif	// DISK_LOG

	// フェーズ設定
	ctrl.phase = BUS::execute;

	// データ転送のための初期化
	ctrl.offset = 0;
	ctrl.blocks = 1;
#if USE_WAIT_CTRL == 1
	ctrl.execstart = ::GetTimeUs();
#endif	// USE_WAIT_CTRL

	// コマンド別処理
	switch (ctrl.cmd[0]) {
		// TEST UNIT READY
		case 0x00:
			CmdTestUnitReady();
			return;

		// REZERO UNIT
		case 0x01:
			CmdRezero();
			return;

		// REQUEST SENSE
		case 0x03:
			CmdRequestSense();
			return;

		// FORMAT UNIT
		case 0x04:
			CmdFormat();
			return;

		// FORMAT UNIT
		case 0x06:
			CmdFormat();
			return;

		// REASSIGN BLOCKS
		case 0x07:
			CmdReassign();
			return;

		// READ(6)
		case 0x08:
			CmdRead6();
			return;

		// WRITE(6)
		case 0x0a:
			CmdWrite6();
			return;

		// SEEK(6)
		case 0x0b:
			CmdSeek6();
			return;

		// ASSIGN(SASIのみ)
		case 0x0e:
			CmdAssign();
			return;

		// SPECIFY(SASIのみ)
		case 0xc2:
			CmdSpecify();
			return;
	}

	// それ以外は対応していない
	Log(Log::Warning, "未対応コマンド $%02X", ctrl.cmd[0]);
	CmdInvalid();
}

//---------------------------------------------------------------------------
//
//	ステータスフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Status()
{
#if USE_WAIT_CTRL == 1
	DWORD time;
#endif	// USE_WAIT_CTRL
	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::status) {

#if USE_WAIT_CTRL == 1
		// 最小実行時間
		if (ctrl.execstart > 0) {
			time = ::GetTimeUs() - ctrl.execstart;
			if (time < min_exec_time) {
				::SleepUs(min_exec_time - time);
			}
			ctrl.execstart = 0;
		} else {
			// フェーズチェンジ時間
			::SleepUs(min_status_time);
		}
#endif	// USE_WAIT_CTRL

#if defined(DISK_LOG)
		Log(Log::Normal, "ステータスフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::status;

		// ターゲットが操作する信号線
		ctrl.bus->SetMSG(FALSE);
		ctrl.bus->SetCD(TRUE);
		ctrl.bus->SetIO(TRUE);

		// データ転送は1バイトx1ブロック
		ctrl.offset = 0;
		ctrl.length = 1;
		ctrl.blocks = 1;
		ctrl.buffer[0] = (BYTE)ctrl.status;

#if USE_BURST_BUS != 1
		// ステータスを要求
		ctrl.bus->SetDAT(ctrl.buffer[0]);
		ctrl.bus->SetREQ(TRUE);

#if defined(DISK_LOG)
		Log(Log::Normal, "ステータスフェーズ $%02X", ctrl.status);
#endif	// DISK_LOG
		return;
#endif	// USE_BURST_BUS
	}

#if USE_BURST_BUS == 1
	// 送信
	SendBurst();
#else
	// リクエスト中
	if (ctrl.bus->GetREQ()) {
		// イニシエータが受信した
		if (ctrl.bus->GetACK()) {
			SendNext();
		}
	} else {
		// イニシエータが次を要求
		if (!ctrl.bus->GetACK()) {
			Send();
		}
	}
#endif	// USE_BURST_BUS
}

//---------------------------------------------------------------------------
//
//	メッセージインフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::MsgIn()
{
	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::msgin) {

#if defined(DISK_LOG)
		Log(Log::Normal, "メッセージインフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::msgin;

		// ターゲットが操作する信号線
		ctrl.bus->SetMSG(TRUE);
		ctrl.bus->SetCD(TRUE);
		ctrl.bus->SetIO(TRUE);

		// length, blocksは設定済み
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.blocks > 0);
		ctrl.offset = 0;

#if USE_BURST_BUS != 1
		// メッセージを要求
		ctrl.bus->SetDAT(ctrl.buffer[ctrl.offset]);
		ctrl.bus->SetREQ(TRUE);

#if defined(DISK_LOG)
		Log(Log::Normal, "メッセージインフェーズ $%02X", ctrl.buffer[ctrl.offset]);
#endif	// DISK_LOG
		return;
#endif	// USE_BURST_BUS
	}

#if USE_BURST_BUS == 1
	// 送信
	SendBurst();
#else
	// リクエスト中
	if (ctrl.bus->GetREQ()) {
		// イニシエータが受信した
		if (ctrl.bus->GetACK()) {
			SendNext();
		}
	} else {
		// イニシエータが次を要求
		if (!ctrl.bus->GetACK()) {
			Send();
		}
	}
#endif	// USE_BURST_BUS
}

//---------------------------------------------------------------------------
//
//	データインフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::DataIn()
{
#if USE_WAIT_CTRL == 1
	DWORD time;
#endif	// USE_WAIT_CTRL

	ASSERT(this);
	ASSERT(ctrl.length >= 0);

	// フェーズチェンジ
	if (ctrl.phase != BUS::datain) {

#if USE_WAIT_CTRL == 1
		// 最小実行時間
		if (ctrl.execstart > 0) {
			time = ::GetTimeUs() - ctrl.execstart;
			if (time < min_data_time) {
				::SleepUs(min_data_time - time);
			}
			ctrl.execstart = 0;
		}
#endif	// USE_WAIT_CTRL

		// レングス0なら、ステータスフェーズへ
		if (ctrl.length == 0) {
			Status();
			return;
		}

#if defined(DISK_LOG)
		Log(Log::Normal, "データインフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::datain;

		// ターゲットが操作する信号線
		ctrl.bus->SetMSG(FALSE);
		ctrl.bus->SetCD(FALSE);
		ctrl.bus->SetIO(TRUE);

		// length, blocksは設定済み
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.blocks > 0);
		ctrl.offset = 0;

#if USE_BURST_BUS != 1
		// データを設定
		ctrl.bus->SetDAT(ctrl.buffer[ctrl.offset]);

		// データを要求
		ctrl.bus->SetREQ(TRUE);
		return;
#endif	// USE_BURST_BUS
	}

#if USE_BURST_BUS == 1
	// 送信
	SendBurst();
#else
	// リクエスト中
	if (ctrl.bus->GetREQ()) {
		// イニシエータが受信した
		if (ctrl.bus->GetACK()) {
			SendNext();
		}
	} else {
		// イニシエータが次を要求
		if (!ctrl.bus->GetACK()) {
			Send();
		}
	}
#endif	// USE_BURST_BUS
}

//---------------------------------------------------------------------------
//
//	データアウトフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::DataOut()
{
#if USE_WAIT_CTRL == 1
	DWORD time;
#endif	// USE_WAIT_CTRL

	ASSERT(this);
	ASSERT(ctrl.length >= 0);

	// フェーズチェンジ
	if (ctrl.phase != BUS::dataout) {

#if USE_WAIT_CTRL == 1
		// 最小実行時間
		if (ctrl.execstart > 0) {
			time = ::GetTimeUs() - ctrl.execstart;
			if (time < min_data_time) {
				::SleepUs(min_data_time - time);
			}
			ctrl.execstart = 0;
		}
#endif	// USE_WAIT_CTRL

		// レングス0なら、ステータスフェーズへ
		if (ctrl.length == 0) {
			Status();
			return;
		}

#if defined(DISK_LOG)
		Log(Log::Normal, "データアウトフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::dataout;

		// ターゲットが操作する信号線
		ctrl.bus->SetMSG(FALSE);
		ctrl.bus->SetCD(FALSE);
		ctrl.bus->SetIO(FALSE);

		// length, blocksは設定済み
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.blocks > 0);
		ctrl.offset = 0;

#if USE_BURST_BUS != 1
		// データを要求
		ctrl.bus->SetREQ(TRUE);
		return;
#endif	// USE_BURST_BUS
	}

#if USE_BURST_BUS == 1
	// 受信
	ReceiveBurst();
#else
	// リクエスト中
	if (ctrl.bus->GetREQ()) {
		// イニシエータが送信した
		if (ctrl.bus->GetACK()) {
			Receive();
		}
	} else {
		// イニシエータに次を要求
		if (!ctrl.bus->GetACK()) {
			ReceiveNext();
		}
	}
#endif	// USE_BURST_BUS
}

//---------------------------------------------------------------------------
//
//	共通エラー
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Error()
{
	DWORD lun;

	ASSERT(this);

	// バス情報の取り込み
	ctrl.bus->Aquire();

	// リセットチェック
	if (ctrl.bus->GetRST()) {
		// コントローラをリセット
		Reset();

		// バスもリセット
		ctrl.bus->Reset();
		return;
	}

	// ステータスフェーズ、メッセージインフェーズはバスフリー
	if (ctrl.phase == BUS::status || ctrl.phase == BUS::msgin) {
		BusFree();
		return;
	}

#if defined(DISK_LOG)
	Log(Log::Warning, "エラー(ステータスフェーズへ)");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;

	// ステータスとメッセージを設定(CHECK CONDITION)
	ctrl.status = (lun << 5) | 0x02;

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	TEST UNIT READY
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdTestUnitReady()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "TEST UNIT READYコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->TestUnitReady(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	REZERO UNIT
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdRezero()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "REZERO UNITコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Rezero(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdRequestSense()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "REQUEST SENSEコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->RequestSense(ctrl.cmd, ctrl.buffer);
	ASSERT(ctrl.length > 0);

#if defined(DISK_LOG)
	Log(Log::Normal, "センスキー $%02X", ctrl.buffer[2]);
#endif	// DISK_LOG

	// リードフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	FORMAT UNIT
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdFormat()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "FORMAT UNITコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Format(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	REASSIGN BLOCKS
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdReassign()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "REASSIGN BLOCKSコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Reassign(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	READ(6)
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdRead6()
{
	DWORD lun;
	DWORD record;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// レコード番号とブロック数を取得
	record = ctrl.cmd[1] & 0x1f;
	record <<= 8;
	record |= ctrl.cmd[2];
	record <<= 8;
	record |= ctrl.cmd[3];
	ctrl.blocks = ctrl.cmd[4];
	if (ctrl.blocks == 0) {
		ctrl.blocks = 0x100;
	}

#if defined(DISK_LOG)
	Log(Log::Normal,
		"READ(6)コマンド レコード=%06X ブロック=%d", record, ctrl.blocks);
#endif	// DISK_LOG

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->Read(ctrl.buffer, record);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.next = record + 1;

	// リードフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	WRITE(6)
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdWrite6()
{
	DWORD lun;
	DWORD record;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// レコード番号とブロック数を取得
	record = ctrl.cmd[1] & 0x1f;
	record <<= 8;
	record |= ctrl.cmd[2];
	record <<= 8;
	record |= ctrl.cmd[3];
	ctrl.blocks = ctrl.cmd[4];
	if (ctrl.blocks == 0) {
		ctrl.blocks = 0x100;
	}

#if defined(DISK_LOG)
	Log(Log::Normal,
		"WRITE(6)コマンド レコード=%06X ブロック=%d", record, ctrl.blocks);
#endif	// DISK_LOG

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->WriteCheck(record);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.next = record + 1;

	// ライトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	SEEK(6)
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdSeek6()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "SEEK(6)コマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Seek(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	ASSIGN
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdAssign()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "ASSIGNコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Assign(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 4バイトのデータをリクエスト
	ctrl.length = 4;

	// ライトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	SPECIFY
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdSpecify()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "SPECIFYコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Assign(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 10バイトのデータをリクエスト
	ctrl.length = 10;

	// ライトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	サポートしていないコマンド
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::CmdInvalid()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "サポートしていないコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (ctrl.unit[lun]) {
		// ドライブでコマンド処理
		ctrl.unit[lun]->InvalidCmd();
	}

	// 失敗(エラー)
	Error();
}

//===========================================================================
//
//	データ転送
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	データ送信
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Send()
{
	BOOL result;

	ASSERT(this);
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(ctrl.bus->GetIO());

	// オフセットとレングス
	ASSERT(ctrl.length >= 1);
	ctrl.offset++;
	ctrl.length--;

	// ACKアサート直後にSendNextでデータ設定済みならリクエストを上げる
	if (ctrl.length != 0) {
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データ引き取り後の処理(リード/データインのみ)
	if (ctrl.phase == BUS::datain) {
		if (ctrl.blocks != 0) {
			// 次のバッファを設定(offset, lengthをセットすること)
			result = XferIn(ctrl.buffer);
			ctrl.bus->SetDAT(ctrl.buffer[ctrl.offset]);
		}
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら送信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// メッセージインフェーズ
		case BUS::msgin:
			// バスフリーフェーズ
			BusFree();
			break;

		// データインフェーズ
		case BUS::datain:
			// ステータスフェーズ
			Status();
			break;

		// ステータスフェーズ
		case BUS::status:
			// メッセージインフェーズ
			ctrl.length = 1;
			ctrl.blocks = 1;
			ctrl.buffer[0] = (BYTE)ctrl.message;
			MsgIn();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	データ送信継続
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::SendNext()
{
	ASSERT(this);

	// REQが上がっている
	ASSERT(ctrl.bus->GetREQ());
	ASSERT(ctrl.bus->GetIO());

	// ターゲットが操作する信号線
	ctrl.bus->SetREQ(FALSE);

	// バッファにデータがあれば先に設定する
	if (ctrl.length > 1) {
		ctrl.bus->SetDAT(ctrl.buffer[ctrl.offset + 1]);
	}
}

//---------------------------------------------------------------------------
//
//	データ受信
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Receive()
{
	DWORD data;

	ASSERT(this);

	// REQが上がっている
	ASSERT(ctrl.bus->GetREQ());
	ASSERT(!ctrl.bus->GetIO());

	// データ取得
	ctrl.bus->Aquire();
	data = (DWORD)ctrl.bus->GetDAT();

	// ターゲットが操作する信号線
	ctrl.bus->SetREQ(FALSE);

	switch (ctrl.phase) {
		// コマンドフェーズ
		case BUS::command:
			ctrl.cmd[ctrl.offset] = data;
#if defined(DISK_LOG)
			Log(Log::Normal, "コマンドフェーズ $%02X", data);
#endif	// DISK_LOG

			// 最初のデータ(オフセット0)によりレングスを再設定
			if (ctrl.offset == 0) {
				if (ctrl.cmd[0] >= 0x20 && ctrl.cmd[0] <= 0x7D) {
					// 10バイトCDB
					ctrl.length = 10;
				}
			}
			break;

		// データアウトフェーズ
		case BUS::dataout:
			ctrl.buffer[ctrl.offset] = (BYTE)data;
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	データ受信継続
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::ReceiveNext()
{
	BOOL result;

	ASSERT(this);

	// REQが下がっていること
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(!ctrl.bus->GetIO());

	// オフセットとレングス
	ASSERT(ctrl.length >= 1);
	ctrl.offset++;
	ctrl.length--;

	// レングス!=0なら、再びreqをセット
	if (ctrl.length != 0) {
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データアウトフェーズの処理
	if (ctrl.phase == BUS::dataout) {
		if (ctrl.blocks == 0) {
			// このバッファで終了
			result = XferOut(FALSE);
		} else {
			// 次のバッファに続く(offset, lengthをセットすること)
			result = XferOut(TRUE);
		}
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら受信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// コマンドフェーズ
		case BUS::command:
			// 実行フェーズ
			Execute();
			break;

		// データアウトフェーズ
		case BUS::dataout:
			// フラッシュ
			FlushUnit();

			// ステータスフェーズ
			Status();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

#if USE_BURST_BUS == 1
//---------------------------------------------------------------------------
//
//	バースト送信
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::SendBurst()
{
	int len;
	BOOL result;

	ASSERT(this);
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(ctrl.bus->GetIO());

	// レングス!=0なら送信
	if (ctrl.length != 0) {
		len = ctrl.bus->SendHandShake(ctrl.buffer, ctrl.length);

		// 全て送信できなければステータスフェーズへ移行
		if (len != (int)ctrl.length) {
			Error();
			return;
		}

		// オフセットとレングス
		ctrl.offset += ctrl.length;
		ctrl.length = 0;
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データ引き取り後の処理(リード/データインのみ)
	if (ctrl.phase == BUS::datain) {
		if (ctrl.blocks != 0) {
			// 次のバッファを設定(offset, lengthをセットすること)
			result = XferIn(ctrl.buffer);
		}
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら送信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// メッセージインフェーズ
		case BUS::msgin:
			// バスフリーフェーズ
			BusFree();
			break;

		// データインフェーズ
		case BUS::datain:
			// ステータスフェーズ
			Status();
			break;

		// ステータスフェーズ
		case BUS::status:
			// メッセージインフェーズ
			ctrl.length = 1;
			ctrl.blocks = 1;
			ctrl.buffer[0] = (BYTE)ctrl.message;
			MsgIn();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	データ受信
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::ReceiveBurst()
{
	int len;
	BOOL result;

	ASSERT(this);

	// REQが下がっていること
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(!ctrl.bus->GetIO());

	// レングス!=0なら受信
	if (ctrl.length != 0) {
		// 受信
		len = ctrl.bus->ReceiveHandShake(ctrl.buffer, ctrl.length);

		// 全て受信できなければステータスフェーズへ移行
		if (len != (int)ctrl.length) {
			Error();
			return;
		}

		// オフセットとレングス
		ctrl.offset += ctrl.length;
		ctrl.length = 0;
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データアウトフェーズの処理
	if (ctrl.phase == BUS::dataout) {
		if (ctrl.blocks == 0) {
			// このバッファで終了
			result = XferOut(FALSE);
		} else {
			// 次のバッファに続く(offset, lengthをセットすること)
			result = XferOut(TRUE);
		}
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら受信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// データアウトフェーズ
		case BUS::dataout:
			// フラッシュ
			FlushUnit();

			// ステータスフェーズ
			Status();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}
#endif	// USE_BURST_BUS

//---------------------------------------------------------------------------
//
//	データ転送IN
//	※offset, lengthを再設定すること
//
//---------------------------------------------------------------------------
BOOL FASTCALL SASIDEV::XferIn(BYTE *buf)
{
	DWORD lun;

	ASSERT(this);
	ASSERT(ctrl.phase == BUS::datain);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		return FALSE;
	}

	// READ系コマンドに限る
	switch (ctrl.cmd[0]) {
		// READ(6)
		case 0x08:
		// READ(10)
		case 0x28:
			// ディスクから読み取りを行う
			ctrl.length = ctrl.unit[lun]->Read(buf, ctrl.next);
			ctrl.next++;

			// エラーなら、ステータスフェーズへ
			if (ctrl.length <= 0) {
				// データイン中止
				return FALSE;
			}

			// 正常なら、ワーク設定
			ctrl.offset = 0;
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			return FALSE;
	}

	// バッファ設定に成功した
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	データ転送OUT
//	※cont=TRUEの場合、offset, lengthを再設定すること
//
//---------------------------------------------------------------------------
BOOL FASTCALL SASIDEV::XferOut(BOOL cont)
{
	DWORD lun;
	SCSIBR *bridge;

	ASSERT(this);
	ASSERT(ctrl.phase == BUS::dataout);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		return FALSE;
	}

	// MODE SELECTまたは、WRITE系
	switch (ctrl.cmd[0]) {
		// MODE SELECT
		case 0x15:
		// MODE SELECT(10)
		case 0x55:
			if (!ctrl.unit[lun]->ModeSelect(
				ctrl.cmd, ctrl.buffer, ctrl.offset)) {
				// MODE SELECTに失敗
				return FALSE;
			}
			break;

		// WRITE(6)
		case 0x0a:
		// WRITE(10)
		case 0x2a:
			// ホストブリッジはSEND MESSAGE10に差し替える
			if (ctrl.unit[lun]->GetID() == MAKEID('S', 'C', 'B', 'R')) {
				bridge = (SCSIBR*)ctrl.unit[lun];
				if (bridge->SendMessage10(ctrl.cmd, ctrl.buffer) <= 0) {
					// メッセージ送信失敗
					return FALSE;
				}

				// 正常なら、ワーク設定
				ctrl.offset = 0;
				break;
			}

		// WRITE AND VERIFY
		case 0x2e:
			// 書き込みを行う
			if (!ctrl.unit[lun]->Write(ctrl.buffer, ctrl.next - 1)) {
				// 書き込み失敗
				return FALSE;
			}

			// 次のブロックが必要ないならここまで
			ctrl.next++;
			if (!cont) {
				break;
			}

			// 次のブロックをチェック
			ctrl.length = ctrl.unit[lun]->WriteCheck(ctrl.next - 1);
			if (ctrl.length <= 0) {
				// 書き込みできない
				return FALSE;
			}

			// 正常なら、ワーク設定
			ctrl.offset = 0;
			break;

		// SPECIFY(SASIのみ)
		case 0xc2:
			break;

		default:
			ASSERT(FALSE);
			break;
	}

	// バッファ保存に成功した
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	論理ユニットフラッシュ
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::FlushUnit()
{
	DWORD lun;

	ASSERT(this);
	ASSERT(ctrl.phase == BUS::dataout);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		return;
	}

	// WRITE系のみ
	switch (ctrl.cmd[0]) {
		// WRITE(6)
		case 0x0a:
		// WRITE(10)
		case 0x2a:
		// WRITE AND VERIFY
		case 0x2e:
			// フラッシュ
			if (!ctrl.unit[lun]->IsCacheWB()) {
				ctrl.unit[lun]->Flush();
			}
			break;
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	ログ出力
//
//---------------------------------------------------------------------------
void FASTCALL SASIDEV::Log(Log::loglevel level, const char *format, ...)
{
#if USE_LOG_OUTPUT > 0
	char buffer[0x200];
	va_list args;
	va_start(args, format);

#ifndef DISK_LOG_WARNING
	if (level == Log::Warning) {
		return;
	}
#endif	// DISK_LOG_WARNING

	// フォーマット
	vsprintf(buffer, format, args);

	// 可変長引数終了
	va_end(args);

	// ログ出力
	printf("%s\n", buffer);
#endif	// USE_LOG_OUTPUT
}

//===========================================================================
//
//	SCSI デバイス
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
SCSIDEV::SCSIDEV() : SASIDEV()
{
	// ワーク初期化
#if (USE_BURST_BUS == 1 && USE_SYNC_TRANS == 1)
	scsi.syncenable = TRUE;
#else
	scsi.syncenable = FALSE;
#endif	// USE_BURST_BUS == 1 && USE_SYNC_TRANS == 1
	scsi.syncperiod = 0;
	scsi.syncoffset = 0;
	scsi.atnmsg = FALSE;
	scsi.msc = 0;
	memset(scsi.msb, 0x00, sizeof(scsi.msb));
}

//---------------------------------------------------------------------------
//
//	デバイスリセット
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::Reset()
{
	ASSERT(this);

	// ワーク初期化
#if (USE_BURST_BUS == 1 && USE_SYNC_TRANS == 1)
	scsi.syncenable = TRUE;
#else
	scsi.syncenable = FALSE;
#endif	// USE_BURST_BUS == 1 && USE_SYNC_TRANS == 1
	scsi.syncperiod = 0;
	scsi.syncoffset = 0;
	scsi.atnmsg = FALSE;
	scsi.msc = 0;
	memset(scsi.msb, 0x00, sizeof(scsi.msb));

	// 基底クラス
	SASIDEV::Reset();
}

//---------------------------------------------------------------------------
//
//	実行
//
//---------------------------------------------------------------------------
BUS::phase_t FASTCALL SCSIDEV::Process()
{
	ASSERT(this);

	// 未接続なら何もしない
	if (ctrl.id < 0 || ctrl.bus == NULL) {
		return ctrl.phase;
	}

	// バス情報の取り込み
	ctrl.bus->Aquire();

	// リセット
	if (ctrl.bus->GetRST()) {
#if defined(DISK_LOG)
		Log(Log::Normal, "RESET信号受信");
#endif	// DISK_LOG

		// コントローラをリセット
		Reset();

		// バスもリセット
		ctrl.bus->Reset();
		return ctrl.phase;
	}

	// フェーズ処理
	switch (ctrl.phase) {
		// バスフリー
		case BUS::busfree:
			BusFree();
			break;

		// セレクションフェーズ
		case BUS::selection:
			Selection();
			break;

		// データアウト(MCI=000)
		case BUS::dataout:
			DataOut();
			break;

		// データイン(MCI=001)
		case BUS::datain:
			DataIn();
			break;

		// コマンド(MCI=010)
		case BUS::command:
			Command();
			break;

		// ステータス(MCI=011)
		case BUS::status:
			Status();
			break;

		// メッセージアウト(MCI=110)
		case BUS::msgout:
			MsgOut();
			break;

		// メッセージイン(MCI=111)
		case BUS::msgin:
			MsgIn();
			break;

		// その他
		default:
			ASSERT(FALSE);
			break;
	}

	return ctrl.phase;
}

//---------------------------------------------------------------------------
//
//	フェーズ
//
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//
//	バスフリーフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::BusFree()
{
	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::busfree) {

#if defined(DISK_LOG)
		Log(Log::Normal, "バスフリーフェーズ");
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::busfree;

		// 信号線
		ctrl.bus->SetREQ(FALSE);
		ctrl.bus->SetMSG(FALSE);
		ctrl.bus->SetCD(FALSE);
		ctrl.bus->SetIO(FALSE);
		ctrl.bus->SetBSY(FALSE);

		// ステータスとメッセージを初期化
		ctrl.status = 0x00;
		ctrl.message = 0x00;

		// ATNメッセージ受信スタータス初期化
		scsi.atnmsg = FALSE;
		return;
	}

	// セレクションフェーズに移行
	if (ctrl.bus->GetSEL() && !ctrl.bus->GetBSY()) {
		Selection();
	}
}

//---------------------------------------------------------------------------
//
//	セレクションフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::Selection()
{
	DWORD id;

	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::selection) {
		// IDが一致していなければ無効
		id = 1 << ctrl.id;
		ctrl.bus->Aquire();
		if ((ctrl.bus->GetDAT() & id) == 0) {
			return;
		}

		// 有効なユニットが無ければ終了
		if (!HasUnit()) {
			return;
		}

#if defined(DISK_LOG)
		Log(Log::Normal,
			"セレクションフェーズ ID=%d (デバイスあり)", ctrl.id);
#endif	// DISK_LOG

		// フェーズ設定
		ctrl.phase = BUS::selection;

		// BSYを上げて応答
		ctrl.bus->SetBSY(TRUE);
		return;
	}

	// セレクション完了
	if (!ctrl.bus->GetSEL() && ctrl.bus->GetBSY()) {
		// ATN=1ならメッセージアウトフェーズ、そうでなければコマンドフェーズ
		if (ctrl.bus->GetATN()) {
			MsgOut();
		} else {
			Command();
		}
	}
}

//---------------------------------------------------------------------------
//
//	実行フェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::Execute()
{
	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "実行フェーズ コマンド$%02X", ctrl.cmd[0]);
#endif	// DISK_LOG

	// フェーズ設定
	ctrl.phase = BUS::execute;

	// データ転送のための初期化
	ctrl.offset = 0;
	ctrl.blocks = 1;
#if USE_WAIT_CTRL == 1
	ctrl.execstart = ::GetTimeUs();
#endif	// USE_WAIT_CTRL

	// コマンド別処理
	switch (ctrl.cmd[0]) {
		// TEST UNIT READY
		case 0x00:
			CmdTestUnitReady();
			return;

		// REZERO
		case 0x01:
			CmdRezero();
			return;

		// REQUEST SENSE
		case 0x03:
			CmdRequestSense();
			return;

		// FORMAT UNIT
		case 0x04:
			CmdFormat();
			return;

		// REASSIGN BLOCKS
		case 0x07:
			CmdReassign();
			return;

		// READ(6)
		case 0x08:
			CmdRead6();
			return;

		// WRITE(6)
		case 0x0a:
			CmdWrite6();
			return;

		// SEEK(6)
		case 0x0b:
			CmdSeek6();
			return;

		// INQUIRY
		case 0x12:
			CmdInquiry();
			return;

		// MODE SELECT
		case 0x15:
			CmdModeSelect();
			return;

		// MDOE SENSE
		case 0x1a:
			CmdModeSense();
			return;

		// START STOP UNIT
		case 0x1b:
			CmdStartStop();
			return;

		// SEND DIAGNOSTIC
		case 0x1d:
			CmdSendDiag();
			return;

		// PREVENT/ALLOW MEDIUM REMOVAL
		case 0x1e:
			CmdRemoval();
			return;

		// READ CAPACITY
		case 0x25:
			CmdReadCapacity();
			return;

		// READ(10)
		case 0x28:
			CmdRead10();
			return;

		// WRITE(10)
		case 0x2a:
			CmdWrite10();
			return;

		// SEEK(10)
		case 0x2b:
			CmdSeek10();
			return;

		// WRITE and VERIFY
		case 0x2e:
			CmdWrite10();
			return;

		// VERIFY
		case 0x2f:
			CmdVerify();
			return;

		// SYNCHRONIZE CACHE
		case 0x35:
			CmdSynchronizeCache();
			return;

		// READ DEFECT DATA(10)
		case 0x37:
			CmdReadDefectData10();
			return;

		// READ TOC
		case 0x43:
			CmdReadToc();
			return;

		// PLAY AUDIO(10)
		case 0x45:
			CmdPlayAudio10();
			return;

		// PLAY AUDIO MSF
		case 0x47:
			CmdPlayAudioMSF();
			return;

		// PLAY AUDIO TRACK
		case 0x48:
			CmdPlayAudioTrack();
			return;

		// MODE SELECT(10)
		case 0x55:
			CmdModeSelect10();
			return;

		// MDOE SENSE(10)
		case 0x5a:
			CmdModeSense10();
			return;

		// SPECIFY(SASIのみ/SxSI利用時の警告抑制)
		case 0xc2:
			CmdInvalid();
			return;
	}

	// それ以外は対応していない
	Log(Log::Normal, "未対応コマンド $%02X", ctrl.cmd[0]);
	CmdInvalid();
}

//---------------------------------------------------------------------------
//
//	メッセージアウトフェーズ
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::MsgOut()
{
	ASSERT(this);

	// フェーズチェンジ
	if (ctrl.phase != BUS::msgout) {

#if defined(DISK_LOG)
		Log(Log::Normal, "メッセージアウトフェーズ");
#endif	// DISK_LOG

		// セレクション後のメッセージアウトフェーズは
		// IDENTIFYメッセージの処理
		if (ctrl.phase == BUS::selection) {
			scsi.atnmsg = TRUE;
			scsi.msc = 0;
			memset(scsi.msb, 0x00, sizeof(scsi.msb));
		}

		// フェーズ設定
		ctrl.phase = BUS::msgout;

		// ターゲットが操作する信号線
		ctrl.bus->SetMSG(TRUE);
		ctrl.bus->SetCD(TRUE);
		ctrl.bus->SetIO(FALSE);

		// データ転送は1バイトx1ブロック
		ctrl.offset = 0;
		ctrl.length = 1;
		ctrl.blocks = 1;

#if USE_BURST_BUS == 0
		// メッセージを要求
		ctrl.bus->SetREQ(TRUE);
		return;
#endif	// USE_BURST_BUS
	}

#if USE_BURST_BUS == 1
	// 受信
	ReceiveBurst();
#else
	// リクエスト中
	if (ctrl.bus->GetREQ()) {
		// イニシエータが送信した
		if (ctrl.bus->GetACK()) {
			Receive();
		}
	} else {
		// イニシエータに次を要求
		if (!ctrl.bus->GetACK()) {
			ReceiveNext();
		}
	}
#endif	// USE_BURST_BUS
}

//---------------------------------------------------------------------------
//
//	共通エラー処理
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::Error()
{
	ASSERT(this);

	// バス情報の取り込み
	ctrl.bus->Aquire();

	// リセットチェック
	if (ctrl.bus->GetRST()) {
		// コントローラをリセット
		Reset();

		// バスもリセット
		ctrl.bus->Reset();
		return;
	}

	// ステータスフェーズ、メッセージインフェーズはバスフリー
	if (ctrl.phase == BUS::status || ctrl.phase == BUS::msgin) {
		BusFree();
		return;
	}

#if defined(DISK_LOG)
	Log(Log::Normal, "エラー(ステータスフェーズへ)");
#endif	// DISK_LOG

	// ステータスとメッセージを設定(CHECK CONDITION)
	ctrl.status = 0x02;
	ctrl.message = 0x00;

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	コマンド
//
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdInquiry()
{
	Disk *disk;
	int lun;
	DWORD major;
	DWORD minor;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "INQUIRYコマンド");
#endif	// DISK_LOG

	// 有効なユニットを探す
	disk = NULL;
	for (lun = 0; lun < UnitMax; lun++) {
		if (ctrl.unit[lun]) {
			disk = ctrl.unit[lun];
			break;
		}
	}

	// ディスク側で処理(本来はコントローラで処理される)
	if (disk) {
		major = (DWORD)(DEFAULT_VERSION >> 8);
		minor = (DWORD)(DEFAULT_VERSION & 0xff);
		ctrl.length =
			ctrl.unit[lun]->Inquiry(ctrl.cmd, ctrl.buffer, major, minor);
	} else {
		ctrl.length = 0;
	}

	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 同期転送サポート情報の追加
	if (scsi.syncenable) {
		ctrl.buffer[7] |= (1 << 4);
	}

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdModeSelect()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "MODE SELECTコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->SelectCheck(ctrl.cmd);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// データアウトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	MODE SENSE
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdModeSense()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "MODE SENSEコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->ModeSense(ctrl.cmd, ctrl.buffer);
	ASSERT(ctrl.length >= 0);
	if (ctrl.length == 0) {
		Log(Log::Warning,
			"サポートしていないMODE SENSEページ $%02X", ctrl.cmd[2]);

		// 失敗(エラー)
		Error();
		return;
	}

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	START STOP UNIT
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdStartStop()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "START STOP UNITコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->StartStop(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	SEND DIAGNOSTIC
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdSendDiag()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "SEND DIAGNOSTICコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->SendDiag(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	PREVENT/ALLOW MEDIUM REMOVAL
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdRemoval()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "PREVENT/ALLOW MEDIUM REMOVALコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Removal(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	READ CAPACITY
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdReadCapacity()
{
	DWORD lun;
	int length;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "READ CAPACITYコマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	length = ctrl.unit[lun]->ReadCapacity(ctrl.cmd, ctrl.buffer);
	ASSERT(length >= 0);
	if (length <= 0) {
		Error();
		return;
	}

	// レングス設定
	ctrl.length = length;

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	READ(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdRead10()
{
	DWORD lun;
	DWORD record;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ホストブリッジならメッセージ受信
	if (ctrl.unit[lun]->GetID() == MAKEID('S', 'C', 'B', 'R')) {
		CmdGetMessage10();
		return;
	}

	// レコード番号とブロック数を取得
	record = ctrl.cmd[2];
	record <<= 8;
	record |= ctrl.cmd[3];
	record <<= 8;
	record |= ctrl.cmd[4];
	record <<= 8;
	record |= ctrl.cmd[5];
	ctrl.blocks = ctrl.cmd[7];
	ctrl.blocks <<= 8;
	ctrl.blocks |= ctrl.cmd[8];

#if defined(DISK_LOG)
	Log(Log::Normal, "READ(10)コマンド レコード=%08X ブロック=%d", record, ctrl.blocks);
#endif	// DISK_LOG

	// ブロック数0は処理しない
	if (ctrl.blocks == 0) {
		Status();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->Read(ctrl.buffer, record);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.next = record + 1;

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	WRITE(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdWrite10()
{
	DWORD lun;
	DWORD record;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ホストブリッジならメッセージ受信
	if (ctrl.unit[lun]->GetID() == MAKEID('S', 'C', 'B', 'R')) {
		CmdSendMessage10();
		return;
	}

	// レコード番号とブロック数を取得
	record = ctrl.cmd[2];
	record <<= 8;
	record |= ctrl.cmd[3];
	record <<= 8;
	record |= ctrl.cmd[4];
	record <<= 8;
	record |= ctrl.cmd[5];
	ctrl.blocks = ctrl.cmd[7];
	ctrl.blocks <<= 8;
	ctrl.blocks |= ctrl.cmd[8];

#if defined(DISK_LOG)
	Log(Log::Normal,
		"WRTIE(10)コマンド レコード=%08X ブロック=%d", record, ctrl.blocks);
#endif	// DISK_LOG

	// ブロック数0は処理しない
	if (ctrl.blocks == 0) {
		Status();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->WriteCheck(record);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.next = record + 1;

	// データアウトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	SEEK(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdSeek10()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "SEEK(10)コマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->Seek(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	VERIFY
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdVerify()
{
	DWORD lun;
	BOOL status;
	DWORD record;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// レコード番号とブロック数を取得
	record = ctrl.cmd[2];
	record <<= 8;
	record |= ctrl.cmd[3];
	record <<= 8;
	record |= ctrl.cmd[4];
	record <<= 8;
	record |= ctrl.cmd[5];
	ctrl.blocks = ctrl.cmd[7];
	ctrl.blocks <<= 8;
	ctrl.blocks |= ctrl.cmd[8];

#if defined(DISK_LOG)
	Log(Log::Normal,
		"VERIFYコマンド レコード=%08X ブロック=%d", record, ctrl.blocks);
#endif	// DISK_LOG

	// ブロック数0は処理しない
	if (ctrl.blocks == 0) {
		Status();
		return;
	}

	// BytChk=0なら
	if ((ctrl.cmd[1] & 0x02) == 0) {
		// ドライブでコマンド処理
		status = ctrl.unit[lun]->Seek(ctrl.cmd);
		if (!status) {
			// 失敗(エラー)
			Error();
			return;
		}

		// ステータスフェーズ
		Status();
		return;
	}

	// テスト読み込み
	ctrl.length = ctrl.unit[lun]->Read(ctrl.buffer, record);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.next = record + 1;

	// データアウトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	SYNCHRONIZE CACHE
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdSynchronizeCache()
{
	DWORD lun;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// 成功したことにする(未実装)

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	READ DEFECT DATA(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdReadDefectData10()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "READ DEFECT DATA(10)コマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->ReadDefectData10(ctrl.cmd, ctrl.buffer);
	ASSERT(ctrl.length >= 0);

	if (ctrl.length <= 4) {
		Error();
		return;
	}

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	READ TOC
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdReadToc()
{
	DWORD lun;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->ReadToc(ctrl.cmd, ctrl.buffer);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdPlayAudio10()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->PlayAudio(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO MSF
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdPlayAudioMSF()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->PlayAudioMSF(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO TRACK
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdPlayAudioTrack()
{
	DWORD lun;
	BOOL status;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	status = ctrl.unit[lun]->PlayAudioTrack(ctrl.cmd);
	if (!status) {
		// 失敗(エラー)
		Error();
		return;
	}

	// ステータスフェーズ
	Status();
}

//---------------------------------------------------------------------------
//
//	MODE SELECT10
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdModeSelect10()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "MODE SELECT10コマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->SelectCheck10(ctrl.cmd);
	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// データアウトフェーズ
	DataOut();
}

//---------------------------------------------------------------------------
//
//	MODE SENSE(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdModeSense10()
{
	DWORD lun;

	ASSERT(this);

#if defined(DISK_LOG)
	Log(Log::Normal, "MODE SENSE(10)コマンド");
#endif	// DISK_LOG

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ドライブでコマンド処理
	ctrl.length = ctrl.unit[lun]->ModeSense10(ctrl.cmd, ctrl.buffer);
	ASSERT(ctrl.length >= 0);
	if (ctrl.length == 0) {
		Log(Log::Warning,
			"サポートしていないMODE SENSE(10)ページ $%02X", ctrl.cmd[2]);

		// 失敗(エラー)
		Error();
		return;
	}

	// データインフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	GET MESSAGE(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdGetMessage10()
{
	DWORD lun;
	SCSIBR *bridge;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ホストブリッジでないならエラー
	if (ctrl.unit[lun]->GetID() != MAKEID('S', 'C', 'B', 'R')) {
		Error();
		return;
	}

	// バッファの再確保(ブロック毎の転送ではないため)
	if (ctrl.bufsize < 0x2000000) {
		free(ctrl.buffer);
		ctrl.bufsize = 0x2000000;
		ctrl.buffer = (BYTE *)malloc(ctrl.bufsize);
	}

	// ドライブで処理する
	bridge = (SCSIBR*)ctrl.unit[lun];
	ctrl.length = bridge->GetMessage10(ctrl.cmd, ctrl.buffer);

	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.blocks = 1;
	ctrl.next = 1;

	// リードフェーズ
	DataIn();
}

//---------------------------------------------------------------------------
//
//	SEND MESSAGE(10)
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::CmdSendMessage10()
{
	DWORD lun;

	ASSERT(this);

	// 論理ユニット
	lun = (ctrl.cmd[1] >> 5) & 0x07;
	if (!ctrl.unit[lun]) {
		Error();
		return;
	}

	// ホストブリッジでないならエラー
	if (ctrl.unit[lun]->GetID() != MAKEID('S', 'C', 'B', 'R')) {
		Error();
		return;
	}

	// バッファの再確保(ブロック毎の転送ではないため)
	if (ctrl.bufsize < 0x2000000) {
		free(ctrl.buffer);
		ctrl.bufsize = 0x2000000;
		ctrl.buffer = (BYTE *)malloc(ctrl.bufsize);
	}

	// 転送量を設定
	ctrl.length = ctrl.cmd[6];
	ctrl.length <<= 8;
	ctrl.length |= ctrl.cmd[7];
	ctrl.length <<= 8;
	ctrl.length |= ctrl.cmd[8];

	if (ctrl.length <= 0) {
		// 失敗(エラー)
		Error();
		return;
	}

	// 次のブロックを設定
	ctrl.blocks = 1;
	ctrl.next = 1;

	// ライトフェーズ
	DataOut();
}

//===========================================================================
//
//	データ転送
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	データ送信
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::Send()
{
	BOOL result;

	ASSERT(this);
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(ctrl.bus->GetIO());

	// オフセットとレングス
	ASSERT(ctrl.length >= 1);
	ctrl.offset++;
	ctrl.length--;

	// ACKアサート直後にSendNextでデータ設定済みならリクエストを上げる
	if (ctrl.length != 0) {
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データ引き取り後の処理(リード/データインのみ)
	if (ctrl.phase == BUS::datain) {
		if (ctrl.blocks != 0) {
			// 次のバッファを設定(offset, lengthをセットすること)
			result = XferIn(ctrl.buffer);
			ctrl.bus->SetDAT(ctrl.buffer[ctrl.offset]);
		}
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら送信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// メッセージインフェーズ
		case BUS::msgin:
			// IDENTIFYメッセージの拡張メッセージに対する応答送信完了
			if (scsi.atnmsg) {
				// フラグオフ
				scsi.atnmsg = FALSE;

				// コマンドフェーズ
				Command();
			} else {
				// バスフリーフェーズ
				BusFree();
			}
			break;

		// データインフェーズ
		case BUS::datain:
			// ステータスフェーズ
			Status();
			break;

		// ステータスフェーズ
		case BUS::status:
			// メッセージインフェーズ
			ctrl.length = 1;
			ctrl.blocks = 1;
			ctrl.buffer[0] = (BYTE)ctrl.message;
			MsgIn();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	データ送信継続
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::SendNext()
{
	ASSERT(this);

	// REQが上がっている
	ASSERT(ctrl.bus->GetREQ());
	ASSERT(ctrl.bus->GetIO());

	// ターゲットが操作する信号線
	ctrl.bus->SetREQ(FALSE);

	// バッファにデータがあれば先に設定する
	if (ctrl.length > 1) {
		ctrl.bus->SetDAT(ctrl.buffer[ctrl.offset + 1]);
	}
}

//---------------------------------------------------------------------------
//
//	データ受信
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::Receive()
{
	DWORD data;

	ASSERT(this);

	// REQが上がっている
	ASSERT(ctrl.bus->GetREQ());
	ASSERT(!ctrl.bus->GetIO());

	// データ取得
	ctrl.bus->Aquire();
	data = (DWORD)ctrl.bus->GetDAT();

	// ターゲットが操作する信号線
	ctrl.bus->SetREQ(FALSE);

	switch (ctrl.phase) {
		// コマンドフェーズ
		case BUS::command:
			ctrl.cmd[ctrl.offset] = data;
#if defined(DISK_LOG)
			Log(Log::Normal, "コマンドフェーズ $%02X", data);
#endif	// DISK_LOG

			// 最初のデータ(オフセット0)によりレングスを再設定
			if (ctrl.offset == 0) {
				if (ctrl.cmd[0] >= 0x20) {
					// 10バイトCDB
					ctrl.length = 10;
				}
			}
			break;

		// メッセージアウトフェーズ
		case BUS::msgout:
			ctrl.message = data;
#if defined(DISK_LOG)
			Log(Log::Normal, "メッセージアウトフェーズ $%02X", data);
#endif	// DISK_LOG
			break;

		// データアウトフェーズ
		case BUS::dataout:
			ctrl.buffer[ctrl.offset] = (BYTE)data;
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	データ受信継続
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::ReceiveNext()
{
	BOOL result;
	int i;
	BYTE data;

	ASSERT(this);

	// REQが下がっていること
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(!ctrl.bus->GetIO());

	// オフセットとレングス
	ASSERT(ctrl.length >= 1);
	ctrl.offset++;
	ctrl.length--;

	// レングス!=0なら、再びreqをセット
	if (ctrl.length != 0) {
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データ受理後の処理(フェーズ別)
	switch (ctrl.phase) {

		// データアウトフェーズ
		case BUS::dataout:
			if (ctrl.blocks == 0) {
				// このバッファで終了
				result = XferOut(FALSE);
			} else {
				// 次のバッファに続く(offset, lengthをセットすること)
				result = XferOut(TRUE);
			}
			break;

		// メッセージアウトフェーズ
		case BUS::msgout:
			ctrl.message = ctrl.buffer[0];
			if (!XferMsg(ctrl.message)) {
				// メッセージアウトに失敗したら、即座にバスフリー
				BusFree();
				return;
			}

			// メッセージインに備え、メッセージデータをクリアしておく
			ctrl.message = 0x00;
			break;

		default:
			break;
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら受信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		// ターゲットが操作する信号線
		ctrl.bus->SetREQ(TRUE);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// コマンドフェーズ
		case BUS::command:
			// 実行フェーズ
			Execute();
			break;

		// メッセージアウトフェーズ
		case BUS::msgout:
			// ATNがアサートし続ける限りメッセージアウトフェーズを継続
			if (ctrl.bus->GetATN()) {
				// データ転送は1バイトx1ブロック
				ctrl.offset = 0;
				ctrl.length = 1;
				ctrl.blocks = 1;
				// メッセージを要求
				ctrl.bus->SetREQ(TRUE);
				return;
			}

			// ATNで送信されたメッセージの解析
			if (scsi.atnmsg) {
				i = 0;
				while (i < scsi.msc) {
					// メッセージの種類
					data = scsi.msb[i];

					// ABORT
					if (data == 0x06) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード ABORT $%02X", data);
#endif	// DISK_LOG
						// バスフリー
						BusFree();
						return;
					}

					// BUS DEVICE RESET
					if (data == 0x0C) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード BUS DEVICE RESET $%02X", data);
#endif	// DISK_LOG
						// リセット
						Reset();

						// その後バスフリー
						BusFree();
						return;
					}

					// IDENTIFY
					if (data >= 0x80) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード IDENTIFY $%02X", data);
#endif	// DISK_LOG
					}

					// 拡張メッセージ
					if (data == 0x01) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード EXTENDED MESSAGE $%02X", data);
#endif	// DISK_LOG

						// 同期転送が可能な時だけチェック
						if (!scsi.syncenable || scsi.msb[i + 2] != 0x01) {
							// MESSAGE REJECT
							ctrl.length = 1;
							ctrl.blocks = 1;
							ctrl.buffer[0] = 0x07;
							MsgIn();
							return;
						}

						// Transfer period factor(SYNCPERIOD x 4ns制限)
						scsi.syncperiod = scsi.msb[i + 3];
						if (scsi.syncperiod != 0 && scsi.syncperiod < SYNCPERIOD) {
							scsi.syncperiod = SYNCPERIOD;
						}

						// REQ/ACK offset(16に制限)
						scsi.syncoffset = scsi.msb[i + 4];
						if (scsi.syncoffset > SYNCOFFSET) {
							scsi.syncoffset = SYNCOFFSET;
						}

						// SYNCHRONOUS DATA TRANSFER REQUESTメッセージ生成
						ctrl.length = 5;
						ctrl.blocks = 1;
						ctrl.buffer[0] = 0x01;					// 拡張メッセージ
						ctrl.buffer[1] = 0x03;					// 拡張メッセージ長
						ctrl.buffer[2] = 0x01;					// SDTRコード
						ctrl.buffer[3] = (BYTE)scsi.syncperiod;	// 転送期間係数
						ctrl.buffer[4] = (BYTE)scsi.syncoffset;	// REQ/ACKオフセット
						MsgIn();
						return;
					}

					// 次へ
					i++;
				}
			}

			// ATNメッセージ受信スタータス初期化
			scsi.atnmsg = FALSE;

			// コマンドフェーズ
			Command();
			break;

		// データアウトフェーズ
		case BUS::dataout:
			// フラッシュ
			FlushUnit();

			// ステータスフェーズ
			Status();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

#if USE_BURST_BUS == 1
//---------------------------------------------------------------------------
//
//	データ送信
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::SendBurst()
{
	int len;
	BOOL result;

	ASSERT(this);
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(ctrl.bus->GetIO());

	// レングス!=0なら送信
	if (ctrl.length != 0) {
		// バースト送信
		if (ctrl.phase == BUS::datain && scsi.syncoffset > 0) {
			len = ctrl.bus->SendHandShake(
				ctrl.buffer, ctrl.length, scsi.syncoffset);
		} else {
			len = ctrl.bus->SendHandShake(ctrl.buffer, ctrl.length);
		}

		// 全て送信できなければステータスフェーズへ移行
		if (len != (int)ctrl.length) {
			Error();
			return;
		}

		// オフセットとレングス
		ctrl.offset += ctrl.length;
		ctrl.length = 0;
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データ引き取り後の処理(リード/データインのみ)
	if (ctrl.phase == BUS::datain) {
		if (ctrl.blocks != 0) {
			// 次のバッファを設定(offset, lengthをセットすること)
			result = XferIn(ctrl.buffer);
		}
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら送信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// メッセージインフェーズ
		case BUS::msgin:
			// IDENTIFYメッセージの拡張メッセージに対する応答送信完了
			if (scsi.atnmsg) {
				// フラグオフ
				scsi.atnmsg = FALSE;

				// コマンドフェーズ
				Command();
			} else {
				// バスフリーフェーズ
				BusFree();
			}
			break;

		// データインフェーズ
		case BUS::datain:
			// ステータスフェーズ
			Status();
			break;

		// ステータスフェーズ
		case BUS::status:
			// メッセージインフェーズ
			ctrl.length = 1;
			ctrl.blocks = 1;
			ctrl.buffer[0] = (BYTE)ctrl.message;
			MsgIn();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}

//---------------------------------------------------------------------------
//
//	データ受信
//
//---------------------------------------------------------------------------
void FASTCALL SCSIDEV::ReceiveBurst()
{
	int len;
	BOOL result;
	int i;
	BYTE data;

	ASSERT(this);

	// REQが下がっていること
	ASSERT(!ctrl.bus->GetREQ());
	ASSERT(!ctrl.bus->GetIO());

	// レングス!=0なら受信
	if (ctrl.length != 0) {
		// バースト受信
		if (ctrl.phase == BUS::dataout && scsi.syncoffset > 0) {
			len = ctrl.bus->ReceiveHandShake(
				ctrl.buffer, ctrl.length, scsi.syncoffset);
		} else {
			len = ctrl.bus->ReceiveHandShake(ctrl.buffer, ctrl.length);
		}

		// 全て受信できなければステータスフェーズへ移行
		if (len != (int)ctrl.length) {
			Error();
			return;
		}

		// オフセットとレングス
		ctrl.offset += ctrl.length;
		ctrl.length = 0;;
		return;
	}

	// ブロック減算、リザルト初期化
	ctrl.blocks--;
	result = TRUE;

	// データ受理後の処理(フェーズ別)
	switch (ctrl.phase) {

		// データアウトフェーズ
		case BUS::dataout:
			if (ctrl.blocks == 0) {
				// このバッファで終了
				result = XferOut(FALSE);
			} else {
				// 次のバッファに続く(offset, lengthをセットすること)
				result = XferOut(TRUE);
			}
			break;

		// メッセージアウトフェーズ
		case BUS::msgout:
			ctrl.message = ctrl.buffer[0];
			if (!XferMsg(ctrl.message)) {
				// メッセージアウトに失敗したら、即座にバスフリー
				BusFree();
				return;
			}

			// メッセージインに備え、メッセージデータをクリアしておく
			ctrl.message = 0x00;
			break;

		default:
			break;
	}

	// リザルトFALSEなら、ステータスフェーズへ移行
	if (!result) {
		Error();
		return;
	}

	// ブロック!=0なら受信継続
	if (ctrl.blocks != 0){
		ASSERT(ctrl.length > 0);
		ASSERT(ctrl.offset == 0);
		return;
	}

	// 次フェーズに移動
	switch (ctrl.phase) {
		// コマンドフェーズ
		case BUS::command:
			// コマンドデータ転送
			len = 6;
			if (ctrl.buffer[0] >= 0x20 && ctrl.buffer[0] <= 0x7D) {
				// 10バイトCDB
				len = 10;
			}
			for (i = 0; i < len; i++) {
				ctrl.cmd[i] = (DWORD)ctrl.buffer[i];
#if defined(DISK_LOG)
				Log(Log::Normal, "コマンド $%02X", ctrl.cmd[i]);
#endif	// DISK_LOG
			}

			// 実行フェーズ
			Execute();
			break;

		// メッセージアウトフェーズ
		case BUS::msgout:
			// ATNがアサートし続ける限りメッセージアウトフェーズを継続
			if (ctrl.bus->GetATN()) {
				// データ転送は1バイトx1ブロック
				ctrl.offset = 0;
				ctrl.length = 1;
				ctrl.blocks = 1;
				return;
			}

			// ATNで送信されたメッセージの解析
			if (scsi.atnmsg) {
				i = 0;
				while (i < scsi.msc) {
					// メッセージの種類
					data = scsi.msb[i];

					// ABORT
					if (data == 0x06) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード ABORT $%02X", data);
#endif	// DISK_LOG
						// バスフリー
						BusFree();
						return;
					}

					// BUS DEVICE RESET
					if (data == 0x0C) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード BUS DEVICE RESET $%02X", data);
#endif	// DISK_LOG
						// リセット
						Reset();

						// その後バスフリー
						BusFree();
						return;
					}

					// IDENTIFY
					if (data >= 0x80) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード IDENTIFY $%02X", data);
#endif	// DISK_LOG
					}

					// 拡張メッセージ
					if (data == 0x01) {
#if defined(DISK_LOG)
						Log(Log::Normal,
							"メッセージコード EXTENDED MESSAGE $%02X", data);
#endif	// DISK_LOG

						// 同期転送が可能な時だけチェック
						if (!scsi.syncenable || scsi.msb[i + 2] != 0x01) {
							// MESSAGE REJECT
							ctrl.length = 1;
							ctrl.blocks = 1;
							ctrl.buffer[0] = 0x07;
							MsgIn();
							return;
						}

						// Transfer period factor(SYNCPERIOD x 4ns制限)
						scsi.syncperiod = scsi.msb[i + 3];
						if (scsi.syncperiod != 0 && scsi.syncperiod < SYNCPERIOD) {
							scsi.syncperiod = SYNCPERIOD;
						}

						// REQ/ACK offset(16に制限)
						scsi.syncoffset = scsi.msb[i + 4];
						if (scsi.syncoffset > SYNCOFFSET) {
							scsi.syncoffset = SYNCOFFSET;
						}

						// SYNCHRONOUS DATA TRANSFER REQUESTメッセージ生成
						ctrl.length = 5;
						ctrl.blocks = 1;
						ctrl.buffer[0] = 0x01;					// 拡張メッセージ
						ctrl.buffer[1] = 0x03;					// 拡張メッセージ長
						ctrl.buffer[2] = 0x01;					// SDTRコード
						ctrl.buffer[3] = (BYTE)scsi.syncperiod;	// 転送期間係数
						ctrl.buffer[4] = (BYTE)scsi.syncoffset;	// REQ/ACKオフセット
						MsgIn();
						return;
					}

					// 次へ
					i++;
				}
			}

			// ATNメッセージ受信スタータス初期化
			scsi.atnmsg = FALSE;

			// コマンドフェーズ
			Command();
			break;

		// データアウトフェーズ
		case BUS::dataout:
			// フラッシュ
			FlushUnit();

			// ステータスフェーズ
			Status();
			break;

		// その他(ありえない)
		default:
			ASSERT(FALSE);
			break;
	}
}
#endif	// USE_BURST_BUS

//---------------------------------------------------------------------------
//
//	データ転送MSG
//
//---------------------------------------------------------------------------
BOOL FASTCALL SCSIDEV::XferMsg(DWORD msg)
{
	ASSERT(this);
	ASSERT(ctrl.phase == BUS::msgout);

	// メッセージアウトデータの保存
	if (scsi.atnmsg) {
		scsi.msb[scsi.msc] = (BYTE)msg;
		scsi.msc++;
		scsi.msc %= 256;
	}

	return TRUE;
}
